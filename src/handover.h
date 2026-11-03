/*
 * handover.h - application checks and hand-over (section 10)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * SRS:     IMG-003, HND-001, HND-002
 */

#ifndef HANDOVER_H
#define HANDOVER_H

#include <stdbool.h>
#include <stdint.h>

/* IMG-003: initial SP in internal RAM, reset vector inside the slot. */
bool boot_app_vectors_valid(uint32_t slot_base);

/* HND-001/002: put the MCU in the reset state and start the application
 * whose vector table is at app_base. Does not return. */
void boot_handover(uint32_t app_base) __attribute__((noreturn));

#endif /* HANDOVER_H */
