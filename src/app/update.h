/**
 * @file    update.h
 * @brief   Update state machine (SRS section 9).
 *
 * @details
 * Drives the four-phase host-mediated update protocol:
 *
 * 1. **begin** (@ref boot_update_begin) – validate the image descriptor,
 *    check the anti-rollback / signature rules, and prepare the target
 *    slot.  No bytes of the image have been written yet.
 * 2. **data** (@ref boot_update_data) – program the image in chunks.
 *    The host is told the *next expected offset* after each chunk so
 *    out-of-order chunks can be rejected deterministically (UPD-005
 *    out-of-order detection).
 * 3. **end** (@ref boot_update_end) – read the image back from flash and
 *    verify vectors, CRC-32 and (signed builds) SHA-256, then write the
 *    VERIFIED record so the selection module will consider the slot on
 *    the next boot.
 * 4. **activate** (@ref boot_update_activate) – write the ACTIVATED
 *    record with a saturated sequence number (other slot's sequence + 1,
 *    capped) and let the selection module arm the slot for launch.
 *    It can also arm a slot that a previous session verified (e.g. after
 *    a host reconnect).
 *
 * Any phase can be abandoned with @ref boot_update_abort, which erases
 * the target slot's metadata sector (so the half-built image has no
 * VERIFIED/ACTIVATED backing) and resets the session; an aborted
 * transfer leaves an inert, unverified image the next boot will ignore.
 *
 * **Error surface.** All state-changing entry points return a
 * @ref boot_error_t (mirrored 1-for-1 into the `hwlink.Error` enum) and
 * fill a caller-supplied detail buffer with a short, human-oriented
 * string.  `detail == NULL` is always acceptable; the detail is purely
 * diagnostic and must not be relied on for control flow.
 *
 * **Anti-rollback contract.** With `BOOT_SIGNED_IMAGES == 1`, an update
 * descriptor with an invalid or below-floor security counter is refused
 * at `begin` before anything is erased.  `force` does **not** override
 * signature / rollback checks - it only relaxes the
 * "don't touch the slot still on trial" restriction (a bench override
 * for debugging).
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     UPD-001..UPD-010, IMG-001..IMG-006
 */

#ifndef UPDATE_H
#define UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "boot_layout.h"
#include "image.h"
#include "selection.h"

/**
 * @brief   Error codes returned by the update state machine.
 *
 * @details Values are required to be 1-for-1 with the
 *          `hwlink.Error` enum in `protocol/hwlink.fbs`; a new error
 *          code added here must be added there (and vice versa).
 */
typedef enum {
    /** No error; operation succeeded. */
    BOOT_ERR_NONE           = 0,
    /** Malformed request (protocol-level: framing, field ranges). */
    BOOT_ERR_MALFORMED      = 1,
    /** Requested capability is not supported (wrong protocol version,
     *     unknown opcode, etc.). */
    BOOT_ERR_UNSUPPORTED    = 2,
    /** Argument outside the allowed range / shape. */
    BOOT_ERR_BAD_ARGUMENT   = 3,
    /** Requested operation is not valid in the current state (e.g.
     *     `data` before `begin`). */
    BOOT_ERR_BAD_STATE      = 4,
    /** Resource is still in use; try again later. */
    BOOT_ERR_BUSY           = 5,
    /** Requested resource does not exist (e.g. slot index out of range). */
    BOOT_ERR_NOT_FOUND      = 6,
    /** Image header fields fail their validity checks (UPD-002 header
     *     gate). */
    BOOT_ERR_BAD_HEADER     = 7,
    /** Image header layout is inconsistent (load address / size
     *     outside the slot). */
    BOOT_ERR_BAD_LAYOUT     = 8,
    /** Vector table at the slot base is invalid (IMG-003). */
    BOOT_ERR_BAD_VECTORS    = 9,
    /** Chunk arrived out of the expected sequence (UPD-005). */
    BOOT_ERR_OUT_OF_ORDER   = 10,
    /** Flash driver reported a write or verify failure. */
    BOOT_ERR_FLASH_ERROR    = 11,
    /** Image CRC-32 (over the image bytes) does not match the declared
     *     value. */
    BOOT_ERR_CRC_MISMATCH   = 12,
    /** Flash is not in the required dual-bank mode (BSAFE-005) and the
     *     requested operation cannot be completed. */
    BOOT_ERR_WRONG_FLASH_MODE = 13,
    /** Signature / security-counter verification failed (BSAFE-006).
     *     Protocol minor 3: first error code added in that minor. */
    BOOT_ERR_AUTH_FAILED    = 14,
    /** Security counter below the anti-rollback floor (BSAFE-006). */
    BOOT_ERR_ROLLBACK       = 15
} boot_error_t;

