/* blinky: main.h (CubeMX-style). */
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32l4xx_hal.h"

void Error_Handler(void);

#define LED_Pin GPIO_PIN_5
#define LED_GPIO_Port GPIOA

#ifdef __cplusplus
}
#endif
#endif /* __MAIN_H */
