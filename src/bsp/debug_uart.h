/**
 * @file    debug_uart.h
 * @brief   Text debug output on USART6 (PG14, Arduino D1), TX only.
 * @details USART3 carries HWLINK only, so debug lines go to USART6 at
 *          115200 8N1 in the format every firmware in the family uses:
 *
 *     [   1234] BOOT I sel: slot A confirmed seq 5
 *
 *          Register-level and polled: no HAL UART, no interrupts, no printf,
 *          to fit BOOT's 32 KB.  Build a line from the primitives:
 *
 *     boot_dbg_begin('I'); boot_dbg_str("boot slot "); boot_dbg_dec(n);
 *     boot_dbg_end();
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-06
 * @target  STM32F767ZIT6
 * @srs     BDBG-001 (USART6 debug output), HND-001 (hand-over cleanup)
 */

#ifndef DEBUG_UART_H
#define DEBUG_UART_H

#include <stdint.h>

/**
 * 1 enables debug output (BOOT_DEBUG_UART CMake option).  0 turns every
 * call into an empty inline; product builds should use 0.
 */
#ifndef BOOT_DEBUG_UART
#define BOOT_DEBUG_UART 0
#endif

#if BOOT_DEBUG_UART

/** @brief Configure PG14 as USART6_TX (AF8) and start USART6 at 115200 8N1. */
void boot_dbg_init(void);

/**
 * @brief Start a line: timestamp, "BOOT" tag and level.
 * @param[in] level 'E', 'W', 'I' or 'D'.
 */
void boot_dbg_begin(char level);

/**
 * @brief Append a string.
 * @param[in] text NUL-terminated string.
 */
void boot_dbg_str(const char *text);

/**
 * @brief Append an unsigned decimal number.
 * @param[in] value Number to print.
 */
void boot_dbg_dec(uint32_t value);

/**
 * @brief Append "0x" and eight lower-case hex digits.
 * @param[in] value Number to print.
 */
void boot_dbg_hex(uint32_t value);

/** @brief End the line with CR LF. */
void boot_dbg_end(void);

/**
 * @brief Drain, then return USART6 and PG14 to their reset state.
 * @details Called by the hand-over so the application finds USART6
 *          untouched (HND-001).
 */
void boot_dbg_deinit(void);

#else

/** @name No-op stubs used when BOOT_DEBUG_UART is 0
 * @{
 */
static inline void boot_dbg_init(void) {}
static inline void boot_dbg_begin(char level) { (void)level; }
static inline void boot_dbg_str(const char *text) { (void)text; }
static inline void boot_dbg_dec(uint32_t value) { (void)value; }
static inline void boot_dbg_hex(uint32_t value) { (void)value; }
static inline void boot_dbg_end(void) {}
static inline void boot_dbg_deinit(void) {}
/** @} */

#endif /* BOOT_DEBUG_UART */

#endif /* DEBUG_UART_H */
