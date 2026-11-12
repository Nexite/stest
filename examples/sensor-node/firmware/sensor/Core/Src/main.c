/* sensor board (STM32L496RG, CubeMX-style).
 *
 * - The user button on PC13 (pull-up, pressed = low) raises EXTI on its falling edge; each
 *   press toggles the LED on PA5 and counts.
 * - Every 100 ms the main loop converts ADC1 channel 1 (PC0) and sends the reading on CAN1
 *   (500 kbit/s) as SensorReading (standard id 0x120, see can/sensor-node.dbc):
 *     byte 0-1  millivolts    uint16, little-endian, 1 mV/bit
 *     byte 2    button_count  uint8
 *
 * Nothing here knows about stest: the same file builds for the real board. Under stest the
 * firmware runs on virtual time, so "every 100 ms" is exact and the tests are repeatable. */
#include "main.h"

ADC_HandleTypeDef hadc1;
CAN_HandleTypeDef hcan1;

/* Globals the tests read by name (stest finds them through the debug info):
 * sensor.var("g_sample_count"). */
uint32_t g_sample_count = 0;          /* ADC samples taken */
volatile uint8_t g_button_count = 0;  /* button presses (written in the EXTI callback) */
volatile uint32_t g_tx_dropped = 0;   /* readings not sent: no free TX mailbox */

#define SAMPLE_PERIOD_MS 100U
#define SENSOR_READING_ID 0x120U
#define VREF_MV 3300U   /* VDDA; stest's ADC model uses the same 3.3 V reference */
#define ADC_FULL_SCALE 4095U

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_CAN1_Init(void);
static uint16_t read_millivolts(void);
static void send_reading(uint16_t millivolts, uint8_t button_count);

int main(void)
{
  uint32_t next_sample = SAMPLE_PERIOD_MS;

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_CAN1_Init();

  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_CAN_Start(&hcan1) != HAL_OK)
  {
    Error_Handler();
  }

  while (1)
  {
    /* Sleep until the next interrupt (SysTick every 1 ms, or the button). Under stest a
     * sleeping core costs no host time: the simulation skips to the next event. */
    __WFI();
    if (HAL_GetTick() >= next_sample)
    {
      next_sample += SAMPLE_PERIOD_MS;
      uint16_t mv = read_millivolts();
      g_sample_count++;
      send_reading(mv, g_button_count);
    }
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == BUTTON_Pin)
  {
    g_button_count++;
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
  }
}

/* One single conversion of PC0, in millivolts (rounded). */
static uint16_t read_millivolts(void)
{
  uint32_t raw = 0;
  if (HAL_ADC_Start(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_ADC_PollForConversion(&hadc1, 2) == HAL_OK)
  {
    raw = HAL_ADC_GetValue(&hadc1);
  }
  return (uint16_t)((raw * VREF_MV + ADC_FULL_SCALE / 2U) / ADC_FULL_SCALE);
}

static void send_reading(uint16_t millivolts, uint8_t button_count)
{
  CAN_TxHeaderTypeDef header = {0};
  uint8_t data[3];
  uint32_t mailbox;

  data[0] = (uint8_t)(millivolts & 0xFFU);
  data[1] = (uint8_t)(millivolts >> 8);
  data[2] = button_count;

  header.StdId = SENSOR_READING_ID;
  header.IDE = CAN_ID_STD;
  header.RTR = CAN_RTR_DATA;
  header.DLC = sizeof(data);
  header.TransmitGlobalTime = DISABLE;
  if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0U ||
      HAL_CAN_AddTxMessage(&hcan1, &header, data, &mailbox) != HAL_OK)
  {
    g_tx_dropped++;
  }
}

/* 80 MHz from an 8 MHz HSE bypass clock through the PLL; SYSCLK = HCLK = PCLK1 = PCLK2. */
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

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /* Button: pressed pulls the pin low, so interrupt on the falling edge. In a test:
   * sensor.pin("PC13").set_low() presses it, set_high() releases it. */
  GPIO_InitStruct.Pin = BUTTON_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BUTTON_GPIO_Port, &GPIO_InitStruct);

  HAL_NVIC_SetPriority(BUTTON_EXTI_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(BUTTON_EXTI_IRQn);
}

static void MX_ADC1_Init(void)
{
  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
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

  sConfig.Channel = ADC_CHANNEL_1;   /* PC0 */
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_47CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/* 500 kbit/s from PCLK1 = 80 MHz: 80 MHz / (Prescaler 10 x (1 + 13 + 2) tq) = 500 000 bit/s.
 * stest checks this against the bus: a node whose bit rate differs from its bus's stops the
 * test with an error naming both rates. */
static void MX_CAN1_Init(void)
{
  hcan1.Instance = CAN1;
  hcan1.Init.Prescaler = 10;
  hcan1.Init.Mode = CAN_MODE_NORMAL;
  hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan1.Init.TimeSeg1 = CAN_BS1_13TQ;
  hcan1.Init.TimeSeg2 = CAN_BS2_2TQ;
  hcan1.Init.TimeTriggeredMode = DISABLE;
  hcan1.Init.AutoBusOff = ENABLE;
  hcan1.Init.AutoWakeUp = DISABLE;
  hcan1.Init.AutoRetransmission = ENABLE;
  hcan1.Init.ReceiveFifoLocked = DISABLE;
  hcan1.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan1) != HAL_OK)
  {
    Error_Handler();
  }
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
