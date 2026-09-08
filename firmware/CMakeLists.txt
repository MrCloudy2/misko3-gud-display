cmake_minimum_required(VERSION 3.20)
project(misko3_m7_opt C ASM)

set(CMAKE_C_STANDARD 11)
set(TUSB ${CMAKE_SOURCE_DIR}/../tinyusb/src)

add_executable(m7_opt
    src/main.c
    src/gud_device.c
    src/gud_usbd.c
    src/panel.c
    src/lz4_dec.c
    src/buttons.c
    src/joystick.c
    src/usb_descriptors.c
    src/rtt.c
    src/startup_stm32g474qetx.s

    # TinyUSB core plus the HID class driver. The GUD interface deliberately
    # has no built-in driver listed: it is served by our own class driver in
    # gud_usbd.c, registered through usbd_app_driver_get_cb().
    ${TUSB}/tusb.c
    ${TUSB}/common/tusb_fifo.c
    ${TUSB}/device/usbd.c
    ${TUSB}/class/hid/hid_device.c

    # STM32 full-speed device controller port
    ${TUSB}/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
    ${TUSB}/portable/st/stm32_fsdev/fsdev_common.c
)

target_include_directories(m7_opt PRIVATE
    src
    ${TUSB}
    cmsis/Include
    cmsis/Device/ST/STM32G4xx/Include
)

target_compile_definitions(m7_opt PRIVATE STM32G474xx)

set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)

target_compile_options(m7_opt PRIVATE
    ${CPU_FLAGS}
    -O2
    -Wall -Wextra
    -ffunction-sections -fdata-sections
)

target_link_options(m7_opt PRIVATE
    ${CPU_FLAGS}
    -T${CMAKE_SOURCE_DIR}/STM32G474QETX_FLASH.ld
    --specs=nano.specs
    --specs=nosys.specs
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_BINARY_DIR}/m7_opt.map
    -Wl,--print-memory-usage
)

add_custom_command(TARGET m7_opt POST_BUILD
    COMMAND arm-none-eabi-size $<TARGET_FILE:m7_opt>

    # Intel HEX and raw binary alongside the ELF.
    #
    # probe-rs is happy with the ELF, but ST's tools (STM32CubeIDE and
    # STM32CubeProgrammer) want a file whose extension they recognise. HEX is
    # the one to prefer: it carries its own load addresses, so nothing has to
    # be typed in. A raw BIN has no addresses at all and must be flashed to
    # 0x08000000 by hand.
    COMMAND arm-none-eabi-objcopy -O ihex   $<TARGET_FILE:m7_opt> $<TARGET_FILE:m7_opt>.hex
    COMMAND arm-none-eabi-objcopy -O binary $<TARGET_FILE:m7_opt> $<TARGET_FILE:m7_opt>.bin
)
