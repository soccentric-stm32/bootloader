/**
 * @file    boot_board.h
 * @brief   NUCLEO-F767ZI pins used by the bootloader.
 * @details The only place BOOT names a GPIO port or pin.
 *
 * | LED  | Colour | Meaning                              |
 * |------|--------|--------------------------------------|
 * | LD1  | green  | heartbeat / activity                 |
 * | LD2  | blue   | bootloader is running                |
 * | LD3  | red    | no valid image found, or fault       |
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6)
 * @srs     BGEN-006, BGEN-008, ENT-003; pin assignments per Hardware.md section 9
 */

#ifndef BOOT_BOARD_H
#define BOOT_BOARD_H

#include "stm32f7xx_hal.h"

/**
 * @name LD1 (green user LED, active high)
 * @{
 */
#define BOOT_LED1_PORT          GPIOB      /**< LD1 port. */
#define BOOT_LED1_PIN           GPIO_PIN_0 /**< LD1 pin. */
/** @} */

/**
 * @name LD2 (blue user LED, active high)
 * @brief On while BOOT is running.
 * @{
 */
#define BOOT_LED2_PORT          GPIOB      /**< LD2 port. */
#define BOOT_LED2_PIN           GPIO_PIN_7 /**< LD2 pin. */
/** @} */

/**
 * @name LD3 (red user LED, active high)
 * @brief On when no valid image can be launched, or on a fault.
 * @{
 */
#define BOOT_LED3_PORT          GPIOB       /**< LD3 port. */
#define BOOT_LED3_PIN           GPIO_PIN_14 /**< LD3 pin. */
/** @} */

/**
 * @name B1 (user button)
 * @brief Reads high while pressed.
 * @{
 */
#define BOOT_BTN1_PORT          GPIOC       /**< B1 port. */
#define BOOT_BTN1_PIN           GPIO_PIN_13 /**< B1 pin. */
/** @} */

#endif /* BOOT_BOARD_H */
