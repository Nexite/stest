/* adc: CubeMX-style ADC1 single conversions of channel 16 (PB1) every 1 ms, 80 MHz from the
 * PLL (blinky's scaffolding). Calibrates once, then each tick: HAL_ADC_Start,
 * HAL_ADC_PollForConversion(.., 2) and HAL_ADC_GetValue into last_raw (conv_count counts
 * completed conversions, error_count HAL failures).
 *
 * Variants (ADC_VARIANT):
 *   ADC_SYNC (default)  ADC_CLOCK_SYNC_PCLK_DIV4 (HCLK/4 = 20 MHz)
 *   ADC_ASYNC           ADC_CLOCK_ASYNC_DIV1 + __HAL_RCC_ADC_CONFIG(RCC_ADCCLKSOURCE_SYSCLK)
 *   ADC_ASYNC_NOCLK     ADC_CLOCK_ASYNC_DIV1 without selecting an ADC kernel clock
 *   ADC_SCAN            scan mode, two ranks (channels 16 and 15) */
#include "main.h"

#define ADC_SYNC 0
#define ADC_ASYNC 1
#define ADC_ASYNC_NOCLK 2
#define ADC_SCAN 3
#ifndef ADC_VARIANT
#define ADC_VARIANT ADC_SYNC
#endif

ADC_HandleTypeDef hadc1;
volatile uint32_t last_raw = 0xFFFFFFFFu;
volatile uint32_t conv_count = 0;
volatile uint32_t error_count = 0;
volatile uint32_t ready = 0;

void SystemClock_Config(void);
static void MX_ADC1_Init(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_ADC1_Init();
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  ready = 1;
  while (1)
  {
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
      Error_Handler();
    }
    if (HAL_ADC_PollForConversion(&hadc1, 2) == HAL_OK)
    {
      last_raw = HAL_ADC_GetValue(&hadc1);
      conv_count++;
    }
    else
    {
      error_count++;
    }
    uint32_t t = HAL_GetTick();
    while (HAL_GetTick() == t)
    {
      __WFI();
    }
  }
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if (hadc->Instance == ADC1)
  {
#if ADC_VARIANT == ADC_ASYNC
    __HAL_RCC_ADC_CONFIG(RCC_ADCCLKSOURCE_SYSCLK);
#endif
    __HAL_RCC_ADC_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* PB1 -> ADC1_IN16 */
    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  }
}

static void MX_ADC1_Init(void)
{
  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
#if ADC_VARIANT == ADC_ASYNC || ADC_VARIANT == ADC_ASYNC_NOCLK
  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
#else
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
#endif
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
#if ADC_VARIANT == ADC_SCAN
  hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
  hadc1.Init.NbrOfConversion = 2;
#else
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.NbrOfConversion = 1;
#endif
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = ADC_CHANNEL_16;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
#if ADC_VARIANT == ADC_SCAN
  sConfig.Channel = ADC_CHANNEL_15;
  sConfig.Rank = ADC_REGULAR_RANK_2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
#endif
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
  error_count++;
  while (1)
  {
    __WFI();
  }
}
