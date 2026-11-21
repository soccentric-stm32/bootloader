/**
 * @file    proto.h
 * @brief   HWLINK bootloader message service (SRS section 9).
 * @details Decodes host requests, drives the selection and update modules
 *          and replies with HWLINK messages.  One static context is
 *          installed by boot_proto_init(); no dynamic memory is used.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     BGEN-002, ENT-001, ENT-005, UPD-001..UPD-010, TRY-008, section 9
 */

#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>
#include <stdint.h>
#include "selection.h"
#include "update.h"

/** ENT-001: how long the boot path listens for a Hello, in ms. */
#define BOOT_LISTEN_WINDOW_MS   500U

/** Idle time after which update mode returns to the boot path, in ms. */
#define BOOT_IDLE_TIMEOUT_MS    120000U

/**
 * @brief   Install the selection/update contexts and start the protocol.
 * @param[in,out] selection  Selection context used by GetBootInfo.
 * @param[in,out] update     Update context driven by the update requests.
 */
void boot_proto_init(boot_selection_t *selection, boot_update_t *update);

/** @brief Process all bytes that the DMA ring has received. */
void boot_proto_poll(void);

/**
 * @brief   Wait up to @p window_ms for a Hello (ENT-001).
 * @details Other requests received during the window are answered
 *          normally.  Returns early when an UpdateBegin starts a session,
 *          so BOOT does not boot over a live update.
 * @param[in] window_ms  Listen window in ms.
 * @return true when a Hello arrived or an update session is active.
 */
bool boot_proto_listen_for_hello(uint32_t window_ms);

/**
 * @brief   Serve HWLINK requests until Reset/Activate or an idle timeout.
 * @details With @p can_boot false the loop never times out and is used
 *          while no slot is bootable.  Reset and Activate do not return.
 * @param[in] can_boot  true to allow the 120 s idle timeout.
 */
void boot_proto_run(bool can_boot);

#endif /* PROTO_H */
