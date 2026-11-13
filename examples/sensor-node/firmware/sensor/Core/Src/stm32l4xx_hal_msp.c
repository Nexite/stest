/* sensor board: MSP initialization (CubeMX-style): clocks, pins and NVIC of each peripheral.
 *
 * stest is strict: touching a peripheral whose RCC clock is off aborts the simulation with a
 * message naming the register, so every __HAL_RCC_*_CLK_ENABLE below is required, as on the
 * chip. */
#include "main.h"

void HAL_MspInit(void)
{
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if (hadc->Instance == ADC1)
  {
    __HAL_RCC_ADC_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    /* PC0 -> ADC1_IN1. In a test, Python sets its voltage: sensor.pin("PC0").set_voltage(1.65) */
    GPIO_InitStruct.Pin = SENSOR_IN_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(SENSOR_IN_GPIO_Port, &GPIO_InitStruct);
  }
}

void HAL_CAN_MspInit(CAN_HandleTypeDef *hcan)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if (hcan->Instance == CAN1)
  {
    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    /* PA11 -> CAN1_RX, PA12 -> CAN1_TX. The test wires the controller to a simulated bus:
     * sim.node("sensor", exe, can={"CAN1": bus}) */
    GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF9_CAN1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
  }
}
