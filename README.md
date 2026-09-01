# pi-relay-control-alpine

A GPIO relay control daemon for Raspberry Pi 3 running Alpine Linux (aarch64).
Exposes a simple TCP socket interface to turn one or more relays on/off and
query their state, with persistent state across restarts.

This started as an Alpine/OpenRC port of [pi-relay-control](https://github.com/jacohanekom/pi-relay-control)
(which targets Raspberry Pi 5 / Raspberry Pi OS / systemd, and controls a
single relay). This repo has since diverged: it supports any number of
relays, each on its own GPIO pin and TCP port, configured as separate
`relay` lines in `/etc/pi-relay-control.conf`. It has no Debian packaging
and doesn't publish to the aipicam APT repo -- CI builds a plain binary
tarball and a standalone, offline-installable `.apk`.

## Requirements

- Raspberry Pi 3 (or 2/4/5) running Alpine Linux, aarch64
- `liblgpio.so.1` at runtime (not in Alpine's repos -- see below)
- One or more relays, each on its own BCM GPIO pin (configurable)

## Install the .apk (recommended)

Every push builds `pi-relay-control-aarch64.apk` (GitHub Actions artifact;
tagged `v*` pushes also attach it to a GitHub Release), built via `abuild`
from [`alpine/APKBUILD`](alpine/APKBUILD). It bundles `relay_control`,
`liblgpio.so.1`, the config, and the OpenRC init script as a normal apk
package -- installs, uninstalls, and upgrades cleanly with `apk`, no repo
or network access needed on the Pi itself.

It's signed with a throwaway key generated fresh in CI each run (there's
no distributed repo to establish trust for), so install with
`--allow-untrusted`:

```sh
apk add --allow-untrusted ./pi-relay-control-aarch64.apk

rc-update add pi-relay-control default
rc-service pi-relay-control start
```

Uninstall with `apk del pi-relay-control`.

## Install from the release tarball

Every push builds `pi-relay-control-alpine-aarch64.tar.gz` (GitHub Actions
artifact; tagged `v*` pushes also attach it to a GitHub Release). It
contains the `relay_control` binary, `liblgpio.so.1`, the default config,
and the OpenRC init script.

```sh
tar xzf pi-relay-control-alpine-aarch64.tar.gz
cd pi-relay-control-alpine-aarch64

install -Dm755 relay_control /usr/bin/relay_control
install -Dm755 liblgpio.so.1 /usr/lib/liblgpio.so.1
install -Dm644 pi-relay-control.conf /etc/pi-relay-control.conf
install -Dm755 pi-relay-control.initd /etc/init.d/pi-relay-control

rc-update add pi-relay-control default
rc-service pi-relay-control start
```

`relay_control` is linked with `-static-libgcc -static-libstdc++`, so the
only runtime shared-library dependency beyond musl itself is
`liblgpio.so.1`, which the tarball provides -- no `apk add` needed for it.

## Build from source

```sh
apk add build-base git linux-headers

git clone --depth 1 https://github.com/joan2937/lg /tmp/lg
make -C /tmp/lg
sudo make -C /tmp/lg install   # installs liblgpio.so.1 + headers to /usr/local

make
sudo make install               # installs to /usr/bin, /etc, /etc/init.d
```

If you installed `liblgpio` to `/usr/local/lib` and it's not being found at
runtime, copy `liblgpio.so.1` into `/usr/lib` (in musl's default search
path) instead of relying on `/usr/local/lib`.

## Configuration

Edit `/etc/pi-relay-control.conf`, one line per relay:

```
relay 5 7778    # BCM GPIO 5,  controlled on TCP port 7778
relay 6 7779    # BCM GPIO 6,  controlled on TCP port 7779
```

Each relay runs its own listener on its own port, so ports must be unique.
The daemon claims all configured pins on the same gpiochip at startup. If
no `relay` lines are present, it falls back to a single relay on GPIO 5 /
port 7778.

Restart after changes: `rc-service pi-relay-control restart`

## Usage

Control a relay with any TCP client, e.g. `nc`, against the port assigned
to it in the config:

```bash
echo "on"     | nc localhost 7778    # Turn relay ON  → OK RELAY=ON
echo "off"    | nc localhost 7778    # Turn relay OFF → OK RELAY=OFF
echo "status" | nc localhost 7778    # Query state    → RELAY=ON
```

### Commands

| Command  | Response                                    |
|----------|---------------------------------------------|
| `on`     | `OK RELAY=ON`                               |
| `off`    | `OK RELAY=OFF`                              |
| `status` | `RELAY=ON` or `RELAY=OFF`                   |
| other    | `ERR unknown command. Use: on | off | status` |

## Service management

```bash
rc-service pi-relay-control start
rc-service pi-relay-control stop
rc-service pi-relay-control restart
rc-update add pi-relay-control default   # start on boot
rc-service pi-relay-control status
```

The service runs as root, respawns automatically on failure (5 s delay,
unlimited retries, via `supervise-daemon`), and persists each relay's
state to `/var/lib/relay_control/state_pin<N>` (one file per configured
GPIO pin) so every relay returns to its last position after a reboot.

**Requires device provisioning**: `start_pre()` refuses to start unless
`/.successfully-initialized` exists at the filesystem root -- the marker
file [pi-bluetooth-configuration](https://github.com/jacohanekom/pi-bluetooth-configuration-alpine)
creates once its BLE setup wizard finishes. This keeps the relay from
being driven on a freshly-imaged Pi that hasn't been provisioned yet. On
a fully unprovisioned device, `rc-update add pi-relay-control default`
still registers the service, but boot-time start attempts fail (logged)
until provisioning completes and the Pi reboots -- at which point the
marker file already exists and the service starts normally on its own.

## GPIO chip detection

The daemon opens the gpiochip exposing the 40-pin header by label rather
than assuming a fixed chip number, since that number shifts depending on
what else (HATs, PMIC, etc.) enumerates first:

- `pinctrl-rp1` -- Raspberry Pi 5's RP1 southbridge
- `pinctrl-bcm2835` -- the SoC's own GPIO controller on Pi 1/2/3/4,
  including under Alpine's `linux-rpi` kernel

Falls back to `gpiochip0` if neither label is found.
