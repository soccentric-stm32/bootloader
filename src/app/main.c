/**
 * @file    main.c
 * @brief   A/B bootloader entry point and reset flow.
 *
 * @details
 * This is the top-level program of the bootloader image.  It wires
 * together the individual BSP / selection / protocol modules into the
 * full boot sequence:
 *
 * 1. `HAL_Init()`, `.ramfunc` relocation, LED + button GPIO setup.
 * 2. Clock initialisation - if it fails the board enters the red
 *    error loop (a fatal, non-recoverable condition).
 * 3. Flash is *locked by default*: no flash writes are issued until the
 *    dual-bank mode check (BSAFE-005) has passed.
 * 4. First scan + choose; wrong flash mode traps in the diagnostic
 *    loop (still answering HWLINK so a host can read the reason).
 * 5. Trial-limit enforcement (TRY-003), including the RAM-only fallback
 *    if the REJECTED metadata record cannot be programmed.
 * 6. Stay-request (ENT-002) and B1 held at reset (ENT-003) evaluation,
 *    then the 500 ms Hello window (ENT-001).
 * 7. Update mode (ENT-004/005) when requested; otherwise, trial
 *    bookkeeping, IWDG start, and hand-over to the chosen slot.
 *
 * The flow is intentionally linear in `main()`: every step either
 * returns normally (to the next step) or never returns at all (error
 * loops and update mode), which makes the state transitions obvious from
 * a single reading of the source.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6), dual-bank flash
 * @srs     SRS-BOOT-F767 Appendix A: BSAFE-005, BGEN-008, ENT-001..005,
 *          SEL-001..004, TRY-001..008, HND-001..005
 */

#include <stdbool.h>
#include <stdint.h>
#include "stm32f7xx_hal.h"
#include "boot_board.h"
#include "boot_layout.h"
#include "clock.h"
#include "debug_uart.h"
#include "flash.h"
#include "handover.h"
#include "iwdg.h"
#include "proto.h"
#include "selection.h"
#include "uart3.h"
#include "update.h"

/**
 * @name Module-private working state.
 * @brief The two shared context objects threaded through the selection,
 *        update and protocol modules for the duration of this boot.
 * @{
 */

/** Selection context: scan results, chosen slot, trial and reject state. */
static boot_selection_t boot_selection;

/** Update context: transfer state for the (optional) in-boot update. */
static boot_update_t boot_update;

/** @} */

/**
 * @brief   Configure the user LEDs (output, no pull) and button B1
 *          (input) GPIOs.
 *
 * @details Enables the clocks on GPIOB (LEDs) and GPIOC (button), then
 *          configures:
 *
 * - the three user LEDs as push-pull outputs at low speed (the LEDs are
 *   slow loads; there is no need to drive them with the high-speed
 *   buffers),
 * - the user button B1 as a plain input.  The board already provides an
 *   external pull-down on this pin (Hardware.md section 9), so no
 *   internal pull is needed, and the input is sampled with its
 *   deglitching window in @ref boot_button_held.
 *
 * @srs     BGEN-008, ENT-003
 */
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

/**
 * @brief   Check that the flash is configured for dual-bank mode.
 *
 * @details BSAFE-005: the A/B slot scheme assumes two independent banks
 *          (each ~1 MB, bank A at 0x08000000, bank B at 0x08100000).
 *          If the option byte `nDBANK` is set, the parts of the two
 *          banks are interleaved as one 2 MB bank and the slot layout
 *          is invalid - the bootloader must refuse to write anything
 *          and trap in the diagnostic loop.
 *
 * @return  `true`  when the option byte indicates dual-bank mode.
 * @return  `false` when the parts are interleaved (single-bank mode);
 *          the caller must enter the wrong-flash-mode diagnostic loop.
 *
 * @srs     BSAFE-005
 */
static bool boot_flash_is_dual_bank(void)
{
    return (FLASH->OPTCR & FLASH_OPTCR_nDBANK) == 0U;
}