/**
 * @brief   Image descriptor as provided by the host in an
 *          `UpdateBegin` request.
 *
 * @details Fields mirror the on-wire `ImageHeader` table (see
 *          `protocol/hwlink.fbs`); the struct in this file is the
 *          bootloader's view of the same values and must stay
 *          byte-compatible (order, size, endianness) with the wire
 *          format.
 */
typedef struct {
    /** Image header magic ("IBWH", little-endian as decoded by the
     *     parser - the wire-format value, not yet byte-swapped). */
    uint32_t magic;
    /** Header version (1 = unsigned, 2 = signed per BSAFE-006). */
    uint16_t header_version;
    /** Total size of the on-disk header (256 bytes for every supported
     *     version the bootloader knows about). */
    uint16_t header_size;
    /** Image type; only `BOOT_IMAGE_TYPE_APP` is accepted. */
    uint32_t image_type;
    /** Monotonic image version number as declared by the builder. */
    uint32_t version;
    /** Total size of the image payload in bytes. */
    uint32_t image_size;
    /** CRC-32 over the image payload (must match the bootloader's
     *     computed value in @ref boot_update_end). */
    uint32_t image_crc32;
    /** Load address of the image (must be a legal in-slot address). */
    uint32_t load_address;
    /** Entry address (reset vector target of the image). */
    uint32_t entry_address;
    /** Build time, seconds since the Unix epoch (0 = unset). */
    uint32_t build_timestamp;
    /** Git commit hash (20 bytes) at build time; zeroed if unavailable. */
    uint8_t  git_commit[20];
    /** How many of `git_commit`[20] actually carry a hash; 0..20. */
    uint32_t git_commit_len;
    /** Human-readable image name (NUL-padded to `name_len`). */
    char     name[32];
    /** Used length of `name`. */
    uint32_t name_len;
    /** Optional header CRC-32 (over the on-disk header bytes); only
     *     present when `header_crc32_present` is true. */
    uint32_t header_crc32;
    /** True when `header_crc32` is a meaningful value to verify against. */
    bool     header_crc32_present;
    /**
     * @brief Header bytes 88..251 verbatim (protocol minor 3).
     *
     * @details The signed fields (security counter, key id, SHA-256
     *          digest, ECDSA signature) and the reserved tail of the
     *          on-disk header travel verbatim here so that the
     *          bootloader can reconstruct the exact header the signer
     *          signed.  Older hosts (pre-minor-3) will leave this
     *          absent and `extension_present` false, in which case the
     *          signed path is unavailable.
     */
    uint8_t  extension[BOOT_IMAGE_EXT_SIZE];
    /** True when the extension carries a coherent signed header. */
    bool     extension_present;
} boot_image_desc_t;

/**
 * @brief   Per-boot update state.
 *
 * @details One instance per boot; passed by reference through the
 *          @ref boot_update_* entry points.  `active` is the "a session
 *          is in progress" gate; every other field is only meaningful
 *          while it is true.
 */
