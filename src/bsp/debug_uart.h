/**
 * @file    debug_uart.h
 * @brief   Text debug output on USART6 (PG14, Arduino D1), TX only.
 *
 * @details
 * USART3 carries HWLINK only, so human-readable debug lines go to USART6
 * at 115200 8N1.  Every firmware in the family uses the same port and the
 * same line format, so one terminal shows BOOT and then the application:
 *
 *     [   1234] BOOT I sel: slot A confirmed seq 5
 *
 * The driver is register-level and polled: no HAL UART, no interrupts and
 * no printf, because BOOT has about 1.6 KB of its 32 KB left.  A line is
 * built from the primitives below:
 *
 *     boot_dbg_begin('I'); boot_dbg_str("boot slot "); boot_dbg_dec(n);
 *     boot_dbg_end();
 *
 * With BOOT_DEBUG_UART=0 (CMake option, ON by default) every call is an
 * empty inline function and the build is identical to one without
 * debug output.  Product builds should turn it off.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-06
 * @target  STM32F767ZIT6
 * @srs     BDBG-001 (USART6 debug output), HND-001 (hand-over cleanup)
 */

#ifndef DEBUG_UART_H
#define DEBUG_UART_H

#include <stdint.h>

#ifndef BOOT_DEBUG_UART
#define BOOT_DEBUG_UART 0
#endif

#if BOOT_DEBUG_UART

/** Configure PG14 as USART6_TX (AF8) and start USART6 at 115200 8N1. */
void boot_dbg_init(void);

/** Start a line: timestamp, the "BOOT" tag and the level (E, W, I, D). */
void boot_dbg_begin(char level);

/** Append a NUL-terminated string. */
void boot_dbg_str(const char *text);

/** Append an unsigned decimal number. */
void boot_dbg_dec(uint32_t value);

/** Append "0x" and eight hex digits. */
void boot_dbg_hex(uint32_t value);

/** End the line (CR LF). */
void boot_dbg_end(void);

/**
 * @brief   Drain, then return USART6 and PG14 to their reset state.
 *
 * Called by the hand-over so the application finds USART6 untouched
 * (HND-001) and can claim it for its own debug output.
 */
void boot_dbg_deinit(void);

#else

static inline void boot_dbg_init(void) {}
static inline void boot_dbg_begin(char level) { (void)level; }
static inline void boot_dbg_str(const char *text) { (void)text; }
static inline void boot_dbg_dec(uint32_t value) { (void)value; }
static inline void boot_dbg_hex(uint32_t value) { (void)value; }
static inline void boot_dbg_end(void) {}
static inline void boot_dbg_deinit(void) {}

#endif /* BOOT_DEBUG_UART */

#endif /* DEBUG_UART_H */
