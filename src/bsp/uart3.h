/**
 * @file    uart3.h
 * @brief   USART3 on the ST-LINK virtual COM port (PD8 TX, PD9 RX).
 * @details Raw byte stream for HWLINK: a 4 KB DMA receive ring that keeps
 *          filling during flash programming (BGEN-010), a 115200/921600 baud
 *          switch, and a blocking send that drains the last stop bit.
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
 * @name UART baud rates
 * @{
 */

/** @brief Default rate, 8N1. */
#define BOOT_UART_DEFAULT_BAUD  115200U

/**
 * @brief Fast transfer rate, 8N1.
 * @details Within 0.01 % of nominal from the 54 MHz PCLK1 (BGEN-004).
 */
#define BOOT_UART_FAST_BAUD     921600U

/** @} */

/**
 * @name Receive ring
 * @{
 */

/**
 * @brief Receive ring size in bytes.
 * @details Absorbs host data during a flash erase or program.  The ring is
 *          in the 32-byte aligned `.dma_buffer` section.
 */
#define BOOT_UART_RX_RING_SIZE  4096U

/** @} */

/**
 * @brief   Start USART3 at 115200 8N1 and arm the circular DMA receiver.
 * @details The HAL MSP callback configures PD8/PD9 as AF7 with pull-up and
 *          DMA1 Stream 1 channel 4 in circular mode.  Transmit is polled.
 *
 * @return  true when the UART is ready and the DMA is armed.
 *
 * @srs     BGEN-004, BGEN-010
 */
bool boot_uart3_init(void);

/**
 * @brief   Switch the baud rate.
 * @details Stops DMA, re-initialises the UART and re-arms the receiver,
 *          discarding unread ring data.  On failure the old rate is
 *          restored on a best-effort basis.
 *
 * @warning Send the Ack to the host first; bytes in flight at the old rate
 *          are misframed.
 *
 * @param[in] baud  BOOT_UART_DEFAULT_BAUD or BOOT_UART_FAST_BAUD.
 * @return  true on success or when already at @p baud; false for an
 *          unsupported rate or a failed re-init.
 *
 * @srs     BGEN-004
 */
bool boot_uart3_set_baud(uint32_t baud);

/**
 * @brief   Return the current baud rate.
 * @return  BOOT_UART_DEFAULT_BAUD or BOOT_UART_FAST_BAUD.
 */
uint32_t boot_uart3_baud(void);

/**
 * @name Receive primitives
 * @{
 */

/**
 * @brief   Copy received bytes out of the DMA ring.
 * @details Non-blocking.  Clears ORE/FE/NE/PE first so a bad byte cannot
 *          stall reception.  Single consumer; not reentrant.  Returns at
 *          most the bytes up to the ring wrap, so call again for the rest.
 *
 * @param[out] dst     Destination buffer.
 * @param[in]  max_len Capacity of @p dst in bytes.
 * @return  Bytes copied, 0..max_len.
 *
 * @srs     BGEN-010
 */
uint32_t boot_uart3_read(uint8_t *dst, uint32_t max_len);

/**
 * @brief   Report whether the DMA receiver is armed.
 * @return  true when the DMA stream is busy and the UART receiver enabled.
 */
bool boot_uart3_rx_running(void);

/** @} */

/**
 * @brief   Send bytes and wait until the last stop bit has left the pin.
 * @details Blocking HAL transmit; waits for TC.
 *
 * @param[in] data        Bytes to send; NULL is a no-op.
 * @param[in] len         Byte count; 0 is a no-op.
 * @param[in] timeout_ms  Timeout in ms; 0 selects 200 ms.
 * @return  true on success or no-op; false on timeout or HAL error.
 *
 * @srs     BGEN-004
 */
bool boot_uart3_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/**
 * @brief   Stop USART3 and release its pins, DMA and clocks (HND-001).
 * @details PD8/PD9 return to reset state for the application.  This also
 *          gates the DMA1 clock off.
 *
 * @srs     HND-001
 */
void boot_uart3_deinit(void);

#endif /* UART3_H */
