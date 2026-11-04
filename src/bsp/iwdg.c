/*
 * iwdg.c - independent watchdog for the trial boot
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     TRY-004 (4 s build-time timeout), HND-004
 *
 * LSI is nominally 32 kHz; prescaler /64 and reload 1999 give 4.0 s.
 * The watchdog is started once, right before the hand-over, so that a hung
 * application resets and counts as a failed trial.  BOOT itself does not
 * need to kick it because it does not return from the hand-over.
 */

#include "stm32f7xx_hal.h"
#include "iwdg.h"

#define IWDG_KEY_ACCESS   0x00005555UL
#define IWDG_KEY_RELOAD   0x0000AAAAUL
#define IWDG_KEY_ENABLE   0x0000CCCCUL
#define IWDG_RELOAD_4S    1999UL
#define IWDG_PR_DIV64     4U      /* PR encoding for LSI / 64 */
/* ~40 ms at 216 MHz: generous for five LSI cycles (~160 us), short enough
 * that a dead LSI cannot hang the boot. */
#define IWDG_WAIT_LOOPS   1000000UL

/* Start the LSI and wait for it with a bound.  Without LSI the PVU/RVU
 * flags never clear, which used to spin here forever. */
static void iwdg_start_lsi(void)
{
    RCC->CSR |= RCC_CSR_LSION;
    for (uint32_t i = 0U; i < IWDG_WAIT_LOOPS; i++) {
        if ((RCC->CSR & RCC_CSR_LSIRDY) != 0U) {
            return;
        }
    }
}

static void iwdg_wait_ready(void)
{
    for (uint32_t i = 0U; i < IWDG_WAIT_LOOPS; i++) {
        if ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU)) == 0U) {
            return;
        }
    }
}

void boot_iwdg_kick(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}

void boot_iwdg_start(void)
{
    /* LSI first, so the prescaler/reload updates can complete. */
    iwdg_start_lsi();
    /* ST's sequence (HAL_IWDG_Init): enable the watchdog, unlock PR/RLR,
     * program them, wait for the update to land, then reload. */
    IWDG->KR = IWDG_KEY_ENABLE;
    IWDG->KR = IWDG_KEY_ACCESS;
    IWDG->PR = IWDG_PR_DIV64;
    IWDG->RLR = IWDG_RELOAD_4S;
    iwdg_wait_ready();
    IWDG->KR = IWDG_KEY_RELOAD;
}
