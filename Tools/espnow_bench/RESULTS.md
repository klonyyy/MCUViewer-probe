# ESP-NOW bench results

Measured on 2026-11-01 with two ESP32-S3 boards and ESP-IDF v6.1, with the bench defaults: 2 frames in flight,
2 s streams of 1470-byte frames (the ESP-NOW v2 maximum), round trips over up to 200 pings or 1.5 s.

- **fwd**: the initiator sends; **rev**: the responder (`a0:f2:62:ea:db:3c`) sends.
- **lmk**: encrypted (CCMP with a per-peer key); **open**: not encrypted.
- **loss**: frames handed to ESP-NOW that the receiving application never got.

## Findings

- **Encryption works with 1470-byte v2 frames**, at no cost in throughput or round trip.
- **The 16-QAM rates (24M, MCS3) are the sweet spot**: about 2 MB/s in both directions with no loss down to
  -82 dBm, round trips of 0.8 ms (32 bytes) and 1.8 ms (1470 bytes). 24M reaches about 95% of what one
  unaggregated frame per ACK allows (~2.26 MB/s).
- **11M**: about 1 MB/s. **LR500**: 45-56 kB/s, close to its 500 kb/s raw rate, with round trips of 6 ms
  (32 bytes) and 54 ms (1470 bytes).
- **Every frame the MAC acknowledged reached the application** in every run: no queue drops, no duplicates.
  Frames get lost on the air only, and the sender learns about it from the send callback.
- **1M does not work here**: the frames arrive, but the sender never hears an ACK, so every frame takes all of
  the MAC's retries (about 300 ms). The retries ran on into the next test and spoiled "11M open" in runs 1 and 2;
  "11M lmk" right after was clean. Later bench versions leave 1M out.
- **64-QAM (54M, MCS5, MCS7S) is unpredictable**: in runs 1 and 2, on channel 6, the forward direction degraded
  badly (MCS7S lost 98% of frames) while the received signal was about the same in both directions; in run 3,
  behind the same wall but on channel 1, MCS5 reached 3.1 MB/s both ways at a weaker signal. Interference on
  channel 6 near the responder would explain both the asymmetry and the change; it was not checked.
