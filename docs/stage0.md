# Stage 0 — Reconnaissance report

> Paths are those of the time. Since 2026-10-24 the firmware lives in `App/`, `Platform/` and
> `External/blackmagic`, in C++ (see the decision log in [plan.md](plan.md)).

Scope: decide how to build a wireless SWD probe on ESP32-S3 out of Black Magic Debug (BMD),
with a non-halting memory streaming mode for MCUViewer and a switchable GDB mode.
No firmware was written in this stage.

## 0. Sources examined

| Repo | Commit | Notes |
|---|---|---|
| BMD mainline `codeberg.org/blackmagic-debug/blackmagic` | `197cee92` (2026-10-15, `v2.1.0-25-g197cee92`) | Latest release tag: `v2.1.0` = `449730f8` (2026-08-19) |
| `github.com/flipperdevices/blackmagic-esp32-s2` | `9447ee92` (2023-09-27) | Uses BMD fork `flipperdevices/blackmagic` @ `1cdc9147` (pre-meson era) |
| `github.com/walmis/blackmagic-espidf` | `2f58e68c` (2024-01-04) | **ESP8266** (ESP8266_RTOS_SDK, legacy Make), BMD @ `fdf37dbf` (blacksphere, ~2020) |

All BMD paths below are relative to the BMD repo root at `197cee92`. Between `v2.1.0` and
`197cee92`, the only changes in the files cited here are additive: `adiv5.c` gains 11 lines
(RISC-V DMI gateway probe), `cortexm.c` gains 38 (`mem_nohalt` command). Line numbers past
those insertions shift slightly on `v2.1.0`.

## 1. TL;DR

1. **Mainline BMD as a submodule + our own ESP platform layer is the right call.** Both ESP
   forks are years behind mainline (renamed files and APIs, for example `adiv5_swdp.c` →
   `adiv5_swd.c` and `cortexa.c` → `cortexar.c`). Walmis' fork is ESP8266-only. They are
   useful only as references.
2. **`monitor swd_scan` (`adiv5_swd_scan()`) is intrusive.** It powers the debug domain down
   and up, **halts the core**, writes DEMCR (`TRCENA | VC_HARDERR | VC_CORERESET`), writes
   CPACR and (on STM32F4) DBGMCU, then resumes the core with `C_DEBUGEN` left set (§6.2).
   For streaming we must **not** use the BMD scan. Instead we need a "light connect" built from
   BMD's public low-level DP/AP functions, following the pattern of `src/remote.c` (§6.3).
3. **Reading memory without attach/halt works** with
   `adiv5_mem_read(ap, …)` on a DP/AP pair we construct ourselves. No `target_s` is needed and
   nothing is written to the core (§6.3).
4. **BMD's generic memory read costs `3K + 2W` SWD transactions per sample**: the AP read is not
   pipelined and CSW is rewritten per block. Every transaction also carries 8 idle clocks. The
   Stage-4 hot loop can reach `W + 2K` (§7).
5. **The exception mechanism is a single global setjmp chain.** An uncaught exception calls
   `abort()`, which on ESP means panic and reboot. Exactly one task may own BMD and SWD, and
   every entry into BMD must be wrapped in `TRY`.
6. **ESP-IDF is not installed on this machine.** `C:\Espressif\tools\eim_idf.json` shows
   `"idfInstalled": []`. A version has to be chosen (§10, question 1).

## 2. How mainline BMD builds (meson) and what CMake must reproduce

Build entry points: `meson.build`, `src/meson.build`, `src/target/meson.build`,
`src/platforms/meson.build`, `meson_options.txt`.

### 2.1 Generated files

| Generated | How (meson) | Consumers | CMake equivalent |
|---|---|---|---|
| `version.h` | `vcs_tag()` in `meson.build:53` runs `git describe --always --dirty --tags` and fills `src/include/version.h.in` (`#define FIRMWARE_VERSION "@VCS_TAG@"`). Falls back to the project version `2.1.0` | `src/command.c:36`, `src/remote.c:41` | `execute_process(git describe …)` in the submodule dir + `configure_file(version.h.in … @ONLY)` into the component binary dir, added to the include path |
| `flashstub/*.stub` | `src/target/flashstub/meson.build` rebuilds them only when `objcopy` and `hexdump` are found | `#include "flashstub/*.stub"` in `efm32.c`, `lmi.c`, `mspm0.c`, `rp2040.c` | **Nothing to do.** The `.stub` files are committed in git (`git ls-files src/target/flashstub`), and the include resolves against the source dir |

