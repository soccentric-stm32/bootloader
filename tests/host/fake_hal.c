/*
 * fake_hal.c - backup registers and tick source for the host tests
 */

#include <string.h>
#include "stm32f7xx_hal.h"

static RTC_TypeDef rtc;
RTC_TypeDef *RTC = &rtc;

void fake_hal_reset(void)
{
    memset(&rtc, 0, sizeof(rtc));
}

uint32_t HAL_GetTick(void)
{
    return 0U;
}
