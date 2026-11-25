# ESP-NOW dongle

An ESP32-S3 on the PC's USB port that reaches probes over ESP-NOW, without a router. To the PC it is a serial
port (USB CDC-ACM), which Windows, Linux and macOS drive without installing anything. Over it go the same NETWORK
PROBE frames as over TCP, plus `DONGLE_SELECT`, which picks the probe to talk to, `DONGLE_STATUS`, which tells the
dongle's side of the radio link, and `DONGLE_PAIR`, `DONGLE_UNPAIR` and `DONGLE_PAIRINGS` for [pairing](#pairing)
(see [NetworkProbeProto.hpp](../components/network_probe_proto/NetworkProbeProto.hpp)). The radio side is described
in [EspNowLink.hpp](../components/espnow_link/EspNowLink.hpp) and
[EspNowPairing.hpp](../components/espnow_link/EspNowPairing.hpp).

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
| yellow blinks | pairing with a probe |
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

Flashing keeps the pairings, which live in NVS; `idf.py erase-flash` erases them, and then the probes still know a
dongle that no longer knows them: pair them again.

## Pairing

Out of the box any dongle reaches any probe. Pairing a dongle with a probe changes that for both:

- the probe takes sessions from its paired dongles only (up to 4), and the dongle talks to its paired probes only
  (up to 8);
- their sessions are encrypted by the radio, and every packet carries a tag only the two can make, so nobody else can
  send in their name or replay their packets.

TCP clients are not affected: on the network, the probe serves anyone, as before.

To pair, the host sends `DONGLE_PAIR` with the probe's id; the probe's LED blinks yellow for 30 s, and a short press
of its BOOT button confirms. Nothing else does: the press is the proof that the dongle is yours. If another dongle asks
the same probe during those 30 s, both are refused. Pairing a pair again gives it a new key. Each side keeps the
pairing until it is told to forget it; a firmware update keeps it too.

To unpair, the probe forgets the dongle first, in a session of that dongle (`UNPAIR`), then the dongle forgets the probe
(`DONGLE_UNPAIR`). A paired dongle can also make the probe forget another dongle, a lost one say. Without any paired
dongle at hand, hold the probe's BOOT button for 10 s and let go, then press it briefly within 5 s: it forgets every
dongle and is open again.

In MCUViewer, the NETWORK PROBE settings show the pairing of the picked probe under the probe list when the connection
is *ESP-NOW dongle*, with **Pair** and **Unpair**. **Pairings...** lists the probes the dongle is paired with, in range
or not, and the dongles a paired probe in range is paired with, where a lost one can be forgotten. A script does the
same:

```
python Tools/dongle_pair.py COM7 list
python Tools/dongle_pair.py COM7 pair network-probe-a1b2c3
python Tools/dongle_pair.py COM7 unpair network-probe-a1b2c3
```

## Channels

ESP-NOW talks on one Wi-Fi channel, and a probe is on the channel its radio is on: the channel of the network it
joined, in setup mode the channel of its access point, 6 by default, and in field mode, without a network, the channel
set for it on its setup page (see [field mode](../README.md#field-mode)). The dongle finds it: a DISCOVER from the
host goes out on every channel in turn, which takes about half a second, and each probe's answer tells its channel.
A session then stays on that channel, and while it lasts the probe neither scans for networks nor connects to one by
itself, as both take its radio off the channel. A probe whose network is out of reach rests on the channel of that
network between its tries, for up to 30 s, so it is easy to find.

The dongle scans the channels its Wi-Fi country allows, 1 to 11 by default. To reach probes on networks on channels
12 and 13, set your country where it allows them: *NETWORK PROBE dongle → Wi-Fi country code* in menuconfig, or
`CONFIG_DONGLE_COUNTRY="PL"` in `dongle/sdkconfig.local`.

## Rate

ESP-NOW sends at one fixed rate, so both sides pick theirs: MCS3 (about 2 MB/s of radio, more than the USB side
carries) while the packets get through, and 11 Mb/s (about 1 MB/s) when they fail. After a while on 11 Mb/s they try
MCS3 again. The serial logs of both show each change, such as `sending to the probe at 11M`, and a host asks for them
without the logs: `GET_STATUS` gives the probe's rate, the signal it gets from the dongle and the channel,
`DONGLE_STATUS` the dongle's.

Espressif's long range mode (about 50 kB/s, with more range) is left out for now: at it a probe with data to send
keeps its transmitter on at full power nearly all the time, more than the supply of the probes built so far carries.
They reset over and over at the edge of range. See
[EspNowRate.hpp](../components/espnow_link/EspNowRate.hpp) to put it back.

## Trying it

In MCUViewer, pick *ESP-NOW dongle* as the connection of the NETWORK PROBE in the acquisition settings. The timing test
runs through the dongle too:

```
pip install pyserial
python Tools/read_timing.py --serial COM7
python Tools/read_timing.py --serial COM7 --probe network-probe-a1b2c3
```

It discovers the probes the dongle hears, selects one and runs its tests through it.

## Delivery

Both sides keep one packet in the air and send it again while the radio gets no acknowledgement for it, up to ten
times; a resend whose first copy came through is dropped by its number. At the slowest rate a side that gets no
acknowledgement pauses between the tries until it hears from the other side again: tries back to back there keep the
transmitter on at full power, more than a probe's supply may carry. The dongle tells the probe how much more it has
room for, so a host that reads slowly holds the probe back, as a full TCP window does: frames are not lost, the probe's
sample buffer fills instead and reports an overflow. When the probe stops acknowledging for 2 s, or answers that it
does not know the session (it restarted, or another client took over), the dongle ends the session and flashes red;
requests get `NO_PROBE` from then on. A probe that restarted answers only once it is on its network again, a few
seconds after the restart.

## Limits for now

- Unpaired, any dongle in range can talk to the probe, like any host on the network over TCP: pair them.
- Pairing trusts whoever asks while the probe's LED blinks yellow: someone else in range at that moment could get in
  between. Pair where you see who is around.
- A packet that fails ten times is lost: a lost request or answer shows as a timeout on the host. At a usable signal
  that is rare, see [the bench results](../Tools/espnow_bench/RESULTS.md).
- USB full speed limits the dongle to about 1 MB/s.