/**
 * @brief   Check that button B1 is physically held at reset.
 *
 * @details ENT-003: B1 held at reset keeps the board in bootloader for
 *          this boot (force-update intent).  The pin is sampled ~10
 *          times at 1 ms spacing (~10 ms total) instead of once, so a
 *          contact bounce or a single noise spike cannot erroneously
 *          force the update mode.
 *
 * @return  `true`  when the pin read HIGH on every sample across the
 *          whole window; `false` otherwise (including any sample that
 *          read LOW).
 *
 * @note    Uses `HAL_Delay(1)` between samples, so a working tick is
 *          required - called after the clock initialisation path.
 *
 * @srs     ENT-003
 */
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

/**
 * @brief   Fatal error loop: LED2 off, LED3 blinking at 4 Hz, spin.
 *
 * @details Used for conditions that cannot be recovered from inside the
 *          bootloader (clock initialisation failure, UART bring-up
 *          failure).  The board remains powered and JTAG/SWD remains
 *          available - the LED state is intentionally stable (constant
 *          red blink) so it is distinguishable from the wrong-flash-mode
 *          loop in @ref boot_wrong_flash_mode.
 *
 * @warning Never returns.
 *
 * @srs     BGEN-008
 */
static void boot_error_loop(void)
{
    HAL_GPIO_WritePin(BOOT_LED2_PORT, BOOT_LED2_PIN, GPIO_PIN_RESET);
    while (1) {
        HAL_GPIO_TogglePin(BOOT_LED3_PORT, BOOT_LED3_PIN);
        HAL_Delay(250U);
    }
}

/**
 * @brief   Wrong-flash-mode diagnostic loop (BSAFE-005).
 *
 * @details Behaviour:
 * - No flash writes are possible - the flash lock in @ref main stays
 *   off, the only observable side-effect is the LED and the
 *   protocol endpoint.
 * - The protocol stack is still initialised (in read-only mode, as the
 *   lock implies), so a host connected to the UART can read the
 *   `WRONG_FLASH_MODE` detail and act on it (typically: reprogram the
 *   option bytes in ST-LINK and re-reset).
 * - LED3 blinks at 4 Hz (250 ms period) as the visual marker of this
 *   state.
 *
 * @warning Never returns.
 *
 * @srs     BSAFE-005
 */
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

/**
 * @brief   Re-scan the both slots and re-choose the active one, then
 *          refresh the flash driver's active-slot hint.
 *
 * @details Small helper that wraps the "scan -> choose -> publish"
 *          sequence used in several places in @ref main (after trial
 *          limit handling, after a failed record program, and after the
 *          update-mode branch).  Keeping the three calls in one place
 *          guarantees the order: the flash driver must see the *new*
 *          active slot before any subsequent write that depends on it.
 */
static void boot_rescan_and_choose(void)
{
    boot_selection_scan(&boot_selection);
    boot_selection_choose(&boot_selection);
    boot_flash_set_active(boot_selection.active, boot_selection.active_valid);
}

#if BOOT_DEBUG_UART
/** BDBG-001: one debug line per slot, then the selection result. */
static void boot_dbg_selection(void)
{
    static const char *const header_text[] = {
        "ok", "bad format", "bad layout", "unsigned", "wrong key",
        "bad signature"
    };
    for (uint32_t i = 0U; i < 2U; i++) {
        const boot_slot_info_t *slot = &boot_selection.slot[i];
        boot_dbg_begin(slot->header_valid || !slot->header_present ? 'I'
                                                                   : 'W');
        boot_dbg_str(i == 0U ? "slot A: " : "slot B: ");
        if (!slot->header_present) {
            boot_dbg_str("empty");
        } else {
            uint32_t status = (uint32_t)slot->header_status;
            boot_dbg_str(status < 6U ? header_text[status] : "header error");
            boot_dbg_str(slot->rejected ? ", rejected" :
                         slot->confirmed ? ", confirmed" :
                         slot->activated ? ", pending" : "");
        }
        boot_dbg_end();
    }
    boot_dbg_begin(boot_selection.active_valid ? 'I' : 'W');
    if (boot_selection.active_valid) {
        boot_dbg_str("sel: slot ");
        boot_dbg_str(boot_selection.active == BOOT_SLOT_A ? "A" : "B");
        boot_dbg_str(boot_selection.active_pending ? " on trial, count "
                                                   : " confirmed, count ");
        boot_dbg_dec(boot_selection.trial_count);
    } else {
        boot_dbg_str("sel: no bootable slot");
    }
    boot_dbg_end();
}
#else
static inline void boot_dbg_selection(void) {}
#endif

