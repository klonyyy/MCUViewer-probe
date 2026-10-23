# MVProbe

Wireless SWD probe on the ESP32-S3 for [MCUViewer](https://github.com/klonyyy/MCUViewer).
It is built on [Black Magic Debug](https://codeberg.org/blackmagic-debug/blackmagic) (BMD).

The probe has two modes:

- **GDB mode**: BMD's GDB server over Wi-Fi. Use it for flashing and validation.
- **Streaming mode** (in progress): the probe reads a host-supplied list of memory blocks over
  SWD without halting the core, and streams the samples to the host over an open TCP
  protocol.

## Repository layout

```
firmware/                    ESP-IDF project (esp32s3)
  components/bmd/            BMD (git submodule, pinned) + ESP32-S3 platform port (port/)
  main/                      app_main, Wi-Fi/mDNS, GDB server task
proto/                       streaming protocol spec and header (MIT)
host/python/                 reference client, planner, viewer (MIT)
docs/                        stage reports and measurement notes
```

## Hardware

Reference board: Waveshare ESP32-S3-Pico. The defaults below can be changed in menuconfig
under *MVProbe SWD / Black Magic Debug port*.

| Signal | GPIO | Pico header |
|---|---|---|
| SWDIO | 11 | GP0 (pin 1) |
| SWCLK | 12 | GP1 (pin 2) |
| GND | – | pin 3 |
| nRST (optional, open-drain) | 14 | GP3 (pin 5) |

The target must run at 3.3 V and share ground with the probe.

## Building the firmware

Requires ESP-IDF v6.1.

```
git submodule update --init
cd firmware
idf.py build
idf.py -p <PORT> flash monitor
```

Wi-Fi credentials never go into tracked files. Create `firmware/sdkconfig.local`, which is
git-ignored:

```
CONFIG_MVPROBE_WIFI_SSID="my-network"
CONFIG_MVPROBE_WIFI_PASSWORD="my-password"
```

It is applied as an extra defaults file. Defaults only fill options that are missing from
`sdkconfig`, so after changing it, delete `firmware/sdkconfig` (or use `idf.py menuconfig`).

If no SSID is set, or the station gets no IP address within 15 s, the probe starts an access
point named `mvprobe` (192.168.4.1). The AP is open unless `CONFIG_MVPROBE_AP_PASSWORD` is
set (at least 8 characters).

## Using GDB mode

```
gdb-multiarch firmware.elf
(gdb) target extended-remote mvprobe.local:2022
(gdb) monitor swd_scan
(gdb) attach 1
(gdb) x/8x 0x20000000
(gdb) load
(gdb) monitor swd_info      # SWD clock and transaction timing
(gdb) monitor frequency 8M  # change the SWD clock
```

## Licensing

- `firmware/`: GPL-3.0-or-later (see `firmware/COPYING`), because it links Black Magic Debug.
- `proto/` and `host/`: MIT. These directories must not contain code derived from BMD.
