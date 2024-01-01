/**
 * @file    iwdg.h
 * @brief   Independent watchdog for the trial boot.
 * @details BOOT starts the IWDG just before hand-over with a 4 s timeout; a
 *          hung application resets and the next boot counts the failed trial.
 *          The application must refresh it from start-up (APP-004).
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     TRY-004, HND-004
 */

#ifndef IWDG_H
#define IWDG_H

/**
 * @brief   Start the IWDG with a 4 s timeout and reload it once.
 * @details LSI / 64, reload 1999.  LSI is independent of the main clock
 *          tree, so the watchdog survives boot_clock_deinit().  Once
 *          started it runs until reset.  Waits for LSI and the register
 *          updates are bounded.
 *
 * @warning Call only immediately before hand-over.
 * @post    The watchdog runs; the application must refresh it (APP-004).
 *
 * @srs     TRY-004, HND-004, APP-004
 */
void boot_iwdg_start(void);

/**
 * @brief   Reload the watchdog counter.
 * @details One register write, non-blocking.  Harmless when the IWDG is not
 *          running, so BOOT calls it around long flash erases and in its
 *          protocol loops.
 *
 * @srs     APP-004 (application side), TRY-004 (boot side)
 */
void boot_iwdg_kick(void);

#endif /* IWDG_H */
