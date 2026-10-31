/* itm: CMSIS ITM_SendChar logging and DWT CYCCNT. 80 MHz from the PLL (blinky's scaffolding).
 *  - cycles_10ms: CYCCNT delta across HAL_Delay(9) (ST's HAL adds one tick: 10 ms), measured
 *    right after a SysTick boundary; enabled the way firmware does (DEMCR.TRCENA, CYCCNTENA).
 *  - then logs "hello <i>\n" on ITM port 0 every 100 ms with plain CMSIS ITM_SendChar, which
 *    drops the output when no debugger enabled the ITM (Sim(debugger_attached=False)).
 *  - loops counts iterations of the logging loop (it must advance with or without a debugger).
 * Env STEST_ITM_PARTIAL=1: after the first line, sends "partial" without a newline and
 * returns from main (strict abort), to test flushing of unterminated ITM text. */
#include <stdio.h>
#include <stdlib.h>
#include "main.h"

volatile uint32_t cycles_10ms = 0;
volatile uint32_t loops = 0;

void SystemClock_Config(void);

static void itm_puts(const char *s)
{
  while (*s)
  {
    ITM_SendChar((uint32_t)(unsigned char)*s++);
  }
}

int main(void)
{
  char buf[32];
  uint32_t i = 0;

  HAL_Init();
  SystemClock_Config();

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  uint32_t t = HAL_GetTick();
  while (HAL_GetTick() == t)
  {
    __WFI();
  }
  uint32_t start = DWT->CYCCNT;
  HAL_Delay(9);
  cycles_10ms = DWT->CYCCNT - start;

  while (1)
  {
    snprintf(buf, sizeof buf, "hello %lu\n", (unsigned long)i++);
    itm_puts(buf);
    loops++;
    if (getenv("STEST_ITM_PARTIAL"))
    {
      itm_puts("partial");
      return 1;
    }
    HAL_Delay(99);
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 40;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

void Error_Handler(void)
{
  while (1)
  {
    __WFI();
  }
}
