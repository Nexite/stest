/* display board: interrupt handlers (CubeMX-style).
 *
 * Under stest these run on the firmware thread when the simulated NVIC takes the interrupt,
 * exactly as on the chip: CAN1_RX0 when a received frame lands in FIFO0. */
#include "main.h"
#include "stm32l4xx_it.h"

extern CAN_HandleTypeDef hcan1;

void NMI_Handler(void)
{
  while (1)
  {
  }
}

void HardFault_Handler(void)
{
  while (1)
  {
  }
}

void MemManage_Handler(void)
{
  while (1)
  {
  }
}

void BusFault_Handler(void)
{
  while (1)
  {
  }
}

void UsageFault_Handler(void)
{
  while (1)
  {
  }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void CAN1_RX0_IRQHandler(void)
{
  HAL_CAN_IRQHandler(&hcan1);
}