No `config.h` is generated. All other configuration is passed as `-D` flags.

### 2.2 Compile definitions to reproduce

| Define | Origin in meson | Value for us |
|---|---|---|
| `CONFIG_POINTER_SIZE` | `meson.build` (`sizeof(void *)`) | `4` |
| `CONFIG_CORTEXM=1` | `src/target/meson.build` `target_cortexm` | required |
| `CONFIG_STM=1` (+ `CONFIG_GD32`, `CONFIG_NRF`, …) | per target family | at least `STM` (the NUCLEO is an F446). See §3.3 |
| `ENABLE_RTT=1` | `rtt_support` option (default on) | **off in Stage 1** (needs `rtt_if_*` platform hooks) |
| `ADVERTISE_NOACKMODE=1` | `advertise_noackmode` (default on) | on |
| `ENABLE_DEBUG=1` | `debug_output` (default off) | Kconfig option. Routes `DEBUG_*` to `PLATFORM_PRINTF` |
| `NO_LIBOPENCM3` | not in meson. Platforms define it themselves | **required**, otherwise `include/gdb_if.h:24` and `include/rtt_if.h:37` pull in `<libopencm3/usb/usbd.h>` |
| `CONFIG_BMDA` | only `-DCONFIG_BMDA=1` for the hosted build | leave undefined (`#if CONFIG_BMDA == 0` is then true) |
| `-std=c11` | `default_options` | `gnu11`/`gnu17` (`general.h` defines `_GNU_SOURCE` anyway) |

The meson `probe` option only selects `src/platforms/<probe>/meson.build` →
`probe_host` dependency. Our ESP component replaces `probe_host`. The `libopencm3` subproject is
not needed.

### 2.3 Toolchain and libc pitfalls (to verify in Stage 1)

- `src/include/platform_support.h` includes `stdio_newlib.h` when `CONFIG_BMDA == 0`. That
  header `#define`s `sprintf → siprintf`, `snprintf → sniprintf`, `vasprintf → vasiprintf`
  and `sscanf → siscanf`. **ESP-IDF ≥ 6.0 switched the default libc to PicolibC**
  (6.0 migration guide). If those symbols are missing, either select `CONFIG_LIBC_NEWLIB=y` or
  provide four thin wrappers in the platform layer.
- `src/target/target_probe.c` defines **unconditional weak aliases** for every vendor probe
  (`TARGET_PROBE_WEAK_NOP(stm32f4_probe)` etc.). meson links objects directly. ESP-IDF links
  components as **static archives**, and there the archive member picked to resolve
  `stm32f4_probe` can be the weak no-op, depending on order. The symptom would be
  `swd_scan` finding a DP but no STM32. Mitigation: `idf_component_register(... WHOLE_ARCHIVE)`.
- BMD uses `CONFIG_*` macros (`CONFIG_STM`, `CONFIG_RP`, `CONFIG_NRF`, `CONFIG_RISCV`, …).
  ESP-IDF's `sdkconfig.h` also uses the `CONFIG_` namespace. Keep `sdkconfig.h` (and any ESP
  header) out of the `platform.h` that BMD sees, and check for collisions at build time.
- `alloca()` is used in `gdb_main.c` (lines 129/166/419, up to ~1 KB for memory reads),
  `command.c:182` and recursively in `target/adi.c:801` (ROM table walk). The task running
  BMD needs a generous stack (start with 12 KB and measure the high-water mark).

## 3. Minimal BMD source set

Paths are relative to `src/`.

### 3.1 Core (from `libbmd_core_sources`, `src/meson.build`)

| File | Needed | Why |
|---|---|---|
| `command.c` | yes | monitor commands (`swd_scan`, `frequency`, …) |
| `crc32.c` | yes | `compare-sections`. The generic impl is used when no `STM32Fx` is defined |
| `exception.c` | yes | TRY/CATCH |
| `gdb_main.c`, `gdb_packet.c`, `hex_utils.c` | yes | GDB server |
| `remote.c` | yes | `gdb_packet.c:148` calls `remote_packet_process()` |
| `maths_utils.c`, `morse.c`, `timing.c` | yes | parity, error morse, `platform_timeout_*` |
| `main.c` | **no** | Defines `int main(void)` with an endless loop. We reimplement its ~40-line `bmp_poll_loop` in our GDB task so the loop can exit on a mode switch |
| `rtt.c` | later | needs `rtt_if.h` hooks (`rtt_if_init`, `rtt_write`, `rtt_getchar`, `rtt_nodata`, …) |

