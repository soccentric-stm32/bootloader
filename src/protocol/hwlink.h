/**
 * @file    hwlink.h
 * @brief   HWLINK framing: COBS, CRC-32 and the 8-byte header.
 * @details A raw frame is `proto[1] flags[1] seq[2] payload[n] crc32[4]`,
 *          little-endian, and is sent COBS-encoded with a 0x00 delimiter.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     HWLINK LNK-001..LNK-005 (SRS-GUI-F767 Appendix A)
 */

#ifndef HWLINK_H
#define HWLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Protocol major version placed in every frame header. */
#define HWLINK_PROTOCOL_MAJOR     1U

/** Protocol minor version reported in HelloReply. */
#define HWLINK_PROTOCOL_MINOR     3U

/** Maximum raw frame size, including header and CRC. */
#define HWLINK_MAX_FRAME          2048U

/** Worst-case COBS-encoded size, including the delimiter. */
#define HWLINK_MAX_ENCODED        (HWLINK_MAX_FRAME + (HWLINK_MAX_FRAME / 254U) + 2U)

/** Frame flag: this message is a response to a request. */
#define HWLINK_FLAG_RESPONSE      (1U << 0U)

/** Frame flag: this message is an unsolicited event. */
#define HWLINK_FLAG_EVENT         (1U << 1U)

/**
 * @brief   Build the unencoded frame (proto, flags, seq, payload, CRC-32).
 * @param[in]  payload       Payload bytes.
 * @param[in]  payload_len   Payload length.
 * @param[in]  flags         HWLINK_FLAG_* bits.
 * @param[in]  seq           Request sequence number.
 * @param[out] raw           Destination for the raw frame.
 * @param[in]  raw_capacity  Capacity of @p raw in bytes.
 * @param[out] raw_len       Encoded raw-frame length on success.
 * @return true on success; false on a NULL argument or a frame too large.
 */
bool hwlink_build_raw(const uint8_t *payload, size_t payload_len,
                      uint8_t flags, uint16_t seq,
                      uint8_t *raw, size_t raw_capacity, size_t *raw_len);

/**
 * @brief   COBS-encode a raw frame and append the 0x00 delimiter.
 * @param[in]  raw           Raw frame bytes.
 * @param[in]  raw_len       Raw frame length; 0 fails.
 * @param[out] out           Encoded frame destination.
 * @param[in]  out_capacity  Capacity of @p out in bytes.
 * @param[out] out_len       Encoded length, including the delimiter.
 * @return true on success; false on a NULL argument or insufficient space.
 */
bool hwlink_cobs_encode(const uint8_t *raw, size_t raw_len,
                        uint8_t *out, size_t out_capacity, size_t *out_len);

/**
 * @brief   COBS-decode one frame (without the delimiter).
 * @param[in]  in            Encoded bytes.
 * @param[in]  in_len        Encoded length.
 * @param[out] out           Decoded destination.
 * @param[in]  out_capacity  Capacity of @p out in bytes.
 * @param[out] out_len       Decoded length on success.
 * @return 0 on success, -1 on a malformed frame or insufficient space.
 */
int hwlink_cobs_decode(const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_capacity, size_t *out_len);

/**
 * @brief   Validate a raw frame and copy out its payload.
 * @details Checks the protocol version, flags and CRC-32.
 * @param[in]  raw              Raw frame bytes.
 * @param[in]  raw_len          Raw frame length.
 * @param[out] payload          Payload destination.
 * @param[in]  payload_capacity Capacity of @p payload in bytes.
 * @param[out] payload_len      Payload length on success.
 * @param[out] flags            Frame flags on success.
 * @param[out] seq              Request sequence number on success.
 * @return true when the frame is valid and the payload fits.
 */
bool hwlink_decode_raw(const uint8_t *raw, size_t raw_len,
                       uint8_t *payload, size_t payload_capacity,
                       size_t *payload_len, uint8_t *flags, uint16_t *seq);

#endif /* HWLINK_H */
