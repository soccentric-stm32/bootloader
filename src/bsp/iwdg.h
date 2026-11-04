/*
 * iwdg.h - independent watchdog for the trial boot
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     TRY-004, HND-004
 */

#ifndef IWDG_H
#define IWDG_H

/* Start the IWDG with a 4 s timeout (LSI /64, reload 1999) and reload it.
 * Must be called immediately before the hand-over; the application has to
 * refresh it from its first instruction on (APP-004). */
void boot_iwdg_start(void);

/* Reload the running watchdog. */
void boot_iwdg_kick(void);

#endif /* IWDG_H */
