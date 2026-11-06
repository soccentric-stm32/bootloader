/**
 * @file    uart3.c
 * @brief   USART3 on the ST-LINK VCP.
 *
 * @details
 * - **Receive**: DMA1 Stream 1 (channel 4) in *circular* mode into the
 *   4 KB ring.  Circular mode is what lets the ring keep filling while
 *   the CPU is stalled in a flash program/erase (BGEN-010, UPD-003) -
 *   there is no "single-shot DMA transfer" that needs re-arming.
 * - **Transmit**: polled through `HAL_UART_Transmit`; a reply is at
 *   most a few hundred bytes and we do not want the extra state the DMA
 *   would bring to a send path we barely use.
 * - **Baud switching**: full deinit + re-init pair (see
 *   @ref boot_uart3_set_baud) with best-effort rollback on failure.
 * - **Ring consumption**: the read pointer is private to this file and
 *   advanced only by @ref boot_uart3_read, so a single consumer is all
 *   we need.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (PD8 TX, PD9 RX)
 * @srs     BGEN-004 (115200 / 921600, 8N1), BGEN-010 (4 KB ring, no
 *          byte lost while flash is programmed), HND-001
 */

#include <string.h>
#include "stm32f7xx_hal.h"
#include "uart3.h"

/** @brief Default per-frame transmit timeout (ms), used when the caller
 *         passes 0 to @ref boot_uart3_send. */
#define UART_TX_TIMEOUT_MS 200U

/**
 * @name Module-private state.
 * @{
 */

/** HAL handle for USART3 (owned by this file). */
static UART_HandleTypeDef huart3;

/** HAL handle for the receive DMA stream (DMA1 Stream 1, channel 4). */
static DMA_HandleTypeDef hdma_usart3_rx;

/**
 * @brief Receive ring itself.  Placed in a dedicated DMA section and
 *        32-byte aligned.
 */
static uint8_t uart_rx_ring[BOOT_UART_RX_RING_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32)));

/** Read (consumer) pointer into the ring; advanced only by
 *    @ref boot_uart3_read. */
static uint32_t uart_rx_read;

/** Currently-active baud rate (one of the two public constants). */
static uint32_t uart_baud = BOOT_UART_DEFAULT_BAUD;

/** @} */

/**
 * @brief   Configure PD8 (TX, AF7) and PD9 (RX, AF7) as USART3 pins.
 *
 * @details Both pins are AF push-pull with a pull-up (RX keeps a
 *          defined state when no VCP is attached) and the
 *          very-high-speed drive setting; the alternate function
 *          selector is AF7 (USART3).
 */
static void uart_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    gpio.Pin       = GPIO_PIN_8 | GPIO_PIN_9;   /* PD8 TX, PD9 RX */
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOD, &gpio);
}

/**
 * @brief   Configure DMA1 Stream 1 for USART3 RX (peripheral->memory,
 *          circular, byte-width, very-high priority), enable the stream
 *          NVIC line, and link the handle to the UART handle.
 */
static void uart_dma_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_usart3_rx.Instance                 = DMA1_Stream1;
    hdma_usart3_rx.Init.Channel             = DMA_CHANNEL_4;
    hdma_usart3_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_usart3_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_usart3_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_usart3_rx.Init.Mode                = DMA_CIRCULAR;
    hdma_usart3_rx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
    hdma_usart3_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    hdma_usart3_rx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_1QUARTERFULL;
    hdma_usart3_rx.Init.MemBurst            = DMA_MBURST_SINGLE;
    hdma_usart3_rx.Init.PeriphBurst         = DMA_PBURST_SINGLE;
    (void)HAL_DMA_Init(&hdma_usart3_rx);

    __HAL_LINKDMA(&huart3, hdmarx, hdma_usart3_rx);

    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 0U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
}

/**
 * @brief   HAL callback: bring USART3's low-level resources up (clock,
 *          GPIO, DMA).  Called from `HAL_UART_Init` only for USART3.
 *
 * @see     uart_gpio_init, uart_dma_init (both in this file).
 */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART3) {
        return;
    }
    __HAL_RCC_USART3_CLK_ENABLE();
    uart_gpio_init();
    uart_dma_init();
}

/**
 * @brief   HAL callback: release USART3's low-level resources (stream
 *          IRQ, DMA handle, GPIO, UART and DMA clocks).
 *
 * @details Called from `HAL_UART_DeInit` on the USART3 path (see
 *          @ref boot_uart3_set_baud and @ref boot_uart3_deinit).
 */
void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART3) {
        return;
    }
    HAL_NVIC_DisableIRQ(DMA1_Stream1_IRQn);
    (void)HAL_DMA_DeInit(&hdma_usart3_rx);
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_8 | GPIO_PIN_9);
    __HAL_RCC_USART3_CLK_DISABLE();
    __HAL_RCC_DMA1_CLK_DISABLE();
}

/**
 * @brief   (Re)start the circular DMA receive over the whole ring,
 *          resetting the read pointer to 0.
 *
 * @return  `true` on success.
 */
static bool uart_start_receiver(void)
{
    uart_rx_read = 0U;
    if (HAL_UART_Receive_DMA(&huart3, uart_rx_ring,
                              BOOT_UART_RX_RING_SIZE) != HAL_OK) {
        return false;
    }
    return true;
}

