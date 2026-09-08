# ST STM32G4 HAL

These files are STMicroelectronics' HAL driver for the STM32G4 family, not my
work. They were taken from the MiSKo3 board's own factory firmware, at
`Firmware/Test/Drivers/STM32G4xx_HAL_Driver/` in
[mjankovec/MiSKo3](https://github.com/mjankovec/MiSKo3), and are unmodified.

Only six modules are enabled in `Inc/stm32g4xx_hal_conf.h`: HAL, ADC, GPIO,
DMA, RCC, CORTEX and SPI. Nine `.c` files are compiled. `Inc/` is complete
because the HAL headers include one another.

## Why the main branch does not use this

`main` sets every register directly and does not link the HAL at all. This
branch uses it for two peripherals where it earns its place:

- **ADC4**, for the analogue stick. RM0440 requires `ADEN` to be set only four
  ADC clock cycles after hardware clears `ADCAL`. The hand-written version in
  `main` missed that and hung waiting for `ADRDY`; the fix there is a manual
  `delay_us(2)`. `HAL_ADCEx_Calibration_Start()` inserts the delay itself, so
  the bug cannot happen.
- **SPI1**, for the XPT2046 touch controller.

The cost is about 4.8 kB of flash and 160 B of RAM, plus `src/hal_glue.c`,
which stops HAL from taking the SysTick interrupt this project deliberately
does not use.

## Licence

Each file carries an ST copyright header referring to a LICENSE file. The
`LICENSE.txt` shipped in the board's repository points in turn to a package
licence file that is not present alongside it. ST publishes the authoritative
terms with the driver in
[STMicroelectronics/STM32CubeG4](https://github.com/STMicroelectronics/STM32CubeG4),
under `Drivers/STM32G4xx_HAL_Driver/`. Refer to that for the terms that apply.
