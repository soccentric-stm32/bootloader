/*
 * boot - A/B bootloader entry point
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI (STM32F767ZIT6), dual-bank flash
 * SRS:     BSAFE-005, BGEN-008, ENT-003, IMG-003, SEL-003, HND-001/002
 *
 * Skeleton of the boot flow of SRS Appendix A. Implemented so far: flash
 * mode check, B1 "stay in BOOT", slot A vector check and hand-over. Still
 * to come, section by section: clock tree (BGEN-003), USART3 + HWLINK
 * (section 9), image header and CRC32 (section 5), metadata and A/B
 * selection with SWP_FB (section 6), trial boot and IWDG (section 7).
 */

#include <stdbool.h>
#include <stdint.h>
#include "stm32f7xx_hal.h"
#include "boot_board.h"
#include "boot_layout.h"
#include "handover.h"

static void boot_leds_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Pin   = BOOT_LED1_PIN | BOOT_LED2_PIN | BOOT_LED3_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BOOT_LED1_PORT, &gpio);

    /* B1 has an external pull-down on the board (Hardware.md section 9). */
    gpio.Pin  = BOOT_BTN1_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(BOOT_BTN1_PORT, &gpio);
}

/* BSAFE-005: BOOT only works with the flash in dual-bank mode. */
static bool boot_flash_is_dual_bank(void)
{
    return (FLASH->OPTCR & FLASH_OPTCR_nDBANK) == 0U;
}

/* ENT-003: B1 held at reset keeps the board in BOOT. */
static bool boot_button_held(void)
{
    return HAL_GPIO_ReadPin(BOOT_BTN1_PORT, BOOT_BTN1_PIN) == GPIO_PIN_SET;
}

int main(void)
{
    /* HAL tick on SysTick at the reset clock (HSI 16 MHz). */
    (void)HAL_Init();
    boot_leds_init();
    HAL_GPIO_WritePin(BOOT_LED2_PORT, BOOT_LED2_PIN, GPIO_PIN_SET);

    if (!boot_flash_is_dual_bank()) {
        /* WRONG_FLASH_MODE: never touch flash, blink LD3. */
        HAL_GPIO_WritePin(BOOT_LED2_PORT, BOOT_LED2_PIN, GPIO_PIN_RESET);
        while (1) {
            HAL_GPIO_TogglePin(BOOT_LED3_PORT, BOOT_LED3_PIN);
            HAL_Delay(250U);
        }
    }

    bool stay = boot_button_held();

    if (!stay && boot_app_vectors_valid(BOOT_SLOT_A_BASE)) {
        boot_handover(BOOT_APP_LOAD_ADDR); /* does not return */
    }

    if (!stay) {
        /* SEL-003: no bootable image. */
        HAL_GPIO_WritePin(BOOT_LED3_PORT, BOOT_LED3_PIN, GPIO_PIN_SET);
    }

    /* Update mode (section 9) attaches here. */
    while (1) {
        __WFI();
    }
}
