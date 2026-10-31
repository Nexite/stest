/* stest HAL-shim C API (MODELS <TYPE>=hal).
 *
 * A HAL shim (chips/<family>/hal/<family>xx_hal_<ppp>_shim.c, listed in the family's
 * family.cmake) replaces ST's <family>xx_hal_<ppp>.c at link time when a firmware is built
 * with stest_add_firmware(... MODELS <TYPE>=hal). The shim is compiled like the rest of the
 * HAL (firmware side, with the MMIO pass) and calls these functions instead of touching the
 * peripheral's registers. They are implemented by the chip family's models (runtime side,
 * uninstrumented C++), which keep the register block consistent (MODER/ODR/... read back
 * the same as in register mode, so Python and firmware observers see one state).
 *
 * Timing contract: each function accounts virtual time as the same register accesses ST's
 * code makes for that operation (stest::shim_access: same cost, clock events, quantum ends,
 * IRQ delivery points and poll-streak accounting as MMIO), so a firmware's event log is
 * byte-identical in register and hal mode. Errors abort strictly, like MMIO (unknown port,
 * peripheral clock disabled, ...).
 *
 * Bus accesses performed here never trigger the hal-mode "firmware accessed ... directly"
 * warning; only firmware MMIO to a hal-owned block does.
 */
#ifndef STEST_HAL_API_H
#define STEST_HAL_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- GPIO (MODELS GPIO=hal) ----
 * port: 'A'..; pins: GPIO_PIN_x mask. mode/pull/speed/alternate use ST's GPIO_InitTypeDef
 * encoding (mode bits [1:0] = MODER value, bit 4 = open-drain; EXTI bits are ignored here:
 * the shim configures SYSCFG/EXTI through their registers, which stay in register mode). */

/* HAL_GPIO_Init's GPIO part, per pin in ascending order: OSPEEDR/OTYPER (output and AF
 * modes), PUPDR (all but analog), AFRL/AFRH (AF), then MODER, each a read-modify-write. */
void     stest_gpio_hal_init(char port, uint16_t pins, uint32_t mode, uint32_t pull, uint32_t speed,
                             uint32_t alternate);
/* HAL_GPIO_DeInit's GPIO part, per pin: MODER analog, AFR 0, OSPEEDR 0, OTYPER 0, PUPDR 0. */
void     stest_gpio_hal_deinit(char port, uint16_t pins);
/* HAL_GPIO_WritePin: level != 0 sets the pins (BSRR), else resets them (BRR). */
void     stest_gpio_hal_write(char port, uint16_t pins, int level);
/* HAL_GPIO_ReadPin: 1 if any of `pin` reads high in IDR, else 0. */
int      stest_gpio_hal_read(char port, uint16_t pin);
/* HAL_GPIO_TogglePin: reads ODR, then sets the low pins and resets the high ones (BSRR). */
void     stest_gpio_hal_toggle(char port, uint16_t pins);
/* HAL_GPIO_LockPin's LCKR key sequence, one LCKR access per call (the lock is a register
 * protocol: write LCKK|pins, pins, LCKK|pins, then read twice). */
void     stest_gpio_hal_lckr_write(char port, uint32_t value);
uint32_t stest_gpio_hal_lckr_read(char port);

#ifdef __cplusplus
}
#endif
#endif
