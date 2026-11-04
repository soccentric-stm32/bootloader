/*
 * proto.h - HWLINK bootloader message service (section 9)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     BGEN-002, ENT-001, ENT-005, UPD-001..UPD-010, TRY-008, §9
 */

#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>
#include <stdint.h>
#include "selection.h"
#include "update.h"

#define BOOT_LISTEN_WINDOW_MS   500U
#define BOOT_IDLE_TIMEOUT_MS    120000U

void boot_proto_init(boot_selection_t *selection, boot_update_t *update);

/* Process all bytes that the DMA ring has received. */
void boot_proto_poll(void);

/* ENT-001: wait up to window_ms for a Hello.  Returns true when one arrived.
 * Other requests received in the window are answered normally. */
bool boot_proto_listen_for_hello(uint32_t window_ms);

/* Serve HWLINK until Reset/Activate (never returns) or until 120 s pass
 * without a valid frame.  can_boot is false while no slot is valid. */
void boot_proto_run(bool can_boot);

#endif /* PROTO_H */
