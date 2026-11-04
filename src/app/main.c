/*
 * boot - A/B bootloader entry point
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI (STM32F767ZIT6), dual-bank flash
 * SRS:     SRS-BOOT-F767 Appendix A: BSAFE-005, BGEN-008, ENT-001..005,
 *          SEL-001..004, TRY-001..008, HND-001..005
 *
 * Reset flow: flash mode check, metadata scan, trial-limit handling, A/B
 * selection, stay-request/B1 checks, 500 ms Hello window, update mode, then
 * trial bookkeeping, IWDG start and hand-over.
 */

#include <stdbool.h>
#include <stdint.h>
#include "stm32f7xx_hal.h"
#include "boot_board.h"
#include "boot_layout.h"
#include "clock.h"
#include "flash.h"
#include "handover.h"
#include "iwdg.h"
#include "proto.h"
#include "selection.h"
#include "uart3.h"
#include "update.h"

static boot_selection_t boot_selection;
static boot_update_t boot_update;

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

/* ENT-003: B1 held at reset keeps the board in BOOT.  Sample it for about
 * 10 ms instead of once, so a bounce or a noise spike cannot trigger it. */
static bool boot_button_held(void)
{
    for (uint32_t i = 0U; i < 10U; i++) {
        if (HAL_GPIO_ReadPin(BOOT_BTN1_PORT, BOOT_BTN1_PIN) != GPIO_PIN_SET) {
            return false;
        }
        HAL_Delay(1U);
    }
    return true;
}

static void boot_error_loop(void)
{
    HAL_GPIO_WritePin(BOOT_LED2_PORT, BOOT_LED2_PIN, GPIO_PIN_RESET);
    while (1) {
        HAL_GPIO_TogglePin(BOOT_LED3_PORT, BOOT_LED3_PIN);
        HAL_Delay(250U);
    }
}

/* BSAFE-005: no valid flash mode, no flash writes.  Still answer HWLINK so
 * a host can read the WRONG_FLASH_MODE detail, and blink LD3. */
static void boot_wrong_flash_mode(void)
{
    boot_proto_init(&boot_selection, &boot_update);

    uint32_t last = HAL_GetTick();
    bool on = false;
    for (;;) {
        boot_proto_poll();
        uint32_t now = HAL_GetTick();
        if ((now - last) >= 250U) {
            on = !on;
            HAL_GPIO_WritePin(BOOT_LED3_PORT, BOOT_LED3_PIN,
                              on ? GPIO_PIN_SET : GPIO_PIN_RESET);
            last = now;
        }
        HAL_Delay(1U);
    }
}

static void boot_rescan_and_choose(void)
{
    boot_selection_scan(&boot_selection);
    boot_selection_choose(&boot_selection);
    boot_flash_set_active(boot_selection.active, boot_selection.active_valid);
}

int main(void)
{
    (void)HAL_Init();
    boot_ramfunc_init();
    boot_leds_init();
    HAL_GPIO_WritePin(BOOT_LED2_PORT, BOOT_LED2_PIN, GPIO_PIN_SET);

    boot_selection_init();

    if (!boot_clock_init()) {
        boot_error_loop();
    }

    /* No flash write is possible until the mode check has passed. */
    boot_flash_set_allowed(false);
    boot_update_reset(&boot_update);

    if (!boot_uart3_init()) {
        boot_error_loop();
    }

    boot_rescan_and_choose();

    if (!boot_flash_is_dual_bank()) {
        boot_wrong_flash_mode();   /* does not return */
    }
    boot_flash_set_allowed(true);

    /* TRY-003: a pending slot that has used up its trials is rejected and
     * the selection is made again. */
    bool trial_limit_pending = boot_selection.active_valid &&
                               boot_selection.active_pending &&
                               boot_selection.trial_count >= BOOT_TRIAL_LIMIT;
    if (boot_selection_apply_trial_limit(&boot_selection)) {
        boot_rescan_and_choose();
    } else if (trial_limit_pending) {
        /* The REJECTED record could not be programmed.  Never boot a slot
         * that has exhausted its trials: reject it in RAM for this boot and
         * select the other slot, or stay in update mode if none is valid.
         * The mask is re-applied on later scans (P0-2). */
        boot_selection_mark_rejected(&boot_selection, boot_selection.active,
                                     BOOT_REJECT_TRIAL_LIMIT);
        boot_rescan_and_choose();
    }

    /* TRY-006: a confirmed slot clears the trial counter. */
    if (boot_selection.active_valid && !boot_selection.active_pending) {
        boot_selection_clear_trial();
    }

    /* SEL-003 / BGEN-008: red LED while no bootable image exists. */
    HAL_GPIO_WritePin(BOOT_LED3_PORT, BOOT_LED3_PIN,
                      boot_selection.active_valid ? GPIO_PIN_RESET
                                                  : GPIO_PIN_SET);

    bool force_update = !boot_selection.active_valid;
    /* ENT-002: always read (and clear) the request, even when an earlier
     * condition already forced update mode, or the magic would survive
     * into the next boot after Activate. */
    if (boot_selection_take_stay_request()) {
        force_update = true;
    }
    force_update = force_update || boot_button_held();

    boot_proto_init(&boot_selection, &boot_update);

    if (!force_update) {
        /* ENT-001: 500 ms window for a Hello. */
        if (boot_proto_listen_for_hello(BOOT_LISTEN_WINDOW_MS)) {
            force_update = true;
        }
    }

    if (force_update) {
        /* ENT-004: with no valid slot this never returns; Activate resets
         * the board.  Otherwise it returns after 120 s without traffic. */
        boot_proto_run(boot_selection.active_valid);

        boot_rescan_and_choose();
        if (!boot_selection.active_valid) {
            HAL_GPIO_WritePin(BOOT_LED3_PORT, BOOT_LED3_PIN, GPIO_PIN_SET);
            boot_proto_run(false);   /* never returns */
        }
    }

    /* TRY-002: count this attempt before the application starts. */
    boot_selection_start_trial(&boot_selection);

    /* HND-003: slot and trial state for the application. */
    boot_selection_write_handover(&boot_selection);

    /* TRY-004 / HND-004: a hung application resets and fails its trial. */
    boot_iwdg_start();

    /* HND-001/002: reset state, optional SWP_FB, jump. */
    boot_handover(boot_selection.active);
}
