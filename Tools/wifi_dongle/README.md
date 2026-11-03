# Wi-Fi dongle demo

An ESP32-S3 on the PC's USB port that joins a probe's access point and shows up on the PC as a network adapter. The
PC then reaches the probe at `192.168.4.1` without a router, MCUViewer and GDB included, and keeps its own Wi-Fi
for the internet. The probe firmware stays as it is.

```
probe (access point network-probe-xxxxxx, 192.168.4.1)  <-- Wi-Fi -->  dongle  <-- USB network adapter -->  PC
```

The dongle has no IP address of its own: it passes Ethernet frames between USB and Wi-Fi unchanged, and the PC's
adapter takes the dongle's Wi-Fi MAC address, so to the probe the PC is the station. Only the DHCP answers are
touched: the dongle blanks the router and DNS server in them, so the PC does not send its internet traffic to the
probe.

## Board

Any ESP32-S3 board whose USB connector is wired to the chip's own USB pins (GPIO19/20), such as the Waveshare
ESP32-S3-Pico. The board's RGB LED (GPIO21 by default, see *Wi-Fi dongle* in menuconfig) shows what it does:

| LED | Meaning |
|---|---|
| blue, short blinks | looking for a probe access point |
| green | joined; it dips for the frames passed on |

## Building and flashing

```
cd Tools/wifi_dongle
idf.py build
idf.py -p <PORT> flash monitor
```

After the flash the board's USB port stops being a serial port: it becomes the network adapter, and the monitor
shows only the boot log before that. Windows 11 and Linux drive the adapter without installing anything.

To flash again, hold BOOT while plugging the board in: it starts in download mode with its serial port back.

## Connecting the probe

The dongle joins the probe's own access point, which the probe opens in setup mode: when none of its saved
networks is in range, or none is saved.

1. Put the probe in setup mode. Out of reach of its saved networks it gets there by itself within 15 s. Within
   reach, forget them: hold BOOT on the probe for 3 s (they can be added again on the setup page later). A probe
   with a network built into its firmware joins that one instead.
2. Plug the dongle in. Its LED turns green once it has joined `network-probe-xxxxxx`, the strongest one in range.
   The PC's new adapter gets an address `192.168.4.x` from the probe (`ipconfig` shows it).
3. Connect:
   - MCUViewer: probe **NETWORK_PROBE**, address `192.168.4.1`, port `2023`. The probe shows in the Probe list too
     if MCUViewer's discovery reaches that adapter.
   - GDB: `target extended-remote 192.168.4.1:2022`.
   - Timing test: `python Tools/read_timing.py 192.168.4.1`.

With several probes in range, the dongle joins the strongest. To pick one, set its access point's name as
**Network to join** in menuconfig. A probe access point with a password (`CONFIG_NETWORK_PROBE_AP_PASSWORD`) needs
the same password as **Network password**, best in `sdkconfig.local`:

```
CONFIG_DONGLE_WIFI_SSID="network-probe-a1b2c3"
CONFIG_DONGLE_WIFI_PASSWORD="secret"
```

When the link drops, the dongle looks for a probe again and rejoins by itself.

## Limits of the demo

- The probe's access point is its setup mode. With saved networks out of range, the probe scans for them every
  30 s, which stalls the link briefly; once one comes in range, the probe joins it and closes the access point a
  minute later. A probe mode that keeps the access point up for good would remove both.
- USB full speed limits the dongle to about 1 MB/s.
