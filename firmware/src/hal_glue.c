/*
 * The minimum needed to make ST's HAL run inside this project.
 *
 * HAL assumes two things that are not true here:
 *
 *   1. That it may own the SysTick interrupt for timekeeping. This project
 *      deliberately runs with no interrupt other than USB, because a blit
 *      retimes the FMC bus around every GET_SCANLINE read and an interrupt
 *      landing inside that window would run at the wrong timings. So
 *      HAL_InitTick() is overridden to do nothing.
 *
 *   2. That HAL_GetTick() returns milliseconds. Those are derived from the
 *      DWT cycle counter, which is already running, rather than starting a
 *      second timer for them.
 *
 * Both functions are declared __weak in stm32g4xx_hal.c, so defining them
 * here replaces the defaults at link time.
 */

#include "stm32g4xx_hal.h"

#define SYSCLK_HZ      170000000u
#define CYCLES_PER_MS  (SYSCLK_HZ / 1000u)

/*
 * HAL's ADC driver uses SystemCoreClock to size its internal delay loops.
 * This project sets the clock by hand in clock_init() and does not compile
 * ST's system_stm32g4xx.c, so the variable is defined here instead.
 */
uint32_t SystemCoreClock = SYSCLK_HZ;

/*
 * HAL expects a counter that wraps at 2^32 ms.
 *
 * DWT->CYCCNT wraps every 25.3 s at 170 MHz, so CYCCNT / 170000 is not enough:
 * that quotient wraps at 25 264, and an unsigned subtraction across the wrap
 * would give a wrong elapsed time. Milliseconds are accumulated instead, with
 * the anchor advanced by a whole number of milliseconds each time.
 *
 * This is the same pattern as scan_t0 in panel.c and ms_ticks in main.c: add
 * the period to the anchor, never assign "now", or the remainder is lost.
 */
static uint32_t hal_ms;
static uint32_t hal_anchor;

uint32_t HAL_GetTick(void)
{
    uint32_t now = DWT->CYCCNT;
    uint32_t diff = now - hal_anchor;          /* correct across the DWT wrap */
    uint32_t whole = diff / CYCLES_PER_MS;

    if (whole) {
        hal_ms += whole;
        hal_anchor += whole * CYCLES_PER_MS;
    }
    return hal_ms;
}

/*
 * No SysTick. HAL_Init() calls this itself; returning HAL_OK lets
 * initialisation continue without the interrupt ever being enabled.
 */
HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    uwTickPrio = TickPriority;
    return HAL_OK;
}

/*
 * Called from HAL_ADC_Init() / HAL_SPI_Init() and their DeInit counterparts.
 * Clocks and pins are set up in joystick_init() and touch_init(), so these are
 * empty -- defined explicitly so it is clear they were not forgotten.
 */
void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc) { (void) hadc; }
void HAL_ADC_MspDeInit(ADC_HandleTypeDef *hadc) { (void) hadc; }
void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi) { (void) hspi; }
void HAL_SPI_MspDeInit(SPI_HandleTypeDef *hspi) { (void) hspi; }
