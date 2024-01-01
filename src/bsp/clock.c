/**
 * @file    clock.c
 * @brief   216 MHz clock tree from the ST-LINK MCO.
 *
 * Clock plan (MHz):
 *
 * | stage        | value  | notes                                |
 * |--------------|--------|--------------------------------------|
 * | HSE bypass   |   8    | ST-LINK MCO on PH0                   |
 * | PLL input    |   1    | PLLM = 8                             |
 * | VCO          | 432    | PLLN = 432                           |
 * | SYSCLK/HCLK  | 216    | PLLP = 2                             |
 * | PCLK1 (APB1) |  54    | /4 - keeps USART3 within 0.01 % at
 * |              |        | 921600 baud                          |
 * | PCLK2 (APB2) | 108    | /2                                   |
 * | flash        | 7 WS   | over-drive enabled                   |
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6 rev Z @ 3.3 V (voltage scale 1)
 * @srs     BGEN-003 (clock tree documented), BGEN-004, HND-001
 */

#include "stm32f7xx_hal.h"
#include "clock.h"

/** @brief Bring up the 216 MHz tree; see @ref boot_clock_init. */
bool boot_clock_init(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_BYPASS;   /* ST-LINK MCO feeds PH0 */
    osc.HSIState       = RCC_HSI_OFF;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = 8U;      /* 8 MHz / 8 = 1 MHz into the VCO   */
    osc.PLL.PLLN       = 432U;    /* 1 MHz * 432 = 432 MHz VCO        */
    osc.PLL.PLLP       = RCC_PLLP_DIV2;   /* 432 / 2 = 216 MHz SYSCLK */
    osc.PLL.PLLQ       = 9U;      /* 432 / 9 = 48 MHz (USB/SDMMC/RNG) */

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        return false;
    }

    if (HAL_PWREx_EnableOverDrive() != HAL_OK) {
        return false;
    }

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                         RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_7) != HAL_OK) {
        return false;
    }

    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
    __HAL_FLASH_ART_ENABLE();
    return true;
}

/**
 * @brief   Restore the reset clock state; see @ref boot_clock_deinit.
 * @details Each step polls its ready flag before the next one, as the
 *          hardware requires.  Waits are unbounded.
 */
void boot_clock_deinit(void)
{
    /* HSI on and SYSCLK back to HSI. */
    SET_BIT(RCC->CR, RCC_CR_HSION);
    while ((RCC->CR & RCC_CR_HSIRDY) == 0U) {
    }
    CLEAR_BIT(RCC->CFGR, RCC_CFGR_SW);
    while ((RCC->CFGR & RCC_CFGR_SWS) != 0U) {
    }

    /* 16 MHz needs 0 flash wait states.  HND-001: leave prefetch and the
     * ART accelerator off and flush the ART cache. */
    CLEAR_BIT(FLASH->ACR, FLASH_ACR_LATENCY);
    CLEAR_BIT(FLASH->ACR, FLASH_ACR_ARTEN | FLASH_ACR_PRFTEN);
    SET_BIT(FLASH->ACR, FLASH_ACR_ARTRST);
    __DSB();
    CLEAR_BIT(FLASH->ACR, FLASH_ACR_ARTRST);
    __DSB();

    /* PLL, PLLI2S, PLLSAI and HSE off. */
    CLEAR_BIT(RCC->CR, RCC_CR_PLLON);
    while ((RCC->CR & RCC_CR_PLLRDY) != 0U) {
    }
    CLEAR_BIT(RCC->CR, RCC_CR_PLLI2SON);
    while ((RCC->CR & RCC_CR_PLLI2SRDY) != 0U) {
    }
    CLEAR_BIT(RCC->CR, RCC_CR_PLLSAION);
    while ((RCC->CR & RCC_CR_PLLSAIRDY) != 0U) {
    }
    CLEAR_BIT(RCC->CR, RCC_CR_HSEON | RCC_CR_HSEBYP | RCC_CR_CSSON);
    while ((RCC->CR & RCC_CR_HSERDY) != 0U) {
    }

    /* HND-001: leave over-drive disabled (reset state). */
    CLEAR_BIT(PWR->CR1, PWR_CR1_ODSWEN);
    while ((PWR->CSR1 & PWR_CSR1_ODSWRDY) != 0U) {
    }
    CLEAR_BIT(PWR->CR1, PWR_CR1_ODEN);
    while ((PWR->CSR1 & PWR_CSR1_ODRDY) != 0U) {
    }

    /* PLL configuration registers to reset values, while the PLLs are off. */
    RCC->PLLCFGR = RCC_PLLCFGR_PLLM_4 | RCC_PLLCFGR_PLLN_6 |
                   RCC_PLLCFGR_PLLN_7 | RCC_PLLCFGR_PLLQ_2 | 0x20000000U;
    RCC->PLLI2SCFGR = RCC_PLLI2SCFGR_PLLI2SN_6 |
                      RCC_PLLI2SCFGR_PLLI2SN_7 |
                      RCC_PLLI2SCFGR_PLLI2SQ_2 |
                      RCC_PLLI2SCFGR_PLLI2SR_1;
    RCC->PLLSAICFGR = RCC_PLLSAICFGR_PLLSAIN_6 |
                      RCC_PLLSAICFGR_PLLSAIN_7 |
                      RCC_PLLSAICFGR_PLLSAIQ_2 | 0x20000000U;

    /* AHB /1, APB1 /1, APB2 /1, HSI as source.  RCC_CSR is deliberately not
     * touched: the application owns the reset flags (BGEN-007). */
    CLEAR_REG(RCC->CFGR);
    SystemCoreClock = HSI_VALUE;
}

/**
 * @brief   SysTick ISR: advance the HAL tick.
 * @details HAL_RCC_ClockConfig() enables the SysTick interrupt and every
 *          BOOT timeout reads HAL_GetTick(); without this handler the first
 *          tick would land in Default_Handler.  Runs in ISR context.
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}
