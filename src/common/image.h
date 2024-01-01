/**
 * @file    image.h
 * @brief   Application image header (section 5) and its checks.
 * @details Header version 2 puts an anti-rollback counter, the signing key
 *          id, the image SHA-256 and an ECDSA P-256 signature over bytes
 *          0..127 into the version 1 reserved area.  Size and all version 1
 *          offsets are unchanged.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     IMG-001..IMG-006, section 5.1, BSAFE-006
 */

#ifndef IMAGE_H
#define IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "boot_layout.h"

#define BOOT_IMAGE_MAGIC          0x48574249UL   /**< "IBWH" little-endian. */
#define BOOT_IMAGE_HEADER_VERSION 2U   /**< Signed header (BSAFE-006). */
#define BOOT_IMAGE_HEADER_VERSION_UNSIGNED 1U /**< CRC-only header. */
#define BOOT_IMAGE_HEADER_SIZE    256U /**< Header size in bytes. */
#define BOOT_IMAGE_TYPE_APP       1U   /**< image_type of an application. */

/**
 * BSAFE-006: 1 accepts only signed (version 2) images; 0 keeps version 1
 * CRC-only behaviour for bench work.  Set by the BOOT_SIGNED_IMAGES CMake
 * option.
 */
#ifndef BOOT_SIGNED_IMAGES
#define BOOT_SIGNED_IMAGES        1
#endif

/**
 * @brief Image header stored at offset 0 of the metadata sector.
 * @note Wire and flash layout shared with mkimage.py and the
 *       applications; do not reorder.  Little-endian.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;               /**< BOOT_IMAGE_MAGIC. */
    uint16_t header_version;      /**< 1 = unsigned, 2 = signed. */
    uint16_t header_size;         /**< BOOT_IMAGE_HEADER_SIZE. */
    uint32_t image_type;          /**< BOOT_IMAGE_TYPE_APP. */
    uint32_t version;             /**< Application version. */
    uint32_t image_size;          /**< Image bytes, 8..BOOT_SLOT_SIZE. */
    uint32_t image_crc32;         /**< CRC-32 of the image_size bytes. */
    uint32_t load_address;        /**< BOOT_APP_LOAD_ADDR. */
    uint32_t entry_address;       /**< Reset handler address. */
    uint32_t build_timestamp;     /**< Build time, Unix seconds. */
    uint8_t  git_commit[20];      /**< SHA-1 of the source commit. */
    char     name[32];            /**< Image name, NUL-padded. */
    /* Version 2 (BSAFE-006).  Zero in a version 1 header. */
    uint32_t security_counter;    /**< Anti-rollback counter, offset 88. */
    uint32_t key_id;              /**< First 4 bytes of SHA-256(pubkey). */
    uint8_t  image_sha256[32];    /**< SHA-256 of the image_size bytes. */
    uint8_t  signature[64];       /**< ECDSA P-256 r || s, big-endian. */
    uint8_t  reserved[60];        /**< Zero. */
    uint32_t header_crc32;        /**< CRC-32 of bytes 0..251. */
} __attribute__((packed, aligned(4))) boot_image_header_t;

_Static_assert(sizeof(boot_image_header_t) == BOOT_IMAGE_HEADER_SIZE,
               "image header must be 256 bytes");

/**
 * Offset of header bytes 88..251 (the version 1 reserved area), which travel
 * verbatim in hwlink.ImageHeader.extension (protocol minor 3).
 */
#define BOOT_IMAGE_EXT_OFFSET     88U
#define BOOT_IMAGE_EXT_SIZE       164U /**< Extension length in bytes. */
/** Signed prefix length: the signature covers header bytes 0..127. */
#define BOOT_IMAGE_SIGNED_SIZE    128U

_Static_assert(offsetof(boot_image_header_t, security_counter) ==
                   BOOT_IMAGE_EXT_OFFSET, "extension offset");
_Static_assert(offsetof(boot_image_header_t, signature) ==
                   BOOT_IMAGE_SIGNED_SIZE, "signed prefix");
_Static_assert(offsetof(boot_image_header_t, header_crc32) ==
                   BOOT_IMAGE_EXT_OFFSET + BOOT_IMAGE_EXT_SIZE,
               "extension size");

/**
 * @brief Check magic, versions, type, header CRC and layout (IMG-001/002).
 * @details With BOOT_SIGNED_IMAGES, also require version 2, BOOT's key id
 *          and a valid signature.  Vector and image CRC checks need the
 *          slot; see boot_image_valid().
 * @param[in] header Header to check; NULL fails.
 * @return true when boot_image_header_check() returns BOOT_HEADER_OK.
 */
bool boot_image_header_valid(const boot_image_header_t *header);

/**
 * @brief Reason boot_image_header_check() rejected a header (BSAFE-006).
 */
typedef enum {
    BOOT_HEADER_OK = 0,          /**< Header accepted. */
    BOOT_HEADER_BAD_FORMAT,      /**< Magic, version, size, type or CRC. */
    BOOT_HEADER_BAD_LAYOUT,      /**< Load address or image size. */
    BOOT_HEADER_UNSIGNED,        /**< Version 1 while signing is required. */
    BOOT_HEADER_WRONG_KEY,       /**< key_id is not BOOT's key. */
    BOOT_HEADER_BAD_SIGNATURE    /**< ECDSA verification failed. */
} boot_header_status_t;

/**
 * @brief Check a header and report the first failure.
 * @param[in] header Header to check; NULL gives BOOT_HEADER_BAD_FORMAT.
 * @return BOOT_HEADER_OK or the first failing check.
 */
boot_header_status_t boot_image_header_check(
    const boot_image_header_t *header);

/**
 * @brief Compare the SHA-256 of the slot with header->image_sha256.
 * @param[in] slot   Slot to hash.
 * @param[in] header Header of that slot; NULL fails.
 * @return true on match; always true when BOOT_SIGNED_IMAGES is 0.
 * @srs BSAFE-006
 */
bool boot_image_digest_matches(boot_slot_t slot,
                               const boot_image_header_t *header);

/**
 * @brief Check the slot's vector table (IMG-003).
 * @details Initial SP must be 8-byte aligned in (BOOT_RAM_START,
 *          BOOT_RAM_END]; the reset vector must be a Thumb address in the
 *          slot.
 * @param[in] slot Slot to check.
 * @return true when both vectors are plausible.
 */
bool boot_image_vectors_valid(boot_slot_t slot);

/**
 * @brief Compare the CRC-32 of the slot read from flash with the header.
 * @param[in] slot   Slot to checksum.
 * @param[in] header Header of that slot; NULL fails.
 * @return true on match.
 * @srs IMG-004, IMG-005
 */
bool boot_image_crc_matches(boot_slot_t slot,
                            const boot_image_header_t *header);

/**
 * @brief Run the header, vector, image CRC and digest checks (SEL-001).
 * @param[in] slot   Slot to check.
 * @param[in] header Header of that slot.
 * @return true when every check passes.
 */
bool boot_image_valid(boot_slot_t slot, const boot_image_header_t *header);

/**
 * @brief Fill the constant fields, zero reserved, and set header_crc32.
 * @details The caller sets version, image_size, image_crc32, entry_address,
 *          build_timestamp, git_commit, name, header_version and, if signed,
 *          the version 2 fields.  header_version 0 becomes 1 (unsigned).
 * @param[in,out] header Header to finish; NULL is ignored.
 */
void boot_image_header_finalize(boot_image_header_t *header);

#endif /* IMAGE_H */
