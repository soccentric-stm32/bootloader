/*
 * uart3.h - USART3 on the ST-LINK VCP
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI (PD8 TX, PD9 RX)
 * SRS:     BGEN-004, BGEN-010, ENT-001, HND-001
 */

#ifndef UART3_H
#define UART3_H

#include <stdbool.h>
#include <stdint.h>

#define BOOT_UART_DEFAULT_BAUD  115200U
#define BOOT_UART_FAST_BAUD     921600U
#define BOOT_UART_RX_RING_SIZE  4096U

/* USART3 8N1, no flow control, 115200 baud, circular DMA receive into a
 * 4 KB ring.  The DMA keeps filling the ring while flash is programmed. */
bool boot_uart3_init(void);

/* Change between the two rates.  The caller sends the Ack first. */
bool boot_uart3_set_baud(uint32_t baud);
uint32_t boot_uart3_baud(void);

/* Copy received bytes out of the DMA ring. */
uint32_t boot_uart3_read(uint8_t *dst, uint32_t max_len);

/* True while the DMA receiver is armed. */
bool boot_uart3_rx_running(void);

/* Send one frame and wait for the last stop bit. */
bool boot_uart3_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/* HND-001: stop the DMA and USART3, release the pins and clocks. */
void boot_uart3_deinit(void);

#endif /* UART3_H */