/**
 * @brief   See @ref boot_uart3_init.
 *
 * @details 8 data bits, 1 stop bit, no parity, TX+RX mode, no HW flow
 *          control, 16x oversampling (the F7's robust default, which
 *          keeps the baud error tiny at both 115200 and 921600).
 *          A failed call leaves the UART in a defined state; retry is
 *          safe.
 */
bool boot_uart3_init(void)
{
    memset(&huart3, 0, sizeof(huart3));
    huart3.Instance          = USART3;
    huart3.Init.BaudRate     = BOOT_UART_DEFAULT_BAUD;
    huart3.Init.WordLength   = UART_WORDLENGTH_8B;
    huart3.Init.StopBits     = UART_STOPBITS_1;
    huart3.Init.Parity       = UART_PARITY_NONE;
    huart3.Init.Mode         = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart3) != HAL_OK) {
        return false;
    }
    uart_baud = BOOT_UART_DEFAULT_BAUD;
    return uart_start_receiver();
}

/**
 * @brief   See @ref boot_uart3_set_baud.
 *
 * @details Rollback: if the re-init fails, the UART is de-inited, re-
 *          initialised at the previous baud (best effort) and the
 *          receiver re-armed.
 */
bool boot_uart3_set_baud(uint32_t baud)
{
    if (baud != BOOT_UART_DEFAULT_BAUD && baud != BOOT_UART_FAST_BAUD) {
        return false;
    }
    if (baud == uart_baud) {
        return true;
    }

    uint32_t old_baud = uart_baud;
    (void)HAL_UART_DMAStop(&huart3);
    (void)HAL_UART_DeInit(&huart3);

    huart3.Init.BaudRate = baud;
    bool ok = HAL_UART_Init(&huart3) == HAL_OK && uart_start_receiver();
    if (!ok) {
        (void)HAL_UART_DeInit(&huart3);
        huart3.Init.BaudRate = old_baud;
        if (HAL_UART_Init(&huart3) == HAL_OK) {
            (void)uart_start_receiver();
        }
        return false;
    }

    uart_baud = baud;
    return true;
}

/** @brief See @ref boot_uart3_baud. */
uint32_t boot_uart3_baud(void)
{
    return uart_baud;
}

/**
 * @brief   See @ref boot_uart3_read.  The head pointer is derived from
 *          the DMA's remaining counter (bytes the DMA still has to
 *          write), which is always correct for a circular ring that is
 *          not re-armed mid-read.  Line errors (ORE/FE/NE/PE) are
 *          acknowledged on entry so a stray bad byte cannot wedge RX.
 */
uint32_t boot_uart3_read(uint8_t *dst, uint32_t max_len)
{
    /* Clear a line error without touching the DMA stream. */
    if ((USART3->ISR & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE |
                        USART_ISR_PE)) != 0U) {
        USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF |
                      USART_ICR_NCF | USART_ICR_PECF;
    }

    uint32_t head = BOOT_UART_RX_RING_SIZE -
                    __HAL_DMA_GET_COUNTER(&hdma_usart3_rx);
    head %= BOOT_UART_RX_RING_SIZE;

    if (head == uart_rx_read || max_len == 0U) {
        return 0U;
    }

    uint32_t count = (head > uart_rx_read)
                         ? (head - uart_rx_read)
                         : (BOOT_UART_RX_RING_SIZE - uart_rx_read);
    if (count > max_len) {
        count = max_len;
    }
    for (uint32_t i = 0U; i < count; i++) {
        dst[i] = uart_rx_ring[(uart_rx_read + i) % BOOT_UART_RX_RING_SIZE];
    }
    uart_rx_read = (uart_rx_read + count) % BOOT_UART_RX_RING_SIZE;
    return count;
}

/**
 * @brief   See @ref boot_uart3_rx_running.  Reads the DMA stream state
 *          and the UART RE bit; either one clear means "not running".
 */
bool boot_uart3_rx_running(void)
{
    return hdma_usart3_rx.State == HAL_DMA_STATE_BUSY &&
           (USART3->CR1 & USART_CR1_RE) != 0U;
}

/**
 * @brief   See @ref boot_uart3_send.  Delegates to the HAL's blocking
 *          transmit, which polls byte-by-byte and waits for TC (last
 *          stop bit clocked out) before returning.
 */
bool boot_uart3_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    if (len == 0U || data == NULL) {
        return true;
    }
    if (timeout_ms == 0U) {
        timeout_ms = UART_TX_TIMEOUT_MS;
    }
    return HAL_UART_Transmit(&huart3, (uint8_t *)data, len, timeout_ms) ==
           HAL_OK;
}

/**
 * @brief   See @ref boot_uart3_deinit.  Stops the DMA receiver and de-
 *          inits the UART; the de-init path (MSP de-init in this file)
 *          releases the pins and clocks.
 */
void boot_uart3_deinit(void)
{
    (void)HAL_UART_DMAStop(&huart3);
    (void)HAL_UART_DeInit(&huart3);
}

/**
 * @brief   DMA1 Stream 1 interrupt handler routed to the USART3 RX
 *          stream (referenced from the vector table; do not redefine
 *          it elsewhere in the image).
 */
void DMA1_Stream1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart3_rx);
}
