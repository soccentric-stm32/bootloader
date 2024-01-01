/**
 * @file    stm32f7xx_hal_conf.h
 * @brief   Project-owned STM32F7xx HAL configuration file.
 *
 * @details
 * This file is the *project's* override of the standard CubeMX / ST HAL
 * configuration template: it pins the external and internal oscillator
 * frequencies the HAL clock-tree math uses, selects which HAL modules the
 * bootloader actually links, and defines the runtime assert hook.
 *
 * Every other `stm32f7xx_hal_conf.h` that ships with a vendor package is
 * deliberately left untouched; the build system points the compiler at
 * this directory first, so ST library headers always resolve to this copy.
 *
 * Oscillator topology for the NUCLEO-F767ZI (see Hardware.md section 6):
 * PH0 carries the ST-LINK 8 MHz MCO output in *bypass* mode, which is what
 * `HSE_VALUE` refers to.  The internal 16 MHz HSI and 32 kHz LSI are the
 * fallbacks the clock driver (`src/bsp/clock.c`) can fall back to.
 *
 * The set of enabled modules is intentionally minimal - the bootloader
 * links only the drivers it needs, and the list grows as each SRS section
 * lands.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6, NUCLEO-F767ZI
 * @srs     BCON-002 (drivers used as shipped; this is the project-owned
 *          configuration file)
 */

#ifndef STM32F7xx_HAL_CONF_H
#define STM32F7xx_HAL_CONF_H

/**
 * @name Crystal / oscillator parameters.
 * @brief Frequencies (in Hz) and startup timeouts the HAL assumes for the
 *        external and internal oscillators.
 *
 * @details PH0 carries the ST-LINK 8 MHz MCO in bypass mode
 * (Hardware.md section 6); the LSI/LSE values are the standard F7
 * silicon defaults.
 * @{
 */

/** High Speed External crystal frequency in Hz (8 MHz ST-LINK bypass). */
#define HSE_VALUE    8000000U

/** Maximum time (ms) the HAL waits for the HSE to stabilise. */
#define HSE_STARTUP_TIMEOUT 100U

/** High Speed Internal oscillator frequency in Hz (16 MHz F7 default). */
#define HSI_VALUE   16000000U

/** Low Speed Internal oscillator frequency in Hz (32 kHz RC). */
#define LSI_VALUE    32000U

/** Low Speed External crystal frequency in Hz (32.768 kHz watch crystal). */
#define LSE_VALUE    32768U

/** Maximum time (ms) the HAL waits for the LSE to stabilise. */
#define LSE_STARTUP_TIMEOUT 5000U

/** SAI1 external audio clock frequency in Hz (unused here; HAL default). */
#define EXTERNAL_SAI1_CLOCK_VALUE 48000U

/** SAI2 external audio clock frequency in Hz (unused here; HAL default). */
#define EXTERNAL_SAI2_CLOCK_VALUE 48000U

/** General external clock frequency in Hz (12.288 MHz default). */
#define EXTERNAL_CLOCK_VALUE 12288000U

/** @} */

/**
 * @name Runtime / power tuning parameters.
 * @brief HAL global tuning values used by the clock and power drivers.
 * @{
 */

/**
 * Supply voltage (mV) the HAL uses to derive core voltage scaling.
 *
 * 3 V class: the F767 runs at 1.2 V core derived from this external
 * supply, i.e. Voltage Scale 2 behaviour assumed for PLL math.
 */
#define VDD_VALUE   3300U

/** SysTick (tick) interrupt priority; 15 = lowest priority. */
#define TICK_INT_PRIORITY 15U

/**
 * Real-Time-OS mode flag.
 *
 * 0 - FreeRTOS (or any RTOS) integration *not* enabled. The HAL
 *     therefore assumes `HAL_GetTick()` is backed by bare-metal
 *     SysTick, not an RTOS port.
 */
#define USE_RTOS 0U

/**
 * 1 - Enable instruction/prefetch behaviour used by the HAL when the
 *     CPU is running at high core clock (needed to hide fetch
 *     latency).
 */
#define PREFETCH_ENABLE 1U

/** @} */

/**
 * @name Enabled HAL modules.
 * @brief Selects which ST driver sources are compiled into the image.
 *
 * @details The list grows deliberately slowly; each SRS section that
 * introduces a new peripheral adds a line here.  Modules not listed
 * are left out of the linker, keeping the boot image small.
 * @{
 */

/** Core (misc) HAL: `HAL_Init`, tick, reset helpers. */
#define HAL_MODULE_ENABLED

/** Cortex-M NVIC and core-level operations. */
#define HAL_CORTEX_MODULE_ENABLED

/** Clock-control (RCC) driver - required for @ref clock_bsp. */
#define HAL_RCC_MODULE_ENABLED

/** GPIO driver - LEDs and buttons, see @ref boot_board.h. */
#define HAL_GPIO_MODULE_ENABLED

/** Power-management driver - low-power state entry helper. */
#define HAL_PWR_MODULE_ENABLED

/**
 * Flash driver.
 *
 * The flash latency macros live inside this submodule; enabling it is
 * mandatory for the RCC driver to compute a valid core/prefetch
 * configuration (UPD-005).
 */
#define HAL_FLASH_MODULE_ENABLED    /* RCC needs the latency macros; UPD-005 */

/** DMA engine driver - used by the USART3 receive ring (BGEN-010). */
#define HAL_DMA_MODULE_ENABLED      /* USART3 receive ring; BGEN-010       */

/** USART driver - the update protocol transport, USART3 (BGEN-004). */
#define HAL_UART_MODULE_ENABLED     /* USART3; BGEN-004                    */

/** @} */

#include "stm32f7xx_hal_rcc.h"
#include "stm32f7xx_hal_gpio.h"
#include "stm32f7xx_hal_cortex.h"
#include "stm32f7xx_hal_pwr.h"
#include "stm32f7xx_hal_pwr_ex.h"
#include "stm32f7xx_hal_flash.h"
#include "stm32f7xx_hal_flash_ex.h"
#include "stm32f7xx_hal_dma.h"
#include "stm32f7xx_hal_uart.h"

#ifdef USE_FULL_ASSERT

/**
 * @brief   HAL runtime assert failure hook.
 *
 * @details Called by @ref assert_param when a parameter check fails and
 *          `USE_FULL_ASSERT` is 1.  Must not return.
 *
 * @param[in] file   8-bit pointer string with the source file name of
 *                   the failed check (e.g. `__FILE__`).
 * @param[in] line   Line number in the source file where the check
 *                   failed (e.g. `__LINE__`).
 */
void assert_failed(uint8_t *file, uint32_t line);

#endif

/**
 * @def     assert_param(expr)
 * @brief   Runtime parameter-check macro used across the HAL drivers.
 *
 * @details When `USE_FULL_ASSERT` is defined the expression `expr` is
 *          evaluated; on failure the user hook `assert_failed()` is
 *          called with the offending file and line.  Otherwise the
 *          macro compiles down to `(void)0U` and the check is removed
 *          from the shipping boot image (the bootloader prefers
 *          determinism over runtime validation).
 *
 * @param[in] expr  Expression expected to be true (non-zero); if
 *                  false, `assert_failed` fires (only with
 *                  `USE_FULL_ASSERT` enabled).
 */
#ifdef  USE_FULL_ASSERT
  #define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
#else
  #define assert_param(expr) ((void)0U)
#endif

#endif /* STM32F7xx_HAL_CONF_H */
