/*
 * fake_hal/stm32f7xx_hal.h - host-test replacement for the STM32 HAL
 *
 * Only the symbols used by src/app/selection.c and the metadata/update
 * modules are provided.  The fake flash and RTC backup registers live in
 * fake_flash.c and fake_hal.c.
 */

#ifndef STM32F7XX_HAL_H
#define STM32F7XX_HAL_H

#include <stdint.h>

typedef struct {
    volatile uint32_t BKP0R;
    volatile uint32_t BKP1R;
    volatile uint32_t BKP2R;
    volatile uint32_t BKP3R;
    volatile uint32_t BKP4R;
    volatile uint32_t BKP5R;
    volatile uint32_t BKP6R;
    volatile uint32_t BKP7R;
    volatile uint32_t BKP8R;
    volatile uint32_t BKP9R;
    volatile uint32_t BKP10R;
    volatile uint32_t BKP11R;
    volatile uint32_t BKP12R;
    volatile uint32_t BKP13R;
    volatile uint32_t BKP14R;
    volatile uint32_t BKP15R;
    volatile uint32_t BKP16R;
    volatile uint32_t BKP17R;
    volatile uint32_t BKP18R;
    volatile uint32_t BKP19R;
} RTC_TypeDef;

extern RTC_TypeDef *RTC;

uint32_t HAL_GetTick(void);

#define __HAL_RCC_PWR_CLK_ENABLE() do { } while (0)
#define __HAL_RCC_RTC_CLK_ENABLE() do { } while (0)
#define HAL_PWR_EnableBkUpAccess() do { } while (0)
#define __DSB() do { } while (0)

#endif /* STM32F7XX_HAL_H */
