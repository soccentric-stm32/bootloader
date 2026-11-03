/*
 * board/boot_board.h - NUCLEO-F767ZI pins used by the bootloader
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI (STM32F767ZIT6)
 * SRS:     BGEN-006, BGEN-008, ENT-003; pins from Hardware.md section 9
 */

#ifndef BOOT_BOARD_H
#define BOOT_BOARD_H

#include "stm32f7xx_hal.h"

#define BOOT_LED1_PORT          GPIOB      /* LD1 green, high = on         */
#define BOOT_LED1_PIN           GPIO_PIN_0
#define BOOT_LED2_PORT          GPIOB      /* LD2 blue: BOOT running       */
#define BOOT_LED2_PIN           GPIO_PIN_7
#define BOOT_LED3_PORT          GPIOB      /* LD3 red: no image / fault    */
#define BOOT_LED3_PIN           GPIO_PIN_14
#define BOOT_BTN1_PORT          GPIOC      /* B1, high when pressed        */
#define BOOT_BTN1_PIN           GPIO_PIN_13

#endif /* BOOT_BOARD_H */