typedef struct {
    /** True when an update session is in progress (between `begin` and
     *     a successful `end`/`activate`, or an aborted one). */
    bool active;
    /** True once @ref boot_update_end has verified the image back from
     *     flash (vectors, CRC, digest). */
    bool verified;
    /** Target slot the image is being programmed into (usually the
     *     inactive one). */
    boot_slot_t slot;
    /** Declared image size in bytes (from the descriptor). */
    uint32_t image_size;
    /** Declared image CRC-32 (from the descriptor). */
    uint32_t image_crc32;
    /** Next byte offset the host is expected to send. */
    uint32_t expected_offset;
    /** Offset of the most-recently-accepted chunk (0 if none). */
    uint32_t last_offset;
    /** True when `last_offset` is meaningful (at least one chunk
     *     accepted). */
    bool last_offset_valid;
    /** Bitmap of the slot sectors that have been erased for this
     *     session (bit N set = sector N erased). */
    uint32_t erased_mask;
    /** Reconstructed image header for this session (version 2 fields
     *     reconstructed from the descriptor's extension). */
    boot_image_header_t header;
    /**
     * @name Word-stage buffer (byte-alignment scratch)
     * @brief 4-byte scratch the driver accumulates before committing a
     *        full 32-bit machine word to flash.
     *
     * @details STM32F7 flash is programmed word-by-word (32-bit).  The
     *          host may hand chunks of any size from 1 to 1024 bytes
     *          that are not 4-byte aligned, so the driver keeps the
     *          leftover bytes here until a full word is ready.  The
     *          final partial word of an image is padded with 0xFF (the
     *          erased state) in @ref boot_update_end, and the image CRC
     *          only covers the declared `image_size` bytes.
     * @{
     */

    /** Buffer holding the in-flight 32-bit word (4 bytes, LSB first). */
    uint8_t word_stage[4];

    /** How many of `word_stage[4]` bytes are currently staged (0..3;
     *     4 is transient, immediately flushed by the driver). */
    uint32_t word_stage_len;

    /** Byte offset of the first staged byte (where `word_stage` will
     *     be committed). */
    uint32_t word_stage_offset;

    /** @} */
} boot_update_t;

/**
 * @brief   Progress reporting callback, set by the caller before any
 *          update entry point and invoked by the driver on every
 *          significant state change (erase, per-sector write, etc.).
 *
 * @param[in] operation  Short, stable operation name
 *                       (e.g. `"erase"`, `"write"`, `"verify"`).
 * @param[in] done       Bytes/units completed so far.
 * @param[in] total      Total bytes/units for the operation; 0 = unknown.
 */
typedef void (*boot_progress_fn)(const char *operation, uint32_t done,
                                 uint32_t total);

/**
 * @brief   Bring the state machine back to the idle state.
 *
 * @details Clear every bit of @ref boot_update_t - called at boot start
 *          (so a stale mid-session state cannot leak into a new boot),
 *          and as the post-success path after `activate`.
 *
 * @param[in,out] update  State to reset; all fields are cleared.
 */
void boot_update_reset(boot_update_t *update);

/**
 * @brief   Install (or clear) the progress callback used by the flash
 *          driver during this session.
 *
 * @param[in] callback  Progress callback, or `NULL` to disable.
 *
 * @note    A single global callback is used - concurrent sessions are
 *          not supported by design.
 */
void boot_update_set_progress_cb(boot_progress_fn callback);

/**
 * @brief   Phase 1: begin an update in the target slot (UPD-002).
 *
 * @details Validation order, with the P0-1 trial guard checked first:
 *
 * 1. **P0-1 trial guard** (skipped when `force`): refuse to begin an
 *    update while the active slot is still in its trial window - the
 *    other slot is the only known-good rollback image and erasing its
 *    metadata would lose it.  `force` is a bench-only override of this
 *    gate only.
 * 2. Request shape: `update`/`desc` non-NULL (else
 *    `BOOT_ERR_MALFORMED`).
 * 3. **BNFR-003 resumption**: if a stale session is still `active`,
 *    reset it (a lost link must not wedge the update path) and re-run
 *    from scratch - the metadata sector is re-erased and slot sectors
 *    are erased lazily on first write.
 * 4. Descriptor gate (UPD-002): magic, version (1 or 2), header size,
 *    image type, load address, image size range - refused with
 *    `BOOT_ERR_BAD_HEADER` or `BOOT_ERR_BAD_LAYOUT`.
 * 5. Header reconstruction + optional host-provided header-CRC check
 *    (IMG-001).  Old hosts omit the field and BOOT recomputes it.
 * 6. **BSAFE-006 authenticate-before-erase**: the reconstructed header
 *    is fully checked (signature, key, format) and its security counter
 *    compared to @ref boot_selection_security_floor, so a foreign or
 *    below-floor image cannot even destroy the inactive slot.
 * 7. Only after /all/ gates pass is the metadata sector erased and the
 *    header record written; the session state is initialised and
 *    `active` is set.
 *
 * @param[in,out] update     State to initialise for this session.
 * @param[in]     selection  Current selection context; may be NULL, in
 *                           which case the trial-window and rollback
 *                           guards are skipped.
 * @param[in]     desc       Descriptor from the host.
 * @param[in]     force      Bench-only override for the P0-1 trial
 *                           guard.  **Does not** relax signature /
 *                           rollback checks (BSAFE-006 is absolute).
 * @param[out]    detail     Diagnostics buffer (may be NULL).
 * @param[in]     detail_cap Capacity of @ref detail in bytes (0 to
 *                           disable).
 *
 * @return  @ref BOOT_ERR_NONE on success; otherwise a
 *          @ref boot_error_t code matching the gate that failed.
 *
 * @srs     UPD-002, P0-1, BNFR-003, IMG-001, BSAFE-006
 */
