/**
 * @file    boot_board.h
 * @brief   NUCLEO-F767ZI peripheral pins used by the bootloader.
 *
 * @details
 * Central definition of every physical pin the bootloader touches on the
 * NUCLEO-F767ZI evaluation board (STM32F767ZIT6, LQFP144 packaging).  Keeping
 * the pin map in one place means hardware changes (board rev, port moves)
 * only require editing this file; the BSP (`src/bsp/`) never hard-codes a
 * GPIO port or pin number.
 *
 * The three user LEDs carry the bootloader's visual state protocol:
 *
 * | LED  | Colour | Meaning                              |
 * |------|--------|--------------------------------------|
 * | LD1  | green  | heartbeat / activity                 |
 * | LD2  | blue   | bootloader is running                |
 * | LD3  | red    | no valid image found, or fault       |
 *
 * Button B1 is polled by the app selector to let the operator force an
 * update or select an alternate image slot.
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
 * @name LED1 (LD1) pin definition (green user LED).
 * @brief GPIO port and pin numbers for the green user LED LD1.
 * @{
 */
/** GPIO port of LED1 (LD1, green). Driven HIGH to switch the LED on. */
#define BOOT_LED1_PORT          GPIOB

/** Pin number of LED1 (LD1, green) on @ref BOOT_LED1_PORT. */
#define BOOT_LED1_PIN           GPIO_PIN_0
/** @} */

/**
 * @name LED2 (LD2) pin definition (blue user LED).
 * @brief GPIO port and pin numbers for the blue user LED LD2.
 *
 * @details Latched ON while the bootloader is actively running (image
 * selection, update download, verification).
 * @{
 */
/** GPIO port of LED2 (LD2, blue) - "BOOT running" indicator. */
#define BOOT_LED2_PORT          GPIOB

/** Pin number of LED2 (LD2, blue) on @ref BOOT_LED2_PORT. */
#define BOOT_LED2_PIN           GPIO_PIN_7
/** @} */

/**
 * @name LED3 (LD3) pin definition (red user LED).
 * @brief GPIO port and pin numbers for the red user LED LD3.
 *
 * @details Latched ON when no valid image can be launched or a fatal
 * fault condition is present.
 * @{
 */
/** GPIO port of LED3 (LD3, red) - "no image / fault" indicator. */
#define BOOT_LED3_PORT          GPIOB

/** Pin number of LED3 (LD3, red) on @ref BOOT_LED3_PORT. */
#define BOOT_LED3_PIN           GPIO_PIN_14
/** @} */

/**
 * @name BTN1 (B1) pin definition (user button).
 * @brief GPIO port and pin numbers for the user button B1.
 *
 * @details Connected to the 3.3 V rail through the NUCLEO button wiring,
 * so the pin reads HIGH while the button is physically pressed and floats
 * to LOW (weak pull-down) when released.  No filtering or debounce
 * library is assumed; the BSP toggles the button in slow loops.
 * @{
 */
/** GPIO port of BTN1 (B1). Pin reads HIGH while the button is pressed. */
#define BOOT_BTN1_PORT          GPIOC

/** Pin number of BTN1 (B1) on @ref BOOT_BTN1_PORT. */
#define BOOT_BTN1_PIN           GPIO_PIN_13
/** @} */

#endif /* BOOT_BOARD_H */
