# Stage 1 — BMD GDB server over Wi-Fi

> Paths are those of the time. Since 2026-10-24 the firmware lives in `App/`, `Platform/` and
> `External/blackmagic`, in C++ (see the decision log in [plan.md](plan.md)).

Status: builds cleanly with ESP-IDF v6.1 (0 warnings with IDF's `-Werror`). The GDB flow works
on hardware (Waveshare ESP32-S3-Pico, 2026-10-23), and SWCLK was confirmed
at 8 MHz with a logic analyzer.

## What was built

| Piece | Where | Notes |
|---|---|---|
| BMD v2.1.0 | `firmware/components/bmd/blackmagic` (submodule) | unpatched |
| BMD component | `firmware/components/bmd/CMakeLists.txt` | Source lists mirror BMD's meson files. All Cortex-M families. `version.h` from `git describe`. `WHOLE_ARCHIVE`, so the real vendor probes win over the weak no-ops in `target_probe.c` (checked in the ELF: `stm32f4_probe` is 200 bytes, not an alias) |
| Platform layer | `port/platform.c`, `port/platform.h` | `platform.h` includes no ESP-IDF header. No BMD `CONFIG_*` name collides with `sdkconfig.h` (checked) |
| SWD tap | `port/swdptap_esp.c` | Dedicated GPIO bundle (SWCLK, SWDIO) created on the SWD core. Half periods are timed on CCOUNT, with resynchronisation after interrupts. Turnaround switches OE via `GPIO_ENABLE_W1TS/W1TC` (OE control is taken away from the dedicated GPIO peripheral at init). Functions live in IRAM |
| JTAG | `port/jtagtap_stub.c` | empty chain (TDO = 1) |
| GDB transport | `port/gdb_if_tcp.c` | TCP port 2022, one client, `TCP_NODELAY`, keepalive. A dropped connection is reported as EOT, so BMD detaches |
| Monitor command | `port/platform_cmds.c` | `monitor swd_info`, see below |
| libc | `port/libc_compat.[ch]` | IDF 6 uses PicolibC. BMD's `stdio_newlib.h` needs `siprintf` & co.: prototypes are force-included and thin wrappers compiled when `CONFIG_LIBC_PICOLIBC=y` |
| GDB loop | `main/gdb_server.c` | BMD's `main.c` loop, reimplemented so it can be left later for the streaming mode |
| Network | `main/wifi.c` | STA from menuconfig. After 15 s without IP, falls back to AP `mvprobe` (192.168.4.1). `WIFI_PS_NONE`. mDNS `mvprobe.local` + `_gdb._tcp` |
| Tasks | `main/app_main.c` | Wi-Fi and lwIP on core 0 (sdkconfig). The SWD owner task `probe` is pinned to core 1 (12 KB stack) |

Deviation from the plan: the GDB transport calls lwIP sockets directly from the SWD owner task
on core 1. The TCP/IP processing itself still runs in the tcpip task on core 0. A separate
`net_task` only becomes necessary in Stage 2, when the acquisition loop must never block on
the network.

## Wiring (Waveshare ESP32-S3-Pico)

| Signal | GPIO | Header pin |
|---|---|---|
| SWDIO | 11 | 1 (GP0) |
| SWCLK | 12 | 2 (GP1) |
| GND | – | 3 |
| nRST (optional) | 14 | 5 (GP3) |

## Build and flash

```
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd firmware
idf.py build
idf.py -p COMx flash monitor
```

Wi-Fi credentials go in `firmware/sdkconfig.local` (git-ignored). See the README.

Expected boot log lines:

```
I (...) wifi: station IP 192.168.x.y          (or: access point "mvprobe" up, IP 192.168.4.1)
I (...) wifi: mDNS hostname mvprobe.local
I (...) mvprobe: starting SWD owner task on core 1
I (...) swd_tap: SWCLK GPIO12, SWDIO GPIO11 on core 1, max SWCLK ... Hz
I (...) gdb_if: GDB server listening on TCP port 2022
```

## Acceptance test

```
gdb-multiarch
(gdb) target extended-remote mvprobe.local:2022
(gdb) monitor swd_scan          # expect: "1 STM32F446" (or similar)
(gdb) attach 1
(gdb) x/8x 0x20000000
(gdb) file <firmware.elf>
(gdb) load
(gdb) monitor swd_info
(gdb) monitor frequency 1M      # then 8M, 12M ...; swd_info after each
```

If `mvprobe.local` does not resolve (mDNS on Windows), use the IP address from the boot log.

## `monitor swd_info`

Reports:
- the nominal clock (CPU cycles per half period) and the fastest clock the loop reaches
  (calibrated at boot with the pins released);
- the clock measured inside the tap primitives (clocks / CPU cycles);
- with a Cortex-M target scanned or attached, 1000× a raw DPIDR read (one SWD transaction,
  54 clocks in BMD) and 1000× a 32-bit memory read of CPUID through BMD's generic MEM-AP path
  (5 transactions). Both are reported in µs per operation.

## Measurements to fill in

| Setting | swd_info nominal | swd_info measured | Analyzer SWCLK | DPIDR read [µs] | mem read32 [µs] | Analyzer: one transaction [µs] |
|---|---|---|---|---|---|---|
| 1 MHz | | | | | | |
| 4 MHz | | | | | | |
| 8 MHz | 8 000 000 (half period 15 cycles) | 6 817 615 | 8 MHz | 10.19 | 51.62 | |
| max | | | | | | |

Expected wire time for one 54-clock transaction at 4 MHz is 13.5 µs. The difference to the
measured DPIDR time is software overhead per transaction.

Reading the 8 MHz results:
- **The bit loop itself is exact.** The analyzer shows 8 MHz.
- **The "measured inside tap primitives" figure is lower (6.8 MHz)** because it also counts the
  fixed cost of each primitive call: entry, turnaround and statistics.
- **One transaction takes 10.19 µs.** 54 clocks at 8 MHz is 6.75 µs of wire time, so about
  **3.4 µs per transaction is software overhead**. That overhead is BMD's
  `adiv5_swd_raw_access` (a `platform_timeout_set()` call on every transaction, function
  pointers, code in flash) plus the tap call overhead.
- **A 32-bit memory read through BMD costs 51.62 µs**, which is 5 transactions at about
  10.3 µs each, consistent with the cost model in `stage0.md` §7.
- **Projection for the Stage-4 example sample** (W≈11, K=4) at 8 MHz:
  - generic BMD path, 35 transactions: ≈ 360 µs (≈ 2.8 kHz);
  - optimized hot loop, 19 transactions, no idle cycles, less overhead: ≈ 110 µs wire time.

## Known limitations and things to watch

- **Maximum speed is untested.** At `half_period = 0`, the SWDIO sample point is only a few
  CPU cycles after the falling edge, plus the GPIO-matrix input synchroniser. Validate any
  setting above ~8 MHz with the analyzer before trusting it.
- **The status LED is not driven.** The RGB LED on GPIO21 needs a WS2812 driver (RMT).
- **The fallback AP is open** unless `CONFIG_MVPROBE_AP_PASSWORD` is set (≥ 8 characters).
- **No target voltage sense** (`Target voltage: Unknown`).
- **Disconnect behavior.** BMD halts and then detaches the target when the GDB client
  disconnects while the target is running. This is BMD's normal behavior, and detach resumes
  the core.
