cmake_minimum_required(VERSION 3.20)
project(misko3_m9_touch C ASM)

set(CMAKE_C_STANDARD 11)
set(TUSB "${CMAKE_SOURCE_DIR}/../tinyusb/src" CACHE PATH "TinyUSB src directory")

set(HALSRC ${CMAKE_SOURCE_DIR}/hal/Src)

add_executable(misko3
    src/main.c
    src/gud_device.c
    src/gud_usbd.c
    src/panel.c
    src/lz4_dec.c
    src/buttons.c
    src/joystick.c
    src/touch.c
    src/hal_glue.c

    # ST HAL: only the modules the joystick and the touch panel need.
    ${HALSRC}/stm32g4xx_hal.c
    ${HALSRC}/stm32g4xx_hal_adc.c
    ${HALSRC}/stm32g4xx_hal_adc_ex.c
    ${HALSRC}/stm32g4xx_hal_rcc.c
    ${HALSRC}/stm32g4xx_hal_gpio.c
    ${HALSRC}/stm32g4xx_hal_spi.c
    ${HALSRC}/stm32g4xx_hal_cortex.c
    ${HALSRC}/stm32g4xx_hal_dma.c
    ${HALSRC}/stm32g4xx_hal_dma_ex.c
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

target_include_directories(misko3 PRIVATE
    src
    ${TUSB}
    cmsis/Include
    cmsis/Device/ST/STM32G4xx/Include
    hal/Inc
)

target_compile_definitions(misko3 PRIVATE STM32G474xx)

set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)

target_compile_options(misko3 PRIVATE
    ${CPU_FLAGS}
    -O2
    -Wall -Wextra
    -ffunction-sections -fdata-sections
)

target_link_options(misko3 PRIVATE
    ${CPU_FLAGS}
    -T${CMAKE_SOURCE_DIR}/STM32G474QETX_FLASH.ld
    --specs=nano.specs
    --specs=nosys.specs
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_BINARY_DIR}/misko3.map
    -Wl,--print-memory-usage
)

add_custom_command(TARGET misko3 POST_BUILD
    COMMAND arm-none-eabi-size $<TARGET_FILE:misko3>
)