boot_error_t boot_update_begin(boot_update_t *update,
                               const boot_selection_t *selection,
                               const boot_image_desc_t *desc, bool force,
                               char *detail, size_t detail_cap);

/**
 * @brief   Phase 2: accept one chunk of image data (UPD-003/004/005).
 *
 * @details The host must send chunks at the `expected_offset` exactly.
 *          A **repeated** chunk at the same offset as the previously
 *          accepted chunk (`last_offset`) is acknowledged again with
 *          the same `next_offset` and no re-programming (UPD-004 retry
 *          semantics - a lost reply cannot be distinguished, so a
 *          repeat is always safe).  Any other out-of-order offset is
 *          refused with `BOOT_ERR_OUT_OF_ORDER` (UPD-005).
 *
 * Chunk size is 1..1024 bytes (`BOOT_ERR_BAD_ARGUMENT` otherwise) and
 * must not extend past `image_size`.  Sectors are erased lazily on the
 * first byte that falls into them (UPD-003), tracked in `erased_mask`.
 * A flash programming failure aborts the whole session (the host must
 * `begin` again) - re-writing half a chunk in place is not supported.
 *
 * @param[in,out] update     Active session; must have been `begin`-ed
 *                           before the first call.
 * @param[in]     offset     Byte offset of the first byte of this chunk.
 * @param[in]     data       Chunk payload.
 * @param[in]     len        Chunk length in bytes (1..1024).
 * @param[out]    next_offset  On success, byte offset of the next chunk
 *                           to send.  May be NULL; on the early error
 *                           paths it is best-effort (the session's
 *                           `expected_offset` if a session is known).
 * @param[out]    detail     Diagnostics buffer (may be NULL).
 * @param[in]     detail_cap Capacity of @ref detail (0 to disable).
 *
 * @return  @ref BOOT_ERR_NONE on success (including the UPD-004
 *          repeated-chunk case); @ref BOOT_ERR_MALFORMED for NULL
 *          pointers; @ref BOOT_ERR_BAD_STATE when no session is
 *          active; @ref BOOT_ERR_BAD_ARGUMENT for bad `len` or a chunk
 *          past `image_size`; @ref BOOT_ERR_OUT_OF_ORDER for an offset
 *          that is neither `expected_offset` nor `last_offset`;
 *          @ref BOOT_ERR_FLASH_ERROR on a flash-layer failure (and the
 *          session is aborted by the driver).
 *
 * @srs     UPD-003, UPD-004, UPD-005
 */
boot_error_t boot_update_data(boot_update_t *update, uint32_t offset,
                              const uint8_t *data, uint32_t len,
                              uint32_t *next_offset,
                              char *detail, size_t detail_cap);

/**
 * @brief   Phase 3: finish the transfer and verify (UPD-006).
 *
 * @details On exit the target slot is a coherent, verified image:
 *
 * - the transfer is complete (UPD-006 gate: `expected_offset ==
 *   image_size`),
 * - the final partial word (if any) is flushed with 0xFF padding,
 * - vectors are valid (IMG-003),
 * - CRC-32 over the slot matches the descriptor (UPD-006),
 * - (signed build) SHA-256 over the slot matches the extension digest
 *   (BSAFE-006),
 * - and the VERIFIED record is written so that the selection module
 *   will consider the slot on the next boot.
 *
 * A repeated `UpdateEnd` (lost reply) is a no-op returning
 * `BOOT_ERR_NONE` and re-reporting the committed CRC (idempotent).
 *
 * @param[in,out] update    Active session.
 * @param[out]    image_crc On success, the CRC-32 the driver computed
 *                          over the slot (for host-side correlation);
 *                          zeroed on entry when non-NULL.
 * @param[out]    detail    Diagnostics buffer (may be NULL).
 * @param[in]     detail_cap Capacity of @ref detail (0 to disable).
 *
 * @return  BOOT_ERR_NONE on success (including the repeated-call
 *          case); @ref BOOT_ERR_BAD_STATE when the session is not
 *          active or the transfer is incomplete; @ref
 *          BOOT_ERR_BAD_VECTORS, @ref BOOT_ERR_CRC_MISMATCH or
 *          @ref BOOT_ERR_AUTH_FAILED on verification failure; @ref
 *          BOOT_ERR_FLASH_ERROR on a flash-layer failure.
 *
 * @note    For a large image the verification itself can take visible
 *          time; progress is reported before and after it (UPD-009).
 *
 * @srs     UPD-006, UPD-009, IMG-003, BSAFE-006
 */
