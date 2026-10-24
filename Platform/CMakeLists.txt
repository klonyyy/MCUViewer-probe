# Black Magic Debug (External/blackmagic, git submodule, pinned) + the ESP32-S3 platform port.
#
# The source lists mirror src/meson.build and src/target/meson.build of BMD v2.1.0.
# Target families: all Cortex-M families from the meson `targets` option.

get_filename_component(bmd_dir "${CMAKE_CURRENT_LIST_DIR}/../External/blackmagic" ABSOLUTE)
set(bmd_src "${bmd_dir}/src")

set(bmd_core_srcs
    command.c
    crc32.c
    exception.c
    gdb_main.c
    gdb_packet.c
    hex_utils.c
    maths_utils.c
    morse.c
    remote.c
    timing.c
)
list(TRANSFORM bmd_core_srcs PREPEND "${bmd_src}/")

set(bmd_target_srcs
    # target_common_sources
    adi.c
    adiv5.c
    adiv5_jtag.c
    adiv5_swd.c
    adiv6.c
    gdb_reg.c
    jtag_devs.c
    jtag_scan.c
    onboard_flash.c
    semihosting.c
    sfdp.c
    spi.c
    target.c
    target_flash.c
    target_probe.c
    # cortexm
    cortex.c
    cortexm.c
    # apollo3
    apollo3.c
    # at32f4
    at32f43x.c
    # ch32 (+ stm32f1 dependency)
    ch32f1.c
    # ch579
    ch579.c
    # efm
    efm32.c
    # hc32
    hc32l110.c
    # lpc
    lpc11xx.c
    lpc15xx.c
    lpc17xx.c
    lpc40xx.c
    lpc43xx.c
    lpc546xx.c
    lpc55xx.c
    lpc_common.c
    # nrf
    nrf51.c
    nrf54l.c
    nrf91.c
    # nxp
    imxrt.c
    kinetis.c
    nxpke04.c
    s32k3xx.c
    # puya
    puya.c
    # renesas (RA only, RZ needs Cortex-A/R support)
    renesas_ra.c
    # rp
    rp2040.c
    rp2350.c
    # sam
    sam3x.c
    sam4l.c
    samd.c
    samx5x.c
    # stm (+ gd32, mm32 reuse stm32f1/stm32f4)
    stm32_common.c
    stm32f1.c
    stm32f4.c
    stm32g0.c
    stm32h5.c
    stm32h7.c
    stm32h7rs.c
    stm32l0.c
    stm32l4.c
    stm32mp15.c
    stm32wb0.c
    # ti
    icepick.c
    lmi.c
    msp432e4.c
    msp432p4.c
    mspm0.c
)
list(TRANSFORM bmd_target_srcs PREPEND "${bmd_src}/target/")

set(port_srcs
    Platform.cpp
    SwdTap.cpp
    JtagTapStub.cpp
    GdbTcpInterface.cpp
    MonitorCommands.cpp
)
if(CONFIG_LIBC_PICOLIBC)
    list(APPEND port_srcs libc_compat.c)
endif()

# version.h: same as meson's vcs_tag() on src/include/version.h.in
execute_process(
    COMMAND git describe --always --dirty --tags
    WORKING_DIRECTORY "${bmd_dir}"
    OUTPUT_VARIABLE VCS_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE vcs_tag_result
)
if(NOT vcs_tag_result EQUAL 0 OR VCS_TAG STREQUAL "")
    set(VCS_TAG "unknown")
endif()
configure_file("${bmd_src}/include/version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

idf_component_register(
    SRCS ${bmd_core_srcs} ${bmd_target_srcs} ${port_srcs}
    INCLUDE_DIRS . "${bmd_src}/include" "${bmd_src}" "${bmd_src}/target"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PRIV_REQUIRES esp_driver_gpio esp_hal_gpio esp_timer lwip log
    # target_probe.c provides weak no-op fallbacks for every vendor probe. Linking the
    # archive whole guarantees that the real probe functions are the ones that get used.
    WHOLE_ARCHIVE
)

# Configuration shared by everything that includes BMD headers
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CONFIG_POINTER_SIZE=4
    NO_LIBOPENCM3=1
    ADVERTISE_NOACKMODE=1
    $<$<BOOL:${CONFIG_MVPROBE_BMD_DEBUG}>:ENABLE_DEBUG=1>
)
# Prototypes for the newlib-only stdio names BMD's stdio_newlib.h maps to
target_compile_options(${COMPONENT_LIB} PUBLIC
    "SHELL:-include \"${CMAKE_CURRENT_LIST_DIR}/libc_compat.h\""
)

# Target family selection (src/target/meson.build), only relevant to BMD's own sources
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_CORTEXM=1
    CONFIG_APOLLO3=1
    CONFIG_AT32=1
    CONFIG_CH32=1
    CONFIG_CH579=1
    CONFIG_EFM32=1
    CONFIG_GD32=1
    CONFIG_HC32=1
    CONFIG_LPC=1
    CONFIG_MM32=1
    CONFIG_NRF=1
    CONFIG_NXP=1
    CONFIG_PUYA=1
    CONFIG_RA=1
    CONFIG_RP=1
    CONFIG_SAM=1
    CONFIG_STM=1
    CONFIG_TI=1
    CONFIG_TI_ICEPICK=1
)
