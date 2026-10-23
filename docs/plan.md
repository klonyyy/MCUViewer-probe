# MVProbe — plan and status

Last updated: 2026-10-23

## Goal

A wireless SWD probe on the ESP32-S3 for MCUViewer, built on Black Magic Debug (BMD).

- **Streaming mode (main purpose).** The host sends a read plan: a list of contiguous memory
  blocks. The probe executes it in a loop over SWD through the MEM-AP, without attaching to
  or halting the core, and streams the samples to the host over an open TCP protocol.
- **GDB mode (secondary, switchable).** BMD's GDB server, for validation and flashing.

## Working agreement

- **Stages.** Work in the stages below. After each stage, stop and summarize: what was done,
  what to test on hardware, and which decisions are open. The next stage starts only after
  the maintainer's OK.
- **No guessing APIs.** Do not guess the BMD API or the structure of other repositories. Read
  the source and cite the paths and functions you rely on.
- **English everywhere.** Code, comments, commits and docs are in English.
- **Commits.** Small, logical commits. Secrets (Wi-Fi credentials) live only in the
  git-ignored `firmware/sdkconfig.local` or in menuconfig.
- **Licensing.**
  - `firmware/` is GPL-3.0-or-later, because it links BMD.
  - `proto/` (`PROTOCOL.md`, `mv_proto.h`) and `host/` are MIT. They are written from scratch
    and contain no code derived from BMD, because they are reused in the closed-source
    MCUViewer.
  - The Python viewer uses PySide6 (LGPL), not PyQt (GPL).
- **No upstream to BMD.** BMD does not accept AI-assisted contributions, so this is a separate
  repository.

## Status

| Stage | State | Report |
|---|---|---|
| 0 — reconnaissance | done | [stage0.md](stage0.md) |
| 1 — BMD GDB server over Wi-Fi | done, works on hardware, SWCLK 8 MHz confirmed with an analyzer | [stage1.md](stage1.md) |
| 2 — streaming in firmware | next | – |
| 3 — protocol + Python client + MCUViewer backend | planned | – |
| 4 — optimization | planned | – |

Baseline measured in Stage 1, at 8 MHz SWCLK through BMD's generic path:
- 10.19 µs per SWD transaction (6.75 µs wire time, ~3.4 µs software overhead);
- 51.62 µs per 32-bit memory read (5 transactions).

## Pending decision before Stage 2

**Order of work.** Choose one:
- **A. As planned:** Stage 2, then Stage 3, then the MCUViewer HSS backend.
- **B. Quick MCUViewer smoke test first:** light connect + `HELLO`/`CONNECT`/`READ`/`WRITE`
  on port 2023 + an MCUViewer backend in NORMAL mode, followed by full streaming and the HSS
  backend.

**MCUViewer sources.** The public repository (commit `4be6621`, 2025-06) may differ from the
closed-source version. Decide whether the backend is written against the closed tree (path
needed) or delivered as a drop-in class built on `proto/mv_proto.h`.

## Stage 2 — streaming mode in firmware (no network protocol yet)

- **Light connect**, see stage0.md §6.3:
  - never `adiv5_swd_scan()`;
  - no halt, no writes to DHCSR, DEMCR or CPACR;
  - DP power-up request without a power cycle;
  - `adiv5_new_ap()` for the AP.
- **One SWD owner task on core 1** runs either the GDB loop or the acquisition loop, switching
  at safe points. When switching from GDB mode, undo GDB-mode side effects: detach attached
  targets, restore DEMCR, clear `C_DEBUGEN`.
- **Optional DWT CYCCNT** on M3 and later: read-modify-write DEMCR.TRCENA (bit 24) and
  DWT_CTRL.CYCCNTENA (bit 0).
- **Plan and pacing:**
  - The plan is a list of `{addr, nwords}`.
  - The loop executes the plan and writes a sample `{t, words[]}` to the ring buffer.
  - `period_us = 0` runs as fast as possible. `period_us > 0` busy-waits on the cycle
    counter; the CPU1 task watchdog check is already disabled.
- **Errors.**
  - One `TRY(EXCEPTION_ALL)` per sample.
  - Check `dp->fault` after every sample, because after a fault the AP reads silently return
    0.
  - Clear sticky errors with `adiv5_dp_error()`, and count errors.
  - After N consecutive errors, do a light reconnect.
  - Validate the plan: aligned to 4 bytes, no block crossing a 1 KB boundary. A faulting
    address otherwise costs about 250 ms per access inside BMD's retry loop.
- **Ring buffer.** Lock-free SPSC, size from Kconfig, in PSRAM (2 MB on the board). On
  overflow, drop whole samples.
- **Hot path.** Hot-path code in IRAM. No FreeRTOS calls in the busy loop (FreeRTOS is placed
  in flash in IDF 6).
- **Statistics** on the UART every second: samples/s, µs per sample, errors, overflows.
- **Test hook.** Without the protocol, start streaming through a monitor command such as
  `monitor stream 0x20000000:4 0x20000100:2` and watch `idf.py monitor`.