- **Next to a station link** (run 3, the station joined on the router's channel): about 10% less throughput and
  more jitter (round-trip p95 of 1-2 ms, peaks around 16 ms), still no loss at 16-QAM. LR works with the station
  joined, and having LR enabled did not keep the station from joining.

## Consequences for the ESP-NOW transport

- Default to 24M or MCS3; fall back to 11M, then LR500, when frames fail. A fixed ESP-NOW rate does not adapt by
  itself, so the transport needs its own rate fallback. 64-QAM at most as an opportunistic step up.
- Always set the peer rate explicitly: the ESP-NOW default is 1M.
- The MAC acknowledgement plus a retry on a failed send callback makes the link nearly reliable. Request
  deduplication by seq stays, as an ACK can still get lost.
- Wi-Fi and ESP-NOW side by side on the router's channel is viable.
- The radio is not the bottleneck: the dongle's USB full-speed link (about 1 MB/s) will be.

## Run 1: on a desk, channel 6

```
rate   key  | rtt 32 B avg/p95 ms | rtt 1470 B avg/p95 ms | fwd kB/s   loss | rev kB/s   loss
1M     open |    48.01 /   62.07  |      0.00 /    0.00   |      3.9   0.0% |      4.3   0.0%
1M     lmk  |    54.47 /   63.08  |      0.00 /    0.00   |      3.9   0.0% |      4.2   0.0%
11M    open |     0.00 /    0.00  |      0.00 /    0.00   |   1092.0  22.7% |   1092.4   0.0%
11M    lmk  |     0.98 /    1.02  |      3.19 /    3.25   |   1081.8   0.0% |   1078.9   0.0%
24M    open |     0.74 /    0.79  |      1.79 /    1.82   |   2183.2   0.0% |   2167.2   0.0%
24M    lmk  |     0.76 /    0.79  |      1.81 /    1.85   |   2173.0   0.0% |   2154.3   0.0%
54M    open |     0.87 /    1.32  |      4.11 /    8.98   |    972.1   0.0% |   3484.1   0.0%
54M    lmk  |     0.91 /    1.62  |      4.82 /   11.74   |    805.9   1.3% |   3240.0   0.1%
MCS3   open |     0.77 /    0.80  |      1.75 /    1.78   |   2261.4   0.0% |   2256.4   0.0%
MCS3   lmk  |     0.79 /    0.82  |      1.76 /    1.79   |   2255.7   0.0% |   2248.7   0.0%
MCS5   open |     0.87 /    1.35  |      4.73 /    9.14   |    455.1   0.2% |   2277.8   0.0%
MCS5   lmk  |     0.88 /    1.35  |      3.14 /    5.99   |   1012.7   0.2% |   2616.7   0.0%
MCS7S  open |     1.63 /    3.08  |      0.00 /    0.00   |      5.7  97.0% |    969.1   0.1%
MCS7S  lmk  |     1.41 /    2.36  |      7.77 /   12.10   |    292.0  13.8% |   1820.8   0.0%
LR500  open |     5.77 /    5.82  |     53.79 /   53.00   |     56.2   0.0% |     56.1   0.0%
LR500  lmk  |     6.27 /    6.29  |     53.40 /   53.43   |     55.7   0.0% |     55.0   0.0%
```

In this bench version a round trip of `0.00` means that no ping got an answer.

## Run 2: behind a wall, channel 6

Received signal -66 to -73 dBm, about the same in both directions.

```
rate   key  | rtt 32 B avg/p95 ms | rtt 1470 B avg/p95 ms | fwd kB/s   loss | rev kB/s   loss
1M     open | setup failed
1M     lmk  |    41.96 /   50.23  |      0.00 /    0.00   |      4.1   0.0% |      4.2   0.0%
11M    open |     0.00 /    0.00  |      0.00 /    0.00   |   1115.0  45.0% |   1084.1   0.0%
11M    lmk  |     0.99 /    1.02  |      3.23 /    3.32   |   1082.3   0.0% |   1071.1   0.0%
24M    open |     0.77 /    0.84  |      1.84 /    2.02   |   2152.5   0.0% |   2164.9   0.0%
24M    lmk  |     0.76 /    0.81  |      1.83 /    1.89   |   2137.0   0.0% |   2104.9   0.0%
54M    open |     0.82 /    1.16  |      3.25 /    6.12   |   1250.7   0.0% |   3478.9   0.0%
54M    lmk  |     0.87 /    1.45  |      3.02 /    6.32   |    987.4   0.4% |   3188.8   0.0%
MCS3   open |     0.77 /    0.82  |      1.82 /    2.43   |   2209.2   0.0% |   2158.0   0.0%
MCS3   lmk  |     0.80 /    0.92  |      1.82 /    2.45   |   2176.7   0.0% |   2135.3   0.0%
MCS5   open |     0.81 /    1.12  |      3.17 /    6.05   |    841.1   0.0% |   2956.7   0.0%
MCS5   lmk  |     0.82 /    1.04  |      2.31 /    4.61   |   1365.8   0.1% |   2965.8   0.0%
MCS7S  open |     1.56 /    2.91  |      0.00 /    0.00   |      4.2  98.5% |   1094.8   0.0%
MCS7S  lmk  |     1.29 /    2.13  |      6.63 /    9.54   |     38.4  75.9% |   1143.0   0.1%
LR500  open |     5.81 /    5.80  |     53.84 /   53.11   |     56.2   0.0% |     56.2   0.0%
LR500  lmk  |     6.28 /    6.31  |     53.39 /   53.44   |     55.7   0.0% |     53.6   0.0%
```

Where the frames went in the telling rows:

| Row | Direction | Sent | MAC ok | MAC fail | Received | RSSI |
|---|---|---|---|---|---|---|
| 1M lmk | fwd | 7 | 0 | 7 | 7 | -73 |
| 11M open | fwd | 100 | 55 | 45 | 55 | -66 |
| 11M open | rev | 1476 | 1476 | 0 | 1476 | -66 |
| 54M open | fwd | 1701 | 1701 | 0 | 1701 | -67 |
| 54M open | rev | 4734 | 4734 | 0 | 4734 | -69 |
| MCS7S open | fwd | 198 | 3 | 195 | 3 | -72 |
| MCS7S open | rev | 1490 | 1490 | 0 | 1490 | -71 |

54M forward lost nothing but moved a third of the reverse direction's frames: the MAC retried frames it does not
report.

## Run 3: behind a wall, next to a station link, channel 1

Behind the wall like run 2. The initiator joined the router (WPA2, `bgn`, router RSSI -87 dBm) before the tests;
the received signal between the boards was -70 to -82 dBm, weaker than in run 2. Two join attempts before this run
failed with reason 2 (`AUTH_EXPIRE`), the router not answering at that weak signal. This bench version leaves 1M
out and reports pings without an answer.

```
rate   key  | rtt 32 B avg/p95 ms        | rtt 1470 B avg/p95 ms      | fwd kB/s   loss | rev kB/s   loss
11M    open |   1.09 /   2.28            |   3.98 /   6.90            |    869.6   0.0% |    937.9   0.0%
11M    lmk  |   1.26 /   3.82            |   3.79 /   6.64            |    899.8   0.0% |    880.4   0.0%
24M    open |   0.87 /   2.15            |   2.18 /   5.14            |   1877.8   0.0% |   1899.5   0.0%
24M    lmk  |   0.84 /   1.12            |   2.03 /   3.02            |   1922.9   0.0% |   1947.1   0.0%
54M    open |   1.00 /   1.84, 14 lost   |   2.57 /   5.65, 11 lost   |   1298.6   1.1% |   2310.4   0.0%
54M    lmk  |   0.80 /   1.71, 2 lost    |   1.70 /   4.15, 3 lost    |   2860.5   0.2% |   2552.4   0.3%
MCS3   open |   0.84 /   1.02            |   1.95 /   4.24            |   2055.1   0.0% |   2027.2   0.0%
MCS3   lmk  |   0.86 /   1.14            |   1.89 /   3.00            |   2046.4   0.0% |   2011.6   0.0%
MCS5   open |   0.80 /   0.98            |   2.00 /   4.78            |   2381.2   0.0% |   2685.7   0.0%
MCS5   lmk  |   0.81 /   0.98            |   1.44 /   3.13            |   3178.8   0.0% |   3124.8   0.0%
MCS7S  open |   0.80 /   0.99            |   1.62 /   4.26            |   2675.3   0.0% |   2296.6   0.0%
MCS7S  lmk  |   0.88 /   1.87            |   2.08 /   5.61, 3 lost    |   1844.1   0.6% |   1873.9   0.6%
LR500  open |   6.18 /   9.30            |  55.76 /  58.73, 1 lost    |     43.4   0.0% |     50.4   0.0%
LR500  lmk  |   6.96 /  10.83            |  59.97 /  85.25, 3 lost    |     44.5   0.0% |     51.9   0.0%
```

Received signal per rate: -78 dBm at 11M, -82 dBm at 24M, -74 to -77 dBm at the OFDM and HT rates, -70 dBm at
LR500. Round-trip maxima reached 16 ms.
