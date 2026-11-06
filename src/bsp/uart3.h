/**
 * @file    uart3.h
 * @brief   USART3 on the ST-LINK virtual COM port.
 *
 * @details
 * The update protocol in BOOT speaks over the ST-LINK VCP on NUCLEO-F767ZI
 * (TX/PD8, RX/PD9).  This wrapper provides the lowest-level serial
 * primitives the high-level protocol (`src/protocol/`) needs:
 *
 * - a circular receive ring (4 KB) filled by DMA, so the UART can
 *   "buffer" host data while the flash is mid-program (UPD-003 / BGEN-010),
 * - a two-rate mode switch (115200 ↔ 921600) negotiated with the host,
 * - a blocking-but-timed send with the last stop bit explicitly drained
 *   before the caller assumes the byte is on the wire.
 *
 * The module is intentionally *not* line-oriented.  It is a raw byte
 * stream; framing is the job of the HWLINK protocol above it.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (PD8 TX, PD9 RX)
 * @srs     BGEN-004, BGEN-010, ENT-001, HND-001
 */

#ifndef UART3_H
#define UART3_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @name UART baud rates.
 * @brief Two supported rates: a conservative default and the fast
 *        transfer rate (921600).
 * @{
 */

/** @brief Conservative default: 115200 8N1. */
#define BOOT_UART_DEFAULT_BAUD  115200U

/**
 * @brief Fast transfer rate: 921600 8N1.
 *
 * @details Chosen at the PCLK1 edge (54 MHz after the /4 APB1 divider)
 *          such that the baud-rate generator error stays within
 *          0.01 %, which the protocol's framing tolerances depend on
 *          (see BGEN-004).
 */
#define BOOT_UART_FAST_BAUD     921600U

/** @} */

/**
 * @name Receive ring geometry.
 * @{
 * @brief  4 KiB circular receive ring in a dedicated DMA section.
 *
 * @details Sized to comfortably absorb a burst of host data while a
 *          flash program / erase is in progress (the slowest operation
 *          the update flow does).  The ring lives in the `.dma_buffer`
 *          linker section (32-byte aligned) - it is /not/ in flash -
 *          so the DMA can own it while the CPU is busy elsewhere.
 */

/** @brief Total bytes in the receive ring (4096). */
#define BOOT_UART_RX_RING_SIZE  4096U

/** @} */

/**
 * @brief   Bring up USART3 (8N1, no HW flow control, 16x oversampling,
 *          default baud 115200) and arm the DMA receiver.
 *
 * @details `HAL_UART_Init` runs the MSP init path in this file, which
 *          enables the UART and DMA clocks, configures PD8/PD9 as
 *          AF7 push-pull / pull-up, and sets up DMA1 Stream 1 (channel
 *          4) for peripheral-to-memory transfers in *circular* mode with
 *          a very-high priority.  On top of that, `boot_uart3_init()`
 *          resets the ring read pointer and starts an ongoing DMA
 *          receive across the whole ring.
 *
 * The send path is polled (via `HAL_UART_Transmit`): a reply is at most
 * a few hundred bytes, so the DMA's throughput is not needed and the
 * simpler code path keeps the send path easy to reason about.
 *
 * @return  `true` on success (UART ready, DMA armed); `false` on any
 *          failure in the HAL initialisation or DMA start.
 *
 * @srs     BGEN-004, BGEN-010
 */
bool boot_uart3_init(void);

/**
 * @brief   Switch the UART baud rate (either direction).
 *
 * @details Only `BOOT_UART_DEFAULT_BAUD` and `BOOT_UART_FAST_BAUD` are
 *          accepted; any other value returns `false` immediately.
 *          Calling with the current rate is a no-op (returns true).
 *
 *          The switch is done as a full deinit / re-init pair: stop the
 *          DMA receiver, de-init the UART, re-init at the new baud, and
 *          re-arm the receiver.  On any failure the old baud is restored
 *          (best-effort - a re-init to the previous rate is attempted).
 *
 * @warning **The caller must have sent an Ack to the host before calling
 *          this**, otherwise the host's bytes will arrive at the old
 *          rate against a UART that has switched to the new one and will
 *          be misframed.  (This is the contract noted in the original
 *          header comment, now formalised here.)
 *
 * @param[in] baud  Target baud rate (one of the two above).
 * @return  `true` on success (including the "already at that rate"
 *          case), `false` if @ref baud is unsupported.
 *
 * @srs     BGEN-004
 */
bool boot_uart3_set_baud(uint32_t baud);

/**
 * @brief   Query the currently-configured baud rate.
 *
 * @return  `BOOT_UART_DEFAULT_BAUD` or `BOOT_UART_FAST_BAUD`.
 */
uint32_t boot_uart3_baud(void);

/**
 * @name Receive primitives.
 * @{
 */

/**
 * @brief   Copy received bytes out of the DMA ring.
 *
 * @details Non-blocking: returns immediately with however many bytes
 *          are available (up to @ref max_len).  The DMA keeps filling
 *          the ring in the background; the caller drains at its own
 *          pace.  The ring is a single-producer (DMA) / single-
 *          consumer (this function) circular buffer, so no locking is
 *          needed.
 *
 *          On entry, any accumulated line errors (overrun, framing,
 *          noise, parity) are acknowledged so that a stray bad byte
 *          cannot wedge the RX path; the bad data itself is already in
 *          the ring if DMA copied it and the caller may choose to
 *          handle (or drop) it.
 *
 * @param[out] dst     Destination buffer.
 * @param[in]  max_len Maximum number of bytes to copy.
 * @return  Number of bytes actually copied (0..max_len).
 *
 * @srs     BGEN-010
 */
uint32_t boot_uart3_read(uint8_t *dst, uint32_t max_len);

/**
 * @brief   True while the DMA receiver is still armed and filling the
 *          ring.
 *
 * @details Useful for the diagnostics path and for testing; the
 *          protocol layer does not normally need to poll this.
 */
bool boot_uart3_rx_running(void);

/** @} */

/**
 * @brief   Send a single frame and wait for the hardware to finish
 *          clocking out the last stop bit.
 *
 * @details Uses the HAL's blocking transmit: each byte is written to
 *          the data register in sequence, and before returning the HAL
 *          waits for the TC (transmit-complete) flag, which is only set
 *          when the last stop bit has left the TX pin.  That final
 *          drain matters for the protocol: the host must not be expected
 *          to see our final byte until we are sure it is on the wire.
 *
 * @param[in] data         Frame bytes to send (non-NULL for len > 0).
 * @param[in] len          Number of bytes to send; 0 is a no-op that
 *                         returns `true` ("send nothing, no error").
 * @param[in] timeout_ms   Per-frame timeout in milliseconds; 0 selects
 *                         the module default (200 ms).
 * @return  `true` on success (or for the zero-length no-op); `false`
 *          on timeout, NULL @ref data, or HAL failure.
 *
 * @srs     BGEN-004
 */
bool boot_uart3_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/**
 * @brief   Tear down USART3 and release its pins and clocks (HND-001).
 *
 * @details Stops the DMA receiver and de-inits the UART.  The
 *          de-init path (MSP de-init in this file) then disables the
 *          stream IRQ, tears down the DMA handle, de-inits the PD8/PD9
 *          pins (back to reset), and gates the UART and DMA clocks off.
 *          After this call no byte can be sent or received - which is
 *          the point: the application now owns PD8/PD9.
 *
 * @srs     HND-001
 */
void boot_uart3_deinit(void);

#endif /* UART3_H */
