/*
 * handover.h - application checks and hand-over (section 10)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * SRS:     IMG-003, HND-001..HND-004, SWP-001/002
 */

#ifndef HANDOVER_H
#define HANDOVER_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_layout.h"

/* Copy the .ramfunc section from flash to DTCM; call once after reset. */
void boot_ramfunc_init(void);

/* HND-001..HND-004: put the MCU in the reset state and start the application
 * in the given slot, applying SWP_FB for slot B.  Does not return. */
void boot_handover(boot_slot_t slot) __attribute__((noreturn));

#endif /* HANDOVER_H */
