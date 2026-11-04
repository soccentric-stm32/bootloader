/*
 * clock.h - 216 MHz clock tree from the ST-LINK MCO
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6 rev Z, 3.3 V (voltage scale 1)
 * SRS:     BGEN-003, BGEN-004, HND-001
 */

#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>

/* HSE bypass 8 MHz -> PLL -> SYSCLK 216 MHz, HCLK 216 MHz, PCLK1 54 MHz,
 * PCLK2 108 MHz, flash latency 7, over-drive on.  This keeps USART3
 * (PCLK1) within 0.01 % at 921600 baud. */
bool boot_clock_init(void);

/* HND-001 without touching RCC_CSR: SYSCLK back to HSI, PLL/HSE off, APB/AHB
 * prescalers /1 and flash wait states 0.  BGEN-007 leaves the reset flags for
 * the application. */
void boot_clock_deinit(void);

#endif /* CLOCK_H */
