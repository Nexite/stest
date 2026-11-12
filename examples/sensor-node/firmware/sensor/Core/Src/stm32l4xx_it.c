/* sensor board: interrupt handlers (CubeMX-style).
 *
 * Under stest these run on the firmware thread when the simulated NVIC takes the interrupt,
 * exactly as on the chip: SysTick every 1 ms of virtual time, EXTI15_10 on a PC13 edge. */
#include "main.h"
#include "stm32l4xx_it.h"

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

void EXTI15_10_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(BUTTON_Pin);
}