### 3.2 Target common (`target_common_sources`, `src/target/meson.build`)

`adi.c adiv5.c adiv5_jtag.c adiv5_swd.c adiv6.c gdb_reg.c jtag_devs.c jtag_scan.c
onboard_flash.c semihosting.c sfdp.c spi.c target.c target_flash.c target_probe.c`

- `adiv6.c` is required: `adiv5_dp_init()` jumps to `adiv6_dp_init()` for DPv3+.
- `adiv5_jtag.c`, `jtag_scan.c` and `jtag_devs.c` are referenced by `command.c`
  (`jtag_scan`). They need a `jtag_proc` and a `jtagtap_init()`. We provide a stub with no JTAG
  pins, so the scan finds nothing.
- `onboard_flash.c`, `spi.c` and `sfdp.c` are referenced by the unconditional `spi_scan`
  command. They need `platform_spi_*` stubs that return `false`/`0`.
- `jtagtap_generic.c`/`swdptap_generic.c` are only for `no_own_ll`. Not used.

### 3.3 Cortex-M and vendor support

- `cortex.c` + `cortexm.c` (`-DCONFIG_CORTEXM=1`).
- STM32 (`-DCONFIG_STM=1`): `stm32_common.c stm32f1.c stm32f4.c stm32g0.c stm32h5.c
  stm32h7.c stm32h7rs.c stm32l0.c stm32l4.c stm32mp15.c stm32wb0.c`.
- Proposal: mirror the meson `targets` option in Kconfig. Default to all Cortex-M families
  (`at32f4 ch32 ch579 efm gd32 hc32 lpc mm32 nrf nxp puya renesas(ra) rp sam stm ti
  apollo3`). Flash size on the S3 is not a constraint. RISC-V, Cortex-A/R, Lattice and Xilinx
  stay off at first.

## 4. Platform interface BMD requires

Collected by grepping the core and target sources for every `platform_*`, `gdb_if_*` and
platform macro.

### 4.1 `platform.h` (included by `general.h` users)

- `PLATFORM_IDENT`, `SET_RUN_STATE(x)`, `SET_IDLE_STATE(x)`, `SET_ERROR_STATE(x)`.
- `NO_LIBOPENCM3` (see §2.2), and optionally `PLATFORM_HAS_CUSTOM_COMMANDS` +
  `platform_cmd_list`. This adds our own `monitor` commands (SWD timing report, mode switch)
  **without patching BMD** (`command.c:200-205`, `243-247`).
- Only if we reuse `platforms/common/swdptap.c`: `gpio_set/gpio_clear/gpio_get/gpio_set_val`,
  `SWDIO_MODE_FLOAT()/SWDIO_MODE_DRIVE()`, `SWCLK_PORT/PIN`, `SWDIO_PORT/PIN`, **and
  `TDI_PORT/TDI_PIN`** (`swdptap_init()` does `gpio_set(TDI_PORT, TDI_PIN)`). We plan our own
  tap instead (§8), so most of these become irrelevant.

### 4.2 Functions and globals the platform must define

