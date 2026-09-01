#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <algorithm>
#include <filesystem>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>
#include <lgpio.h>

const std::string CONFIG_FILE = "/etc/pi-relay-control.conf";
const std::string STATE_DIR   = "/var/lib/relay_control";
const int DEFAULT_GPIO_PIN    = 5;     // BCM 5 = IO21 on HAT
const int DEFAULT_PORT        = 7778;

struct Relay {
    int gpioPin;
    int port;
    std::string stateFile;
    bool alwaysOn = false;
};

int gpioHandle = -1;
std::vector<Relay> g_relays;

// ── Config ───────────────────────────────────────────────────────────────────

// Each relay is one line: "relay <gpio_pin> <port> [always_on]".
// Multiple lines configure multiple independently-controlled relays
// sharing the same gpiochip, each with its own TCP port and persisted
// state file. If no "relay" lines are present, a single relay is
// configured from the built-in defaults.
//
// "always_on" forces that relay ON every time this daemon starts,
// ignoring whatever state was last persisted -- for a relay that
// should come up energized whenever the Pi (and this service with it)
// boots, rather than resuming whatever position a client last left it in.
void loadConfig() {
    std::ifstream file(CONFIG_FILE);
    if (!file.is_open()) {
        std::cout << "No config at " << CONFIG_FILE << ", using defaults." << std::endl;
    } else {
        std::string line;
        while (std::getline(file, line)) {
            // Strip comments and trim whitespace
            auto comment = line.find('#');
            if (comment != std::string::npos)
                line = line.substr(0, comment);
            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string key;
            if (!(iss >> key)) continue;

            if (key == "relay") {
                Relay r;
                if (!(iss >> r.gpioPin >> r.port)) {
                    std::cerr << "Malformed relay line, expected: relay <gpio_pin> <port> [always_on]" << std::endl;
                    continue;
                }
                std::string opt;
                while (iss >> opt) {
                    if (opt == "always_on") {
                        r.alwaysOn = true;
                    } else {
                        std::cerr << "Unknown relay option \"" << opt << "\" for GPIO " << r.gpioPin << std::endl;
                    }
                }
                r.stateFile = STATE_DIR + "/state_pin" + std::to_string(r.gpioPin);
                std::cout << "Config: relay gpio_pin=" << r.gpioPin << " port=" << r.port
                          << (r.alwaysOn ? " always_on=true" : "") << std::endl;
                g_relays.push_back(r);
            }
        }
        file.close();
    }

    if (g_relays.empty()) {
        g_relays.push_back({DEFAULT_GPIO_PIN, DEFAULT_PORT,
                             STATE_DIR + "/state_pin" + std::to_string(DEFAULT_GPIO_PIN)});
    }
}

// ── State persistence ────────────────────────────────────────────────────────

void saveState(const Relay& relay, int state) {
    std::ofstream file(relay.stateFile);
    if (file.is_open()) {
        file << state;
        file.close();
    } else {
        std::cerr << "Failed to save state to " << relay.stateFile << std::endl;
    }
}

int loadState(const Relay& relay) {
    std::ifstream file(relay.stateFile);
    if (!file.is_open()) {
        std::cout << "No state file found for GPIO " << relay.gpioPin << ", defaulting to OFF" << std::endl;
        return 0;
    }
    int state = 0;
    file >> state;
    file.close();
    std::cout << "Restored GPIO " << relay.gpioPin << " state: " << (state ? "ON" : "OFF") << std::endl;
    return state;
}

// ── GPIO helpers ─────────────────────────────────────────────────────────────