## Stage 3 — protocol, Python client, MCUViewer backend

**Protocol** (to be specified in `proto/PROTOCOL.md` and `proto/mv_proto.h`, MIT, versioned):

- **Transport and framing.** TCP port 2023, little-endian. Frame header:
  `{u16 magic = 0x564D 'MV', u8 version = 1, u8 type, u16 len, u16 seq}` + payload.
- **Host → probe:**
  - `HELLO` → firmware version and limits (max runs/words, clock range);
  - `CONNECT(swd_hz, ap_sel, under_reset)` → DPIDR and AP IDR;
  - `SET_PLAN(u32 period_us, u8 nruns, {u32 addr, u16 nwords}[])`;
  - `START`, `STOP`;
  - `READ(addr, nbytes)`, `WRITE(addr, nbytes, data)`;
  - `GET_STATUS`.
- **Probe → host:**
  - `RESP` (status + data);
  - `DATA`;
  - `EVENT` (target lost, overflow, sticky errors).
- **DATA payload:** `{u32 first_sample, u16 nsamples, u16 words_per_sample,
  nsamples × {u32 t, u32 w[]}}`. The sample counter is continuous, so the host can detect
  gaps. Batches go out at about 1400 B or every 10 ms.
- **READ/WRITE** are executed by the acquisition task between samples, through a command
  queue, never in parallel with the plan.
- **Firmware-side plan validation:** addresses aligned to 4, no block crossing a 1 KB
  boundary (TAR auto-increment limit).

**Planner** (Python, later ported to C++ for MCUViewer):

- Input: variables `{addr, size}`. It reads whole aligned words; 8/16-bit values are sliced
  on the host.
- Sample cost = W + 2K transactions (W words, K blocks).
- Merge blocks when the gap is ≤ 1 word, and split at 1 KB boundaries.
- pytest unit tests with edge cases.

**Python client:** connect, `HELLO`/`CONNECT`/`SET_PLAN`/`START`, decode `DATA`, detect gaps,
plot live (pyqtgraph + PySide6), and resolve symbols from the ELF with pyelftools (variable
names → addresses and types).

**MCUViewer backend** (public `IDebugProbe` interface; confirm against the closed-source
tree):

| `IDebugProbe` | Protocol |
|---|---|
| HSS `startAcqusition(addressSizeVector, freq)` | planner → `SET_PLAN(period_us = 1e6 / freq)` + `START` |
| HSS `readSingleEntry()` | next sample from `DATA`: timestamp `t · 1e-6` + address → value map |
| `stopAcqusition()` | `STOP` |
| NORMAL `readMemory()` / `writeMemory()` | `READ` / `WRITE` |

## Stage 4 — optimization (after Stage 2–3 measurements)

- **Dedicated hot loop** instead of the generic `mem_read`: direct DP/AP access, CSW and SELECT
  set once, TAR per block, posted DRW reads (W + 2K transactions), no 8 idle clocks per
  transaction, and own WAIT/FAULT handling with a short budget.
- **SWD on the SPI peripheral**, only if measurements show that bit-banging is the
  bottleneck. GPIO11/12 are already the FSPI IO_MUX pins and GPIO13 (FSPIQ) is kept free.
  Note esp-idf#7800: 3-wire SPI RX switching did not work for the Flipper ESP32-S2 port.
- **Target:** 10 variables in 3 clusters + CYCCNT ≈ 20 transactions ≈ 300 µs → ~3 kHz at
  4 MHz SWD. The Stage-1 projection at 8 MHz is ≈ 110 µs of wire time per such sample.

## Open questions

- **Low-power targets.** May the probe set DBGMCU `DBG_SLEEP`/`DBG_STOP` on STM32, so the
  AHB-AP keeps working while the target sleeps? This would be a vendor-specific opt-in.
- **Fallback AP.** It is open by default. Should a default password be required?
- **Status LED.** WS2812 on GPIO21 via RMT.
- **Target voltage sense.** Needs an ADC pin and a divider.
- **Light connect coverage.** It is ADIv5-only at first. DPv2 multidrop (TARGETSEL) and
  ADIv6 come later.

## Decision log

| Date | Decision |
|---|---|
| 2026-10-23 | Mainline BMD as a pinned submodule (`v2.1.0`) + own ESP platform layer. The ESP forks are reference only |
| 2026-10-23 | Streaming never uses BMD's scan, which halts the core and changes DEMCR, CPACR and DBGMCU. It uses a light connect instead |
| 2026-10-23 | ESP-IDF v6.1. PicolibC kept, with thin `siprintf`-family wrappers for BMD |
| 2026-10-23 | Board: Waveshare ESP32-S3-Pico. SWDIO GPIO11, SWCLK GPIO12, nRST GPIO14 |
| 2026-10-23 | All Cortex-M target families enabled |
| 2026-10-23 | One SWD owner task on core 1. The GDB transport uses lwIP sockets directly from it |
| 2026-10-23 | `host/` and `proto/` MIT, PySide6 for the viewer |
