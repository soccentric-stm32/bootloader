/**
 * @file    fake_hal.c
 * @brief   Backup registers and tick source for the host tests.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  Host tests
 */

#include <string.h>
#include "stm32f7xx_hal.h"

/** Fake RTC register block backing the selection tests. */
static RTC_TypeDef rtc;
/** Pointer the production code sees as the RTC peripheral. */
RTC_TypeDef *RTC = &rtc;

/** @brief Clear every backup register between test cases. */
void fake_hal_reset(void)
{
    memset(&rtc, 0, sizeof(rtc));
}

/** @brief Return a constant tick; the host tests do not use timeouts. */
uint32_t HAL_GetTick(void)
{
    return 0U;
}