// findMainGpiochip finds the /dev/gpiochipN exposing the 40-pin header,
// rather than assuming it's chip 0. Which number that chip lands on
// depends on how many other gpiochips (HATs, PMIC, SD/ETH housekeeping,
// etc.) enumerate first, which varies across boards and kernel/overlay
// combinations -- on at least one Pi 5 in the field, the header's chip
// (labeled "pinctrl-rp1", the RP1 southbridge) came up as gpiochip15,
// not gpiochip0. Pi 3/2/1-class boards have no RP1; their header lives
// directly on the SoC's own controller, labeled "pinctrl-bcm2835" by
// both the mainline and Raspberry Pi kernels -- including on Alpine's
// linux-rpi kernel package. Both labels are checked so the same binary
// works unmodified across board generations.
//
// Queries each chip's label via GPIO_GET_CHIPINFO_IOCTL (the same cdev
// ioctl gpiodetect/libgpiod use) rather than reading /sys/bus/gpio/devices/
// */label -- that sysfs attribute belongs to the legacy, now-deprecated
// sysfs GPIO ABI and isn't guaranteed to exist on current kernels, which
// is why an earlier version of this check silently found nothing.
int findMainGpiochip() {
    const std::filesystem::path devDir = "/dev";
    if (!std::filesystem::exists(devDir)) return -1;

    const std::string prefix = "gpiochip";
    for (const auto& entry : std::filesystem::directory_iterator(devDir)) {
        std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;

        int fd = open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        struct gpiochip_info info{};
        int ret = ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info);
        close(fd);
        if (ret < 0) continue;

        std::string label(info.label);
        if (label != "pinctrl-rp1" && label != "pinctrl-bcm2835") continue;

        try {
            return std::stoi(name.substr(prefix.size()));
        } catch (...) {
            return -1;
        }
    }
    return -1;
}

// Opens the shared gpiochip once and claims every configured relay's pin
// as an output on it -- one chip handle covers all lines on the header,
// so relays don't each need their own chip open.
void setup() {
    system(("mkdir -p " + STATE_DIR).c_str());

    int chipNum = findMainGpiochip();
    if (chipNum < 0) {
        std::cerr << "Could not find a gpiochip labeled pinctrl-rp1 or "
                      "pinctrl-bcm2835, falling back to gpiochip0" << std::endl;
        chipNum = 0;
    } else {
        std::cout << "Found header gpiochip at /dev/gpiochip" << chipNum << std::endl;
    }

    gpioHandle = lgGpiochipOpen(chipNum);
    if (gpioHandle < 0) {
        std::cerr << "Failed to open GPIO chip " << chipNum << std::endl;
        return;
    }

    for (const auto& relay : g_relays) {
        int lastState;
        if (relay.alwaysOn) {
            lastState = 1;
            std::cout << "GPIO " << relay.gpioPin << " is always_on, forcing ON at startup" << std::endl;
            saveState(relay, lastState);
        } else {
            lastState = loadState(relay);
        }
        lgGpioClaimOutput(gpioHandle, 0, relay.gpioPin, lastState);
        std::cout << "GPIO " << relay.gpioPin << " ready, state="
                  << (lastState ? "ON" : "OFF") << std::endl;
    }
}

void cleanup() {
    if (gpioHandle >= 0) {
        for (const auto& relay : g_relays)
            lgGpioWrite(gpioHandle, relay.gpioPin, 0);
        lgGpiochipClose(gpioHandle);
    }
}

void setRelay(const Relay& relay, int state) {
    lgGpioWrite(gpioHandle, relay.gpioPin, state);
    saveState(relay, state);
}

// ── Socket server ────────────────────────────────────────────────────────────

void handleClient(int clientFd, const Relay& relay) {
    char buf[256] = {};

    int n = recv(clientFd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(clientFd); return; }

    std::string cmd(buf);
    cmd.erase(std::remove_if(cmd.begin(), cmd.end(), [](char c){ return c == '\n' || c == '\r'; }), cmd.end());

    std::string response;

    if (cmd == "on") {
        setRelay(relay, 1);
        response = "OK RELAY=ON\n";

    } else if (cmd == "off") {
        setRelay(relay, 0);
        response = "OK RELAY=OFF\n";

    } else if (cmd == "status") {
        int val = lgGpioRead(gpioHandle, relay.gpioPin);
        response = "RELAY=" + std::string(val ? "ON" : "OFF") + "\n";

    } else {
        response = "ERR unknown command. Use: on | off | status\n";
    }

    std::cout << "GPIO " << relay.gpioPin << " CMD: " << cmd << " -> " << response;
    send(clientFd, response.c_str(), response.size(), 0);
    close(clientFd);
}

void runRelayServer(Relay relay) {
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(relay.port);

    if (bind(serverFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind port " << relay.port << " for GPIO " << relay.gpioPin << std::endl;
        return;
    }
    listen(serverFd, 5);

    std::cout << "GPIO " << relay.gpioPin << " relay listening on port " << relay.port << std::endl;

    while (true) {
        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd >= 0)
            handleClient(clientFd, relay);
    }
}

int main() {
    loadConfig();
    setup();

    std::vector<std::thread> servers;
    for (const auto& relay : g_relays)
        servers.emplace_back(runRelayServer, relay);

    for (auto& t : servers)
        t.join();

    cleanup();
    return 0;
}
