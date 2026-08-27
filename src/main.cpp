#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
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
const std::string STATE_FILE  = "/var/lib/relay_control/state";
const int DEFAULT_GPIO_PIN    = 5;     // BCM 5 = IO21 on HAT
const int DEFAULT_PORT        = 7778;

int gpioHandle = -1;
int GPIO_PIN   = DEFAULT_GPIO_PIN;
int SOCKET_PORT = DEFAULT_PORT;

// ── Config ───────────────────────────────────────────────────────────────────

void loadConfig() {
    std::ifstream file(CONFIG_FILE);
    if (!file.is_open()) {
        std::cout << "No config at " << CONFIG_FILE << ", using defaults." << std::endl;
        return;
    }

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

        if (key == "gpio_pin") {
            iss >> GPIO_PIN;
            std::cout << "Config: gpio_pin=" << GPIO_PIN << std::endl;
        } else if (key == "port") {
            iss >> SOCKET_PORT;
            std::cout << "Config: port=" << SOCKET_PORT << std::endl;
        }
    }
    file.close();
}

// ── State persistence ────────────────────────────────────────────────────────

void saveState(int state) {
    std::ofstream file(STATE_FILE);
    if (file.is_open()) {
        file << state;
        file.close();
    } else {
        std::cerr << "Failed to save state to " << STATE_FILE << std::endl;
    }
}

int loadState() {
    std::ifstream file(STATE_FILE);
    if (!file.is_open()) {
        std::cout << "No state file found, defaulting to OFF" << std::endl;
        return 0;
    }
    int state = 0;
    file >> state;
    file.close();
    std::cout << "Restored state: " << (state ? "ON" : "OFF") << std::endl;
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

void setup() {
    system("mkdir -p /var/lib/relay_control");

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

    int lastState = loadState();
    lgGpioClaimOutput(gpioHandle, 0, GPIO_PIN, lastState);
    std::cout << "GPIO " << GPIO_PIN << " ready, state="
              << (lastState ? "ON" : "OFF") << std::endl;
}

void cleanup() {
    if (gpioHandle >= 0) {
        lgGpioWrite(gpioHandle, GPIO_PIN, 0);
        lgGpiochipClose(gpioHandle);
    }
}

void setRelay(int state) {
    lgGpioWrite(gpioHandle, GPIO_PIN, state);
    saveState(state);
}

// ── Socket server ────────────────────────────────────────────────────────────

void handleClient(int clientFd) {
    char buf[256] = {};

    int n = recv(clientFd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(clientFd); return; }

    std::string cmd(buf);
    cmd.erase(std::remove_if(cmd.begin(), cmd.end(), [](char c){ return c == '\n' || c == '\r'; }), cmd.end());

    std::string response;

    if (cmd == "on") {
        setRelay(1);
        response = "OK RELAY=ON\n";

    } else if (cmd == "off") {
        setRelay(0);
        response = "OK RELAY=OFF\n";

    } else if (cmd == "status") {
        int val = lgGpioRead(gpioHandle, GPIO_PIN);
        response = "RELAY=" + std::string(val ? "ON" : "OFF") + "\n";

    } else {
        response = "ERR unknown command. Use: on | off | status\n";
    }

    std::cout << "CMD: " << cmd << " -> " << response;
    send(clientFd, response.c_str(), response.size(), 0);
    close(clientFd);
}

int main() {
    loadConfig();
    setup();

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SOCKET_PORT);

    bind(serverFd, (sockaddr*)&addr, sizeof(addr));
    listen(serverFd, 5);

    std::cout << "Relay control listening on port " << SOCKET_PORT << std::endl;

    while (true) {
        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd >= 0)
            handleClient(clientFd);
    }

    cleanup();
    return 0;
}
