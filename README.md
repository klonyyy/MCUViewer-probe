# NETWORK PROBE

Wireless SWD probe on the ESP32-S3 for [MCUViewer](https://github.com/klonyyy/MCUViewer).
It is built on [Black Magic Debug](https://codeberg.org/blackmagic-debug/blackmagic) (BMD).

The probe serves two kinds of clients, one session at a time:

- **MCUViewer** over the NETWORK PROBE protocol (TCP port 2023). The probe connects to the
  target without halting it and reads and writes memory while it runs. In HSS mode it samples
  on its own, with its own timestamps, and streams the samples. It also receives the target's
  SWO pin and streams the trace bytes.
- **GDB**: BMD's GDB server over Wi-Fi (TCP port 2022). Use it for flashing and validation.

## Hardware

Reference board: Waveshare ESP32-S3-Pico. The defaults below can be changed in menuconfig
under *NETWORK PROBE SWD / Black Magic Debug port* and *NETWORK PROBE SWO*.

| Signal | GPIO |
|---|---|
| SWDIO | 4 |
| SWCLK | 5 |
| SWO (optional, input) | 6 |
| nRST (optional, open-drain) | 10 |
| GND | any GND pin |

The target must run at 3.3 V and share ground with the probe.

### Status LED

The board's RGB LED (a WS2812 on GPIO21) shows what the probe does. GPIO, brightness and color
order are under *NETWORK PROBE status LED* in menuconfig; if red shows as green, switch the color
order.

| LED | Meaning |
|---|---|
| blue, short blinks | joining the Wi-Fi network |
| purple, breathing | setup mode: the access point is up |
| green, slow breathing | on the network, no client |
| cyan | MCUViewer connected; it dips briefly for the frames sent, so streaming flickers evenly |
| orange | GDB session; it dips briefly for the packets |
| one amber flash | samples or SWO bytes were dropped (buffer overflow) |
| one red flash | an access to the target failed, or no target answered |
| four red blinks | the target was lost, or a join from the setup page failed |
| white, speeding up | the BOOT button is held: on release before the end nothing happens |

Alerts show over the session and the network; the held button shows over everything.

## Building the firmware

Requires ESP-IDF v6.1.

```
git submodule update --init
idf.py build
idf.py -p <PORT> flash monitor
```

For development, a default network can be built in. Wi-Fi credentials never go into tracked
files: create `sdkconfig.local` in the repository root. It is git-ignored:

```
CONFIG_NETWORK_PROBE_WIFI_SSID="my-network"
CONFIG_NETWORK_PROBE_WIFI_PASSWORD="my-password"
```

It is applied as an extra defaults file. Defaults only fill options that are missing from
`sdkconfig`, so after changing it, or `sdkconfig.defaults`, delete `sdkconfig` (or use
`idf.py menuconfig`). The default network applies until a network is saved on the setup page.

## Connecting the probe to Wi-Fi

The probe joins the network saved on it. Out of the box no network is saved, so it starts in
**setup mode**:

1. It opens an access point named after its device id, `network-probe-xxxxxx`: the last
   three bytes of its MAC address. The boot log prints the id too.
2. Join that network with a phone or a laptop. The setup page opens by itself (captive
   portal); if it does not, open `http://192.168.4.1/`.
3. Pick your network, enter its password and, if you like, a hostname instead of the device
   id. Only 2.4 GHz networks work.
4. The probe tries the network while its access point stays up, so the page shows the result:
   the address, or why it failed (for example a wrong password). Once connected, the probe
   saves the network and closes the access point a minute later.

At every boot the probe joins the saved network. If it gets no IP address within 15 s, setup
mode starts again, and the probe keeps trying the saved network every 30 s, so after a router
outage it comes back by itself. While the access point is up, MCUViewer and GDB also work
over it, at 192.168.4.1.

To forget the saved network, hold the BOOT button (GPIO0, see
`CONFIG_NETWORK_PROBE_SETUP_BUTTON_GPIO`) for 3 s while the probe runs. It restarts in setup
mode, or joins the default network if one is built in. Do not hold BOOT while powering up: that
starts the ESP32-S3's download mode.

The setup access point is open unless `CONFIG_NETWORK_PROBE_AP_PASSWORD` is set (at least 8
characters).

## Using MCUViewer

In the acquisition settings pick the debug probe **NETWORK_PROBE** and enter:

- **Address**: the hostname with `.local`, by default `network-probe-xxxxxx.local`, or the IP
  address shown on the setup page or in the boot log;
- **Port**: `2023` (the field is shared with the GDB server probe, whose default is 3333);
- **SWD speed**: up to 8000 kHz is verified with a logic analyzer, faster clocks are not yet;
- **Mode**: **HSS**. NORMAL works too, but every sampling tick costs a network round trip, which
  limits it to about 100–300 Hz over Wi-Fi.

The probe serves one client at a time. While MCUViewer is connected, GDB connections are
refused; while GDB is connected, MCUViewer gets no answer until the GDB session ends. A second
MCUViewer connection takes over from the first.

### SWO

The probe receives SWO in NRZ (UART) encoding at up to 5 MBaud, with DMA, and passes the bytes
on unchanged: the host decodes the ITM packets. Like with an ST-Link, the host sets up the
target's trace unit (DEMCR, TPIU, ITM and the vendor's trace pin enable) itself, with memory
writes over SWD; the probe only receives. The SWO clock is the core clock divided by the trace
prescaler + 1, so pick a prescaler that keeps it at or below 5 MHz. SWO runs alongside HSS
streaming, on the same connection.

MCUViewer's trace viewer needs a NETWORK PROBE trace probe on its side to use this.

## Using GDB

```
gdb-multiarch firmware.elf
(gdb) target extended-remote network-probe-xxxxxx.local:2022
(gdb) monitor swd_scan
(gdb) attach 1
(gdb) x/8x 0x20000000
(gdb) load
(gdb) monitor frequency 8M  # change the SWD clock
```

## Repository layout

The repository root is the ESP-IDF project (esp32s3). Its own components are registered in
`CMakeLists.txt` instead of IDF's usual `main/` and `components/`.

```
App/                  probe application: Wi-Fi and mDNS, setup mode with the setup page
                      (SetupPortal) and the saved network (NetworkSettings), the net task
                      (NetServer), the SWD owner task with the GDB server, requests and
                      streaming (ProbeService), light connect (TargetLink), the SWO receiver
                      (SwoCapture), the status LED (StatusLed) and the protocol header shared
                      with MCUViewer
Platform/             BMD's ESP32-S3 platform port (SWD tap, GDB over TCP); builds BMD from
                      External/blackmagic
External/blackmagic/  Black Magic Debug v2.1.0 (git submodule, pinned)
Tools/                host-side test scripts (Python 3, standard library only)
```

## Timing test

`Tools/read_timing.py` measures a probe over the NETWORK PROBE protocol: the network round trip,
READ and READ_BULK throughput, a recorder-sized buffer read the way MCUViewer does it, and the HSS
sampling rate. With `--flash` it also compares READ and READ_BULK byte for byte, with 32-, 16- and
8-bit accesses; `--dump` and `--compare` keep a reference taken with another firmware. It only
reads, the target keeps running, and it leaves a probe alone that serves another session.

```
python Tools/read_timing.py 192.168.1.19 --clock 4000,8000 --flash 0x08000000
python Tools/read_timing.py --help
```

## Development

The firmware is C++ (IDF's default, `gnu++26`): one class per module in PascalCase files,
formatted with the root `.clang-format`. BMD is C: each file includes the BMD headers it needs
in one `extern "C"` block, and the functions BMD calls keep their C names. Only
`Platform/platform.h`, which BMD's own sources include, and the `Platform/libc_compat` shim
stay C.

For clangd, `.clangd` points at `build/compile_commands.json`. clangd needs
`--query-driver=**/xtensa-esp*-elf-*` to find the toolchain headers, which
`.vscode/settings.json` sets.

## Licensing

The firmware is GPL-3.0-or-later (see `COPYING`), because it links Black Magic Debug.

`App/NetworkProbeProto.hpp`, the protocol header shared with MCUViewer, is MIT. It must not
contain code derived from BMD.