/**
 * @brief   Bootloader entry point; runs the full reset flow.
 *
 * @details See the file-level description for the ordered steps.  The
 *          two module-private contexts (@ref boot_selection and
 *          @ref boot_update) are created here and threaded through to
 *          the selection / update / protocol modules; they are plain
 *          `static` storage (no heap, no global locks) and live for
 *          the whole boot.
 *
 * @return  Not reached in normal operation: either @ref boot_handover
 *          is invoked (which does not return) or one of the fatal paths
 *          (@ref boot_error_loop, @ref boot_wrong_flash_mode, or a
 *          terminal @ref boot_proto_run) spins forever.  The `int`
 *          return type is kept for the C runtime's expectation of
 *          `main`; any return here is a defect and will be visible
 *          because the IWDG (started just before hand-over) would
 *          otherwise reset the board immediately.
 *
 * @srs     Appendix A (see file header)
 */
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

    /* BDBG-001: debug text on USART6; empty when BOOT_DEBUG_UART=0. */
    boot_dbg_init();
    boot_dbg_begin('I');
    boot_dbg_str("start v" BOOT_VERSION_STR " " BOOT_GIT_COMMIT ", reset ");
    boot_dbg_hex(RCC->CSR);
    boot_dbg_end();

    /* No flash write is possible until the mode check has passed. */
    boot_flash_set_allowed(false);
    boot_update_reset(&boot_update);

    if (!boot_uart3_init()) {
        boot_error_loop();
    }

    boot_rescan_and_choose();

    if (!boot_flash_is_dual_bank()) {
        boot_dbg_begin('E');
        boot_dbg_str("single-bank flash: run tools/provision.sh");
        boot_dbg_end();
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
    if (trial_limit_pending) {
        boot_dbg_begin('W');
        boot_dbg_str("trial limit reached: slot rejected");
        boot_dbg_end();
    }
    boot_dbg_selection();

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
        boot_dbg_begin('I');
        boot_dbg_str("update mode (HWLINK on USART3)");
        boot_dbg_end();
        boot_proto_run(boot_selection.active_valid);

        boot_rescan_and_choose();
        boot_dbg_selection();
        if (!boot_selection.active_valid) {
            HAL_GPIO_WritePin(BOOT_LED3_PORT, BOOT_LED3_PIN, GPIO_PIN_SET);
            boot_proto_run(false);   /* never returns */
        }
    }

    /* TRY-002: count this attempt before the application starts. */
    boot_selection_start_trial(&boot_selection);

    /* HND-003: slot and trial state for the application. */
    boot_selection_write_handover(&boot_selection);

    boot_dbg_begin('I');
    boot_dbg_str(boot_selection.active == BOOT_SLOT_A ? "boot slot A"
                                                      : "boot slot B");
    if (boot_selection.active_pending) {
        boot_dbg_str(", trial ");
        boot_dbg_dec(boot_selection.trial_count);
    }
    boot_dbg_end();

    /* TRY-004 / HND-004: a hung application resets and fails its trial. */
    boot_iwdg_start();

    /* HND-001/002: reset state, optional SWP_FB, jump. */
    boot_handover(boot_selection.active);
}
