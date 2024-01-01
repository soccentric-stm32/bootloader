/**
 * @file    clock.h
 * @brief   216 MHz clock tree from the ST-LINK MCO, and its teardown.
 * @details PCLK1 = 54 MHz keeps USART3 within 0.01 % at 921600 baud, the
 *          tolerance the update link relies on.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6 rev Z, 3.3 V (voltage scale 1)
 * @srs     BGEN-003, BGEN-004, HND-001
 */

#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>

/**
 * @name Target clock tree.
 * @brief
 * - HSE bypass:  8 MHz (ST-LINK MCO)
 * - PLL output:  216 MHz  (SYSCLK / HCLK)
 * - APB1/PCLK1:  54 MHz   (/4)
 * - APB2/PCLK2:  108 MHz  (/2)
 * - Flash:       7 wait states, over-drive enabled
 * @{
 */

/**
 * @brief   Configure HSE bypass, PLL, over-drive and bus clocks for 216 MHz.
 * @details PLL M=8, N=432, P=2, Q=9; AHB/APB1/APB2 = 1/4/2; 7 wait states,
 *          prefetch and ART on.  HAL_RCC_ClockConfig() also starts SysTick,
 *          whose handler lives in clock.c.
 *
 * @return  true when the tree is running; false on HSE, PLL, over-drive or
 *          switch failure (clock state then unspecified).
 *
 * @note    Call once per reset, before UART3, IWDG timing or HAL_GetTick().
 *
 * @srs     BGEN-003, BGEN-004
 */
bool boot_clock_init(void);

/**
 * @brief   Return the clock tree to its reset state before hand-over.
 * @details SYSCLK = HSI 16 MHz, PLLs and HSE off, prescalers /1, 0 wait
 *          states, prefetch/ART off and flushed, over-drive off.  RCC_CSR is
 *          left alone so the application can read the reset flags
 *          (BGEN-007).
 *
 * @note    HAL timeouts are unusable afterwards; hand-over also stops
 *          SysTick.
 *
 * @srs     HND-001, BGEN-007
 */
void boot_clock_deinit(void);

/** @} */

#endif /* CLOCK_H */
