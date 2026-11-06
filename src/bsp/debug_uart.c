/**
 * @file    debug_uart.c
 * @brief   Text debug output on USART6 (PG14, Arduino D1), TX only.
 *
 * @details
 * - USART6 sits on APB2; BRR is computed from the running PCLK2, so the
 *   baud rate is right whatever clock tree @ref boot_clock_init chose.
 * - Every wait is bounded (@ref DBG_TX_WAIT_LOOPS): a debug port must
 *   never hang the boot.  At 115200 baud a character takes about 87 us,
 *   so the boot banner costs a few tens of milliseconds.
 * - The hand-over calls @ref boot_dbg_deinit, which drains the last
 *   character and puts USART6 and PG14 back to their reset state.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-06
 * @target  STM32F767ZIT6
 * @srs     BDBG-001, HND-001
 */

#include "debug_uart.h"

#if BOOT_DEBUG_UART

#include "stm32f7xx_hal.h"

/** Debug baud rate, shared by every firmware in the family. */
#define DBG_BAUD            115200U
/** PG14 = USART6_TX. */
#define DBG_TX_PIN          14U
/** Alternate function 8 = USART6 on PG14. */
#define DBG_TX_AF           8U
/** Upper bound for one TXE/TC wait (about 1 ms at 216 MHz). */
#define DBG_TX_WAIT_LOOPS   200000U

static void dbg_wait(uint32_t flag)
{
    for (uint32_t i = 0U; i < DBG_TX_WAIT_LOOPS; i++) {
        if ((USART6->ISR & flag) != 0U) {
            return;
        }
    }
}

static void dbg_putc(char c)
{
    dbg_wait(USART_ISR_TXE);
    USART6->TDR = (uint8_t)c;
}

void boot_dbg_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOGEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART6EN;
    (void)RCC->APB2ENR;   /* clock enable takes effect before the access */

    const uint32_t shift = DBG_TX_PIN * 2U;
    GPIOG->MODER = (GPIOG->MODER & ~(3U << shift)) | (2U << shift);
    GPIOG->OSPEEDR = (GPIOG->OSPEEDR & ~(3U << shift)) | (1U << shift);
    const uint32_t af_shift = (DBG_TX_PIN - 8U) * 4U;
    GPIOG->AFR[1] = (GPIOG->AFR[1] & ~(0xFU << af_shift)) |
                    (DBG_TX_AF << af_shift);

    USART6->CR1 = 0U;
    USART6->BRR = (HAL_RCC_GetPCLK2Freq() + (DBG_BAUD / 2U)) / DBG_BAUD;
    USART6->CR1 = USART_CR1_TE | USART_CR1_UE;
}

void boot_dbg_str(const char *text)
{
    while (*text != '\0') {
        dbg_putc(*text);
        text++;
    }
}

void boot_dbg_dec(uint32_t value)
{
    char digits[10];
    uint32_t n = 0U;
    do {
        digits[n] = (char)('0' + (value % 10U));
        n++;
        value /= 10U;
    } while (value != 0U);
    while (n > 0U) {
        n--;
        dbg_putc(digits[n]);
    }
}

void boot_dbg_hex(uint32_t value)
{
    boot_dbg_str("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t nibble = (value >> (uint32_t)shift) & 0xFU;
        dbg_putc((char)(nibble < 10U ? '0' + nibble : 'a' + nibble - 10U));
    }
}

void boot_dbg_begin(char level)
{
    /* "[   1234] BOOT I " - the timestamp is right-aligned to 7 digits. */
    uint32_t ms = HAL_GetTick();
    dbg_putc('[');
    for (uint32_t limit = 1000000U; limit > 1U; limit /= 10U) {
        if (ms < limit) {
            dbg_putc(' ');
        }
    }
    boot_dbg_dec(ms);
    boot_dbg_str("] BOOT ");
    dbg_putc(level);
    dbg_putc(' ');
}

void boot_dbg_end(void)
{
    boot_dbg_str("\r\n");
}

void boot_dbg_deinit(void)
{
    dbg_wait(USART_ISR_TC);
    USART6->CR1 = 0U;
    RCC->APB2RSTR |= RCC_APB2RSTR_USART6RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_USART6RST;
    RCC->APB2ENR &= ~RCC_APB2ENR_USART6EN;

    const uint32_t shift = DBG_TX_PIN * 2U;
    GPIOG->MODER &= ~(3U << shift);
    GPIOG->OSPEEDR &= ~(3U << shift);
    GPIOG->AFR[1] &= ~(0xFU << ((DBG_TX_PIN - 8U) * 4U));
    RCC->AHB1ENR &= ~RCC_AHB1ENR_GPIOGEN;
}

#endif /* BOOT_DEBUG_UART */
