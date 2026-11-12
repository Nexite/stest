/* sensor board: main.h (CubeMX-style). */
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32l4xx_hal.h"

void Error_Handler(void);

/* Pin labels, as CubeMX generates them from the pinout. */
#define LED_Pin GPIO_PIN_5
#define LED_GPIO_Port GPIOA
#define BUTTON_Pin GPIO_PIN_13
#define BUTTON_GPIO_Port GPIOC
#define BUTTON_EXTI_IRQn EXTI15_10_IRQn
#define SENSOR_IN_Pin GPIO_PIN_0
#define SENSOR_IN_GPIO_Port GPIOC

#ifdef __cplusplus
}
#endif
#endif /* __MAIN_H */