boot_error_t boot_update_end(boot_update_t *update, uint32_t *image_crc,
                             char *detail, size_t detail_cap);

/**
 * @brief   Phase 4: arm the slot for launch (UPD-007).
 *
 * @details Two accepted shapes:
 *
 * - **current session**: `update` is active and verified - the target
 *   slot is `update->slot`.
 * - **previous session (host re-connect)**: `update` is idle or NULL -
 *   the target slot is the current flash target and must already
 *   carry a VERIFIED record (so a completed transfer can be finished
 *   after a reset / lost link).
 *
 * Gates (refusing with `BOOT_ERR_BAD_STATE` unless noted):
 *
 * 1. Slot not verified (or, in the previous-session shape, no VERIFIED
 *    record present).
 * 2. Slot already ACTIVATED (idempotency is /not/ provided - callers
 *    should check the selection context before re-activating).
 * 3. Slot is REJECTED (or the REJECTED area is non-erased - a torn
 *    record, treated conservatively as rejected).
 * 4. Slot no longer passes the full image check (header / vectors /
 *    CRC) - e.g. a concurrent write corrupted it.
 * 5. Security counter below the installed floor (BSAFE-006; this gate
 *    returns `BOOT_ERR_ROLLBACK`).
 * 6. Active slot still in its trial window (the other slot is the
 *    rollback image).
 *
 * Sequence number: read from the /other/ slot's ACTIVATED record (0
 * when absent or erased) and **saturating-incremented** at
 * `0xFFFFFFFE`.  (A plain `+1` from `0xFFFFFFFE` would wrap to 1 and
 * lose to the other slot's sequence, so it is capped.)
 *
 * On success: the ACTIVATED record is programmed, the trial counter is
 * cleared, the last-outcome record is cleared, and (if active) the
 * session's `active` flag is dropped.
 *
 * @param[in,out] update    Session (NULL or idle to activate a
 *                          previously-verified slot).
 * @param[in]     selection Current selection context; used for the
 *                          rollback-floor and trial-window guards (may
 *                          be NULL, in which case those two gates are
 *                          skipped).
 * @param[out]    detail    Diagnostics buffer (may be NULL).
 * @param[in]     detail_cap Capacity of @ref detail (0 to disable).
 *
 * @return  @ref BOOT_ERR_NONE on success; @ref BOOT_ERR_BAD_STATE on
 *          any gate refusal; @ref BOOT_ERR_ROLLBACK for a below-floor
 *          security counter; @ref BOOT_ERR_FLASH_ERROR if the ACTIVATED
 *          record could not be programmed.
 *
 * @srs     UPD-007, BNFR-003
 */
boot_error_t boot_update_activate(boot_update_t *update,
                                  const boot_selection_t *selection,
                                  char *detail, size_t detail_cap);

/**
 * @brief   Abandon the in-flight update (UPD-008).
 *
 * @details Erases the target slot's metadata sector (so any partial
 *          metadata the session carried is gone) and resets the session
 *          state.  Image bytes already programmed in the slot are left
 *          in place; without the VERIFIED / ACTIVATED records the
 *          selection module will not consider the slot on the next
 *          boot.
 *
 * Safe to call from any state, including `NULL` (no-op).
 *
 * @param[in,out] update    State to reset back to idle (NULL is a no-op).
 *
 * @srs     UPD-008
 */
void boot_update_abort(boot_update_t *update);

#endif /* UPDATE_H */
