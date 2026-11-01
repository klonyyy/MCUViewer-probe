# ESP-NOW bench

Measures ESP-NOW between two ESP32-S3 boards before the probe gets an ESP-NOW transport: the round trip, the
throughput in both directions and where frames get lost, at several PHY rates, with and without encryption, and
optionally next to a station link on the same radio.

## Running it

Flash the same firmware to both boards and open a monitor on each:

```
cd Tools/espnow_bench
idf.py build
idf.py -p <PORT_A> flash monitor
idf.py -p <PORT_B> flash monitor
```

Both boards wait for an initiator, hopping through the channels. Press BOOT on one of them: it becomes the
initiator, finds the other board, runs every test (about two minutes) and prints a summary. Each rate and key
also gets two detailed log lines as soon as it is done. Press BOOT on the other board to run it from that side.

Each rate is tested open and with a key (`lmk`, CCMP with a fixed bench key). For each one:

- **rtt**: round trip of a 32-byte and a 1470-byte frame (ESP-NOW v2 maximum), up to 200 pings or 1.5 s;
- **fwd**: the initiator streams 1470-byte frames for 2 s and the responder counts them;
- **rev**: the same from the responder to the initiator.

The detailed lines tell where frames went:

| Field | Meaning |
|---|---|
| sent | frames `esp_now_send()` took |
| mac ok / fail | the peer's MAC acknowledged them, or did not after the MAC's retries |
| no_mem | `esp_now_send()` was out of buffers and was tried again |
| errors | `esp_now_send()` refused the frame |
| received, dup, gaps | what the receiving application got, frames seen twice, frames skipped |
| queue drops | frames the receive callback had no buffer for: the application did not keep up |
| loss | frames sent that the receiving application never got |

A frame acknowledged by the MAC can still be lost: `mac ok` minus the unique frames received is what got lost
behind the radio.

The first runs and what they mean for the ESP-NOW transport are in [RESULTS.md](RESULTS.md).

## Options

`idf.py menuconfig`, *ESP-NOW bench*:

- **Channel**: where the initiator runs when it joins no network, 6 by default.
- **Frames in flight**: frames handed to ESP-NOW before the first one's send callback, 2 by default. Try 1 and 4.
- **Throughput test duration**.
- **Wi-Fi network**: the initiator joins it before the tests, so they run on the network's channel next to a
  station link, like a probe on Wi-Fi talking to a dongle. Put the credentials in `sdkconfig.local`:

  ```
  CONFIG_ESPNOW_BENCH_WIFI_SSID="my-network"
  CONFIG_ESPNOW_BENCH_WIFI_PASSWORD="secret"
  ```

  After changing `sdkconfig.local`, delete `sdkconfig` so the build picks it up.