| Symbol | Declared in | Notes |
|---|---|---|
| `platform_time_ms()` | `include/timing.h` | `platform_timeout_set/is_expired` are in core `src/timing.c` (Flipper's old BMD needed them in the platform; mainline does not) |
| `platform_delay(ms)` | `platform_support.h` | `vTaskDelay` |
| `platform_init()` | `platform_support.h` | called by us, not by BMD (we drop `main.c`) |
| `platform_nrst_set_val/get_val` | `platform_support.h` | nRST GPIO (open-drain) |
| `platform_target_voltage()` | `platform_support.h` | must return a string. Contract: return `"Unknown"` if there is no ADC. Flipper returned `NULL`, which goes into a `%s` |
| `platform_hwversion()` | `platform_support.h` | `0` |
| `platform_max_frequency_set/get` | `platform_support.h` | used by `monitor frequency`. `get` must report the **real** clock |
| `platform_target_clk_output_enable(bool)` | `platform_support.h` | drive or release SWCLK/SWDIO |
| `platform_spi_init/deinit/chip_select/xfer` | `platform_support.h` | stubs |
| `uint32_t target_clk_divider` | `include/timing.h` (extern) | defined by the platform (cf. `platforms/common/stm32/timing_stm32.c:34`) |
| `swd_proc_s swd_proc` + `swdptap_init()` | `include/swd.h` | 4 callbacks: `seq_in`, `seq_in_parity`, `seq_out`, `seq_out_parity` |
| `jtag_proc_s jtag_proc` + `jtagtap_init()` | `include/jtagtap.h` | stub |
| `gdb_if_init/getchar/getchar_to/putchar/flush` | `include/gdb_if.h` | see §4.3 |
| `debug_serial_send_stdout()` | `general.h` | only with `PLATFORM_HAS_DEBUG` |
| `platform_target_get/set_power`, `platform_target_voltage_sense` | | only with `PLATFORM_HAS_POWER_SWITCH` (no) |

### 4.3 `gdb_if` contract

Derived from `src/gdb_packet.c:129-135`, `src/gdb_main.c:324-342`,
`src/platforms/hosted/gdb_if.c:283-375` and `src/platforms/ctxlink/ctxlink_gdb_if.c`:

- `gdb_if_getchar()` blocks. Returning `'\x04'` (EOT) means "connection closed": `gdb_main`
  detaches `cur_target` and resets no-ack mode.
- `gdb_if_getchar_to(ms)` returns `(char)-1` on timeout. The poll loop calls it with `0` while
  the target runs, to catch `^C`.
- `gdb_if_putchar(c, flush)` buffers, and `flush=true` sends. The hosted implementation sends
  with `TCP_NODELAY` set on the socket.
- Mainline already has two TCP/Wi-Fi references: BMDA's TCP server
  (`src/platforms/hosted/gdb_if.c`) and **ctxLink**, an in-tree STM32F4 + WINC1500 Wi-Fi probe
  (`src/platforms/ctxlink/ctxlink_gdb_if.c`, `WiFi_Server.c`).

### 4.4 How the ESP forks did it

**Flipper (ESP32-S2)** — `components/blackmagic/`:
- `CMakeLists.txt` lists BMD sources explicitly, compiles the old `main.c`, and passes
  `-DFIRMWARE_VERSION="$(git describe)"` and `-DPC_HOSTED=0` (the old name for `CONFIG_BMDA`).
- `esp32-platform/platform.h`/`platform.c` map `gpio_set/clear/get` to direct
  `GPIO.out_w1ts/out_w1tc/in` register writes (pins < 32 only). `SWDIO_MODE_DRIVE` calls
  `esp_rom_gpio_connect_out_signal()` on every turnaround, which is slow. Bitbanging goes
  through BMD's generic `swdptap.c`. `platform_max_frequency_set()` is empty, so there is
  **no clock control**.
- GDB over TCP (port 2345): `main/network-gdb.c` (accept loop, `TCP_NODELAY`, keepalive,
  `esp_wifi_set_ps(WIFI_PS_NONE)`) → `esp32-platform/gdb-glue.c` (a FreeRTOS `StreamBuffer`
  between the socket task and the BMD task). `gdb_if_getchar_to(timeout)` passes milliseconds
  as ticks, which is wrong unless the tick rate is 1 kHz.
- SPI-based SWD was attempted and disabled. `custom/swd-spi-tap.c` header: *"Does not work due
  to bug with switching 3-wire mode to RX"* (esp-idf#7800). This is relevant for Stage 4.

**walmis (ESP8266)** — reference only:
- Legacy `component.mk` compiles whole BMD directories minus an exclusion list.
- Replaces `exception.c/h` with a **per-task** chain stored in FreeRTOS TLS
  (`components/blackmagic/exception.h`: `#define innermost_exception
  (*get_innermost_exception())`, `main/gdb_if.cpp:91-97`). This allows several GDB clients.
- Rewrote `gdb_main`/`gdb_packet`/`gdb_if` in C++. Uses port 2022 with `TCP_NODELAY`.
- SWD clock via a calibrated delay loop (`main/platform.c:65-84`, `SWD_CYCLES_PER_CLOCK=19`,
  `SWD_TOTAL_CYCLES=127` at 160 MHz).

## 5. Error handling (TRY/CATCH) and what it means for the acquisition loop

Mechanism (`src/include/exception.h`, `src/exception.c`):
- `TRY(mask)` pushes a frame onto the **global** `innermost_exception` and calls `setjmp`.
  `raise_exception()` walks the chain and `longjmp`s to the first frame whose mask matches.
- **If no frame matches, it calls `abort()`.** On ESP-IDF that is a panic and reboot.
- There is a single global chain, so it is not task-safe. Locals modified inside `TRY` must be
  `volatile`.

What raises and what does not, in `src/target/adiv5_swd.c` `adiv5_swd_raw_access()`
(line 406):

| SWD outcome | BMD behaviour |
|---|---|
| ACK WAIT | retried in a loop until a **250 ms** timeout (`platform_timeout_set(&timeout, 250U)`), then `DAPABORT`, `dp->fault = WAIT`, returns 0. No exception |
| ACK FAULT | writes ABORT (clears all sticky bits) and **retries in the same 250 ms loop**. Then `dp->fault = FAULT`, returns 0. No exception |
| no response (`0b111`) | `dp->fault = 7`, returns 0. Callers going through `adiv5_dp_recoverable_access()` (`adiv5_interface.h:151`), such as SELECT writes and `dp_read`, do one line-reset recovery and then `raise_exception("SWD invalid ACK")` |
| other invalid ACK | `raise_exception(EXCEPTION_ERROR, "SWD invalid ACK")` |
| read parity error | `dp->fault = 1`, `raise_exception(EXCEPTION_ERROR, "SWD parity error")` |
| any AP access while `dp->fault != 0` | **returns 0 immediately, without bus traffic** (`if ((addr & ADIV5_APnDP) && dp->fault) return 0;`) |

Recovery is `adiv5_dp_error(dp)` → `adiv5_swd_clear_error(dp, false)` (`adiv5_swd.c:356`). It
reads CTRL/STAT, writes ABORT for the sticky bits that are set, and clears `dp->fault`. For
DPv2+ with a fault, or when protocol recovery is requested, it first does a line reset plus
TARGETSEL and DPIDR. `target_check_error()` → `cortex_check_error()` is the same call.

Consequences for `acq_task`:
1. **A sample is atomic.** After the plan, check `dp->fault`. If it is set, the rest of the
   sample silently read zeros: discard the sample and call `adiv5_dp_error()`.
2. Put one `TRY(EXCEPTION_ALL)` around a whole sample, not around each transaction. setjmp
   on the Xtensa windowed ABI spills register windows, so its cost needs measuring. In `CATCH`:
   count the error, recover, and after N consecutive errors do a light reconnect (§6.3), never
   `swd_scan`.
3. **A bad address in the plan can stall ~250 ms per access** because FAULT is retried. The
   firmware must validate the plan. The Stage-4 hot loop should handle WAIT and FAULT itself
   with a short budget, raise an `EVENT` and move on.
4. Exactly one task executes BMD code at any time (see §9). The walmis TLS approach is not
   needed if we keep it that way.

## 6. Reading target memory without `target_attach` (no halt)

### 6.1 The GDB-side path (works, but sits behind an intrusive scan)

`target_mem32_read(t, …)` (`target/target.c:395`) → `target_mem64_read` →
`t->mem_read = cortexm_mem_read` (`cortexm.c:233`) → `cortexm_cache_clean()` (no-op unless
the D-cache is known to be enabled) → `adiv5_mem_read(cortex_ap(t), …)` → `target_check_error()`.

None of this needs `target_attach()`. BMD itself marks STM32F4 with
`TOPT_NON_HALTING_MEM_IO` (`stm32f4.c:491`) and uses this for RTT. But the `target_s` only
exists after `adiv5_swd_scan()`, and that scan is intrusive.

### 6.2 Side effects of `adiv5_swd_scan()` on a Cortex-M / STM32F4

Call chain: `cmd_swd_scan` → `adiv5_swd_scan()` (`adiv5_swd.c:158`) → `adiv5_dp_init()`
(`adiv5.c:370`).

1. `adiv5_power_cycle_aps()` (`adiv5.c:333`): CTRL/STAT ← 0, waits for the ACKs to drop, then
   ← `CSYSPWRUPREQ|CDBGPWRUPREQ`.
2. For an AHB3 AP0: `cortexm_prepare()` (`adiv5.c:177`) → `cortexm_initial_halt()`
   (`adiv5.c:93`):
   - writes DHCSR `C_DEBUGEN|C_HALT` in a loop **until the core halts**;
   - saves DEMCR into `ap->ap_cortexm_demcr`, then writes `TRCENA|VC_HARDERR|VC_CORERESET`
     (`adiv5.c:193-195`).
3. ROM table walk → `cortexm_probe()` writes **CPACR |= CP10/CP11 full access** to detect the
   FPU, and does not restore it (`cortexm.c:313-316`).
4. `stm32f4_probe()` → `stm32f4_configure_dbgmcu()` (`stm32f4.c:268-278, 307`): DBGMCU_CR
   gets `DBG_SLEEP|DBG_STOP|DBG_STANDBY`, and APB1_FZ gets `WWDG|IWDG`.
5. `adi_ap_resume_cores()` (`adi.c:506`) → `cortexm_halt_resume()` (`cortexm.c:963`): DHCSR ←
   `DBGKEY|C_DEBUGEN`. The core runs again, but **halting debug stays enabled**.
6. DEMCR is only restored in `cortexm_detach()` (`cortexm.c:610-629`).
   `target_list_free()` calls it only for **attached** targets (`target.c:142-150`). Note that
   `cortexm_detach` itself briefly sets `C_HALT`.

Effect on a running application:
- The core is stopped for milliseconds, which matters for motor control or comms stacks.
- Afterwards `C_DEBUGEN=1` with `VC_CORERESET|VC_HARDERR`, so the next local reset
  (`NVIC_SystemReset`) **halts at the reset vector**. DEMCR survives everything except a
  power-on reset, and **HardFault halts the core** instead of running its handler, as does
  `BKPT`. A streaming-only probe must not leave the target in this state.

### 6.3 The streaming path: "light connect" plus direct MEM-AP reads (recommended)

BMD itself does this in `src/remote.c`. It keeps a static `adiv5_debug_port_s remote_dp`
whose function pointers are set to the SWD implementations (`remote.c:124-142`: `adiv5_swd_*`,
`adiv5_ap_reg_read/write`, `adiv5_mem_read_bytes`). It builds an `adiv5_access_port_s` on the
stack with `apsel`, `csw` and `dp`, then calls `adiv5_mem_read(&ap, …)` directly
(`remote_packet_process_adiv5()`, `remote.c:469-580`). There is no `target_s`, no scan and no halt.

Proposed call path for `CONNECT` in streaming mode (all public BMD API, nothing patched):

1. `swdptap_init()` (our tap) and `platform_target_clk_output_enable(true)`.
2. Wake-up sequence: line reset, JTAG-to-SWD and dormant-to-SWD. BMD's versions are `static`
   in `adiv5_swd.c:58-135` (`swd_line_reset_sequence`, `dormant_to_swd_sequence`,
   `jtag_to_swd_sequence`), so we implement these standard ADIv5.2 sequences in the platform
   layer using `swd_proc.seq_out`.
3. `adiv5_dp_read_dpidr(dp)` (`adiv5.c:268`). With `dp->read_no_check` set, as it is for
   SWD, this is a plain read that never raises. Set `dp->version` from
   DPIDR. For DPv2, read TARGETID and set `dp->targetsel` (needed by
   `adiv5_swd_clear_error`).
4. `adiv5_dp_abort(dp, STKERRCLR|…)`.
5. `adiv5_dp_write(dp, ADIV5_DP_CTRLSTAT, CSYSPWRUPREQ|CDBGPWRUPREQ)` and poll
   `adiv5_dp_read(dp, CTRLSTAT)` for both ACKs. This deliberately skips the power-down step of
   `adiv5_power_cycle_aps()`.
6. `adiv5_new_ap(dp, apsel)` (`adiv5.c:216`) → `adi_configure_ap()` (`adi.c:486`). This only
   reads AP registers (IDR, CFG, CSW, BASE), computes a sane `ap->csw` (DbgSwEnable,
   MasterType, HPROT data/privileged, HNONSEC from SPIDEN) and touches no target memory. The
   result gives the DPIDR and AP IDR for the `CONNECT` response.
7. Per sample: for each plan run, `adiv5_mem_read(ap, dst, addr, len)`. Then check
   `dp->fault` and call `adiv5_dp_error(dp)` if needed.

Writes to target state on this path: **none** (DP/AP registers only). The optional CYCCNT
enable (DEMCR.TRCENA, DWT_CTRL.CYCCNTENA) will be an explicit, read-modify-write opt-in in
Stage 2.

Limitations to keep in mind:
- ADIv6 (DPv3) uses a different AP addressing scheme (`adiv6.c`). The light connect starts as
  ADIv5-only.
- DPv2 multidrop targets need TARGETSEL after each line reset.
- Debugger access while the target sleeps: on STM32, AHB-AP accesses during Sleep/Stop can stall
  or fail unless DBGMCU `DBG_SLEEP`/`DBG_STOP` are set. Whether the probe may set them
  (vendor-specific, and a state change) is an open question for Stage 2.
- Switching from GDB mode to streaming after a GDB scan or attach should undo §6.2: detach
  attached targets, restore DEMCR from `ap->ap_cortexm_demcr`, clear `C_DEBUGEN`. Design
  this in Stage 2.

## 7. Cost model of BMD's generic memory read

Clocks per transaction in `adiv5_swd_raw_access()` with the generic `swdptap.c`: 8 (request)
+ 1 (turnaround) + 3 (ACK) + 33 (data and parity) + 1 (turnaround) + **8 idle**
(`swd_proc.seq_out(0, 8U)`, "favour correctness") = **54 clocks**. The protocol minimum is 46.

`adiv5_mem_read_bytes()` (`adiv5.c:658`) for one block of W aligned words:
- `adi_ap_mem_access_setup()` (`adi.c:522`): `adiv5_ap_write(CSW)` = SELECT write + CSW write,
  plus a TAR write. That is **3**.
- Per word: `adiv5_dp_read(DRW)` → `adiv5_swd_read()` (`adiv5_swd.c:347`) = AP read + RDBUFF
  read. That is **2W**, with no pipelining.
- Plus `adiv5_dp_error()` per sample (1 CTRL/STAT read).

So a sample costs **3K + 2W (+1)** transactions with the generic path, against **W + 2K** for a
hot loop with CSW and SELECT set once, TAR per block, and posted DRW reads.

Example from the Stage-4 goal (10 variables in 3 clusters + CYCCNT, so W≈11, K=4), wire time
only, at 4 MHz:

| Path | Transactions | Clocks | Time | Max rate |
|---|---|---|---|---|
| Generic BMD | 35 | 35 × 54 | ≈ 470 µs | ≈ 2.1 kHz |
| Optimized (no idle cycles) | 19 | 19 × 46 | ≈ 220 µs | ≈ 4.5 kHz |

Software overhead is not included. Notably, `adiv5_swd_raw_access` calls
`platform_timeout_set()` (one `platform_time_ms()`) on every transaction. Stage 2 measures
the real numbers.

## 8. ESP32-S3 specifics that shape the design

- **Dedicated GPIO is per core.** ESP-IDF docs (v6.1, `dedic_gpio`): *"Any operations on the
  GPIO bundle should be put inside a task which is running on the same CPU core"*. The task
  that creates the bundle must be pinned, and **all SWD traffic, in both GDB and streaming
  modes, must run on that core** (core 1). Fast access goes through `hal/dedic_gpio_cpu_ll.h`
  (`dedic_gpio_cpu_ll_write_mask()` etc.).
- **SWDIO direction.** Dedicated GPIO drives the output value, not the output enable. The
  turnaround therefore toggles OE through the GPIO peripheral, which is slower than a dedicated
  instruction but happens only twice per transaction. Flipper's per-turnaround
  `esp_rom_gpio_connect_out_signal()` should be avoided. Open-drain plus pull-up is too slow at
  4 MHz. The exact OE handling with a dedic output signal has to be verified against the
  installed IDF source in Stage 1.
- **Own SWD tap, not `platforms/common/swdptap.c`.** BMD's generic tap is built around
  `gpio_set()`-style macros and a `volatile` counter delay. For calibrated, measurable timing
  on the S3, a dedicated `swdptap_esp.c` (same `swd_proc` interface) is cleaner. Timing comes
  from the CPU cycle counter, and the delay is calibrated per frequency.
- **IRAM.** Flash operations such as NVS writes or OTA disable the cache on both cores, so the
  tap and the hot loop should be `IRAM_ATTR`. Since IDF 6.0 most FreeRTOS functions are placed
  in flash by default (`CONFIG_FREERTOS_IN_IRAM` restores IRAM placement), so the hot loop must
  not call into FreeRTOS.
- **Core split.** Wi-Fi and lwIP pinned to core 0 (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`,
  `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`). The idle-task watchdog check on CPU1 is disabled
  for busy-wait pacing. To verify in Stage 1: exact Kconfig names for the chosen IDF version.

## 9. Decision: mainline BMD submodule + own ESP platform layer — **confirmed, with amendments**

Why not the forks: Flipper's BMD base is ~2022, pre-meson, with different file and function
names, `PC_HOSTED`, no clock control, and it targets the S2 with USB and DAP-Link extras.
walmis targets the ESP8266 with legacy Make and a C++ rewrite of the GDB core. Neither gives
us more than ideas.

Amendments:
1. **Streaming never calls `adiv5_swd_scan()`.** It uses the light connect of §6.3 (BMD
   low-level API only). GDB mode keeps the full BMD flow.
2. **One SWD-owner task on core 1 runs either the GDB loop or the acquisition loop**,
   switching at safe points. The reasons: BMD is not reentrant (one global `innermost_exception`,
   `target_list`, `swd_proc`, a static `olddir` in the tap) and dedicated GPIO is per core.
   `net_task` and Wi-Fi run on core 0 and talk to it through queues and stream buffers.
3. **Pin the submodule to a tag.** Proposal: `v2.1.0`. We bump it deliberately.
4. **No patches to the BMD tree.** Everything goes through `platform.h`,
   `PLATFORM_HAS_CUSTOM_COMMANDS`, our own `gdb_if`/tap, and public functions. If a patch ever
   becomes unavoidable, it lives as a `.patch` file in our repo and is applied at configure
   time.

## 10. Proposed repo layout changes

```
firmware/
  components/bmd/                 single component = BMD + our port (avoids circular deps)
    blackmagic/                   git submodule (mainline, pinned)
    port/                         platform.h/.c, swdptap_esp.c, jtagtap_stub.c, gdb_if_tcp.c,
                                  platform_cmds.c, version.h.in handling
    CMakeLists.txt                explicit source list, -D flags, WHOLE_ARCHIVE
    Kconfig                       target families, debug output, SWD pins/frequency
  components/mvstream/            light connect, acq loop, ring buffer (REQUIRES bmd)
  main/                           app_main, wifi, mdns, mode select, net_task
proto/                            PROTOCOL.md + mv_proto.h + LICENSE (MIT); SPDX headers
host/python/                      client, planner, viewer (+ LICENSE — see question 4)
docs/
LICENSE                           GPL-3.0-or-later (note: proto/ is MIT)
```

Rationale: `platform.h` must be on BMD's include path, and the port must see BMD's headers.
Two components would depend on each other, so one component with a `port/` folder is simpler.
Streaming code lives in its own component so it stays independent of GDB plumbing.

## 11. Decisions (resolved 2026-10-23)

1. **ESP-IDF v6.1**, installed with EIM CLI into `C:\Espressif` for target esp32s3.
2. **Hardware: Waveshare ESP32-S3-Pico** (ESP32-S3R2: 2 MB quad PSRAM in package, 16 MB
   flash, CH343 USB-UART for the console). Pin choice is left to the firmware:
   - SWDIO = **GPIO11** (header GP0, physical pin 1), SWCLK = **GPIO12** (GP1, pin 2),
     GND on pin 3, nRST = **GPIO14** (GP3, pin 5).
   - GPIO11/12 are the FSPID/FSPICLK IO_MUX pins, so an SPI-based SWD (Stage 4) can use the
     same wiring. GPIO13 (FSPIQ, GP2) is kept free for that.
   - All are below 32: SWDIO direction switching uses `GPIO_ENABLE_W1TS/W1TC`.
   - Avoided: strapping pins 0/3/45/46, flash/PSRAM pins 26–37, USB 19/20, UART0 43/44 and
     the RGB LED on GPIO21.
   - All three are configurable in menuconfig.
3. **BMD pinned to `v2.1.0`** (release, `449730f8`). Bump deliberately.
4. **Licensing as forced by dependencies:**
   - `firmware/` is GPL-3.0-or-later because of BMD. ESP-IDF (Apache-2.0) and mdns
     (Apache-2.0) are compatible with it.
   - `proto/` and `host/python/` are **MIT**. They contain no BMD code. The host viewer must
     use **PySide6 (LGPL)** rather than PyQt (GPL) behind pyqtgraph. pyelftools is public
     domain, and numpy and pyqtgraph are permissive.
5. **All Cortex-M families** are enabled: apollo3, at32f4, ch32, ch579, efm, gd32, hc32, lpc,
   mm32, nrf, nxp, puya, renesas (RA), rp, sam, stm, ti.

## 12. Risks

| Risk | Mitigation |
|---|---|
| Weak no-op probe stubs win in a static archive (§2.3) | `WHOLE_ARCHIVE`. Acceptance test: `swd_scan` names the STM32F446 |
| PicolibC lacks `siprintf` & co. | `CONFIG_LIBC_NEWLIB` or wrappers |
| `CONFIG_*` macro collisions BMD ↔ sdkconfig | keep ESP headers out of BMD-visible `platform.h`, grep at build time |
| Uncaught BMD exception reboots the probe | every BMD entry point wrapped in `TRY(EXCEPTION_ALL)` |
| 250 ms WAIT/FAULT retry inside BMD stalls streaming | plan validation now, own fault handling in the Stage-4 hot loop |
| Bitbang timing jitter from interrupts on core 1 | SWD is synchronous, so jitter lowers throughput but not correctness. Measure in Stage 1 |
| Debug access to a sleeping STM32 | open question for Stage 2 (DBGMCU opt-in) |
