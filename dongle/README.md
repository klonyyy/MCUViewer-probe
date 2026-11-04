# ESP-NOW dongle

An ESP32-S3 on the PC's USB port that reaches probes over ESP-NOW, without a router. To the PC it is a serial
port (USB CDC-ACM), which Windows, Linux and macOS drive without installing anything. Over it go the same NETWORK
PROBE frames as over TCP, plus `DONGLE_SELECT`, which picks the probe to talk to (see
[NetworkProbeProto.hpp](../components/network_probe_proto/NetworkProbeProto.hpp)). The radio side is described in
[EspNowLink.hpp](../components/espnow_link/EspNowLink.hpp).

```
probe  <-- ESP-NOW -->  dongle  <-- USB serial port -->  PC
```

The probe firmware speaks ESP-NOW next to Wi-Fi: it serves one client at a time, over TCP or through a dongle, and a
new client takes over from the old one, as before.

## Board

Any ESP32-S3 board whose USB connector is wired to the chip's own USB pins (GPIO19/20), such as the Waveshare
ESP32-S3-Pico. Its RGB LED (GPIO21 by default) shows what the dongle does:

| LED | Meaning |
|---|---|
| blue, short blinks | no session with a probe |
| green | a session; it dips for the traffic |
| one red flash | the probe ended the session: another client took over, or it heard nothing for 2 s |

## Building and flashing

```
cd dongle
idf.py build
idf.py -p <PORT> flash
```

After the flash, the board's USB port stops being the ESP32-S3's own serial port: it comes back as a new one named
*NETWORK PROBE ESP-NOW dongle*. To flash again, hold BOOT while plugging the board in, flash, then plug it in once
more: the reset at the end of the flash leaves a board started with BOOT in download mode.

## Channel

ESP-NOW talks on one Wi-Fi channel, and a probe is on the channel its radio is on: the channel of the network it
joined (its boot log shows it, `connected with ..., channel N`), or in setup mode the channel of its access point,
6 by default. Set the dongle to the same one: *NETWORK PROBE dongle → Wi-Fi channel* in menuconfig, or
`CONFIG_DONGLE_CHANNEL=1` in `dongle/sdkconfig.local`. Finding the probe's channel by itself comes later.

## Trying it

MCUViewer does not speak to the dongle yet. The timing test does:

```
pip install pyserial
python Tools/read_timing.py --serial COM7
python Tools/read_timing.py --serial COM7 --probe network-probe-a1b2c3
```

It discovers the probes the dongle hears, selects one and runs its tests through it.

## Limits for now

- Fixed channel, see above.
- No encryption or pairing: any dongle in range can talk to the probe, like any host on the network over TCP.
- No retransmission: a request or an answer lost on the air shows as a timeout on the host. Losses at a good
  signal are rare, see [the bench results](../Tools/espnow_bench/RESULTS.md).
- USB full speed limits the dongle to about 1 MB/s, and there is no flow control yet: when the probe sends faster
  than that for long, the dongle drops frames, and the host sees gaps or a failed READ_BULK.
