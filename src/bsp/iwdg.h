/**
 * @file    iwdg.h
 * @brief   Independent watchdog for the trial boot.
 *
 * @details
 * The IWDG is the last line of defence for the trial-boot mechanism
 * (TRY-*): it is started by BOOT immediately before the hand-over, and
 * the application is expected to start refreshing it from its very first
 * executed instruction (APP-004).  If the application hangs, the IWDG
 * fires, the board resets, and the selection logic on the next boot counts
 * that trial against the pending slot.
 *
 * The timeout is deliberately a single, known value (4 seconds) rather
 * than a "long enough" window - it is short enough that a stuck board
 * recovers quickly, but long enough that a normal application boot
 * sequence (init, drivers, first `kick`) comfortably fits inside it.
 *
 * The exact LSI prescaler / reload pair is an implementation detail of
 * @c iwdg.c; the two public entry points below are the only interface
 * other modules should use.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     TRY-004, HND-004
 */

#ifndef IWDG_H
#define IWDG_H

/**
 * @brief   Configure and start the IWDG, then do an initial reload.
 *
 * @details
 * - Prescaler /64, reload 1999, giving a 4-second timeout from the LSI
 *   (LSI is internally generated and running independently of the
 *   main clock tree - so this works even if the PLL has been stopped
 *   by a prior hand-over deinit, see @ref boot_clock_deinit).
 * - After the start window, the watchdog is running and cannot be
 *   stopped without a reset; the only lever from then on is
 *   @ref boot_iwdg_kick.
 *
 * @warning **Must be called immediately before the hand-over** and
 *          *never* earlier, otherwise the bootloader's own startup
 *          would be at risk from a spurious refresh window.  This is
 *          a single-shot start: calling it twice in the same boot is
 *          harmless (the hardware accepts the reconfigure) but
 *          pointless.
 *
 * @pre     LSI is available (it is, by default, and is independent of
 *          the main clock tree).
 * @post    The watchdog is running and has been reloaded once;
 *          the application must start kicking it (APP-004).
 *
 * @srs     TRY-004, HND-004, APP-004
 */
void boot_iwdg_start(void);

/**
 * @brief   Reload (kick) the running watchdog.
 *
 * @details Cheap, non-blocking.  Call this at regular intervals from
 *          the application's main loop (or from a heartbeat task) to
 *          keep the watchdog from firing.  The interval should be well
 *          inside the 4 s timeout (e.g. every 100 ms).
 *
 * @pre     @ref boot_iwdg_start has already been called in this boot
 *          (kicking before start is a no-op - the window is only
 *          active after the start).
 *
 * @srs     APP-004 (application side), TRY-004 (boot side)
 */
void boot_iwdg_kick(void);

#endif /* IWDG_H */
