/* Firmware-side half of runtime_cortexm_cmsis_e2e: compiled with the MMIO pass and
 * the host CMSIS shim ahead of CMSIS-Core 5.9.0's core_cm4.h. Returns 0 or the
 * number of the first failed check. */
#include "fw_device.h"

uint32_t SystemCoreClock = 80000000u;
static volatile uint32_t ticks, tim_hits, pendsv_hits;
static volatile uint32_t ipsr_in_tim;

void SysTick_Handler(void) { ticks++; }
void PendSV_Handler(void) { pendsv_hits++; }
void TIM2_IRQHandler(void) {
  tim_hits++;
  ipsr_in_tim = __get_IPSR();
}

int fw_run(void) {
  if (SysTick_Config(SystemCoreClock / 1000u)) return 1;
  if (SysTick->LOAD != 79999u) return 2;
  if (NVIC_GetPriority(SysTick_IRQn) != 15u) return 3;  /* SysTick_Config sets lowest */

  NVIC_SetPriorityGrouping(3u);
  if (NVIC_GetPriorityGrouping() != 3u) return 4;
  NVIC_SetPriority(TIM2_IRQn, NVIC_EncodePriority(3u, 5u, 0u));
  if (NVIC_GetPriority(TIM2_IRQn) != 5u) return 5;

  __disable_irq();
  if (__get_PRIMASK() != 1u) return 6;
  NVIC_EnableIRQ(TIM2_IRQn);
  NVIC_SetPendingIRQ(TIM2_IRQn);
  if (tim_hits != 0u) return 7;
  if (!NVIC_GetPendingIRQ(TIM2_IRQn)) return 8;
  __enable_irq();
  if (tim_hits != 1u) return 9;
  if (ipsr_in_tim != 16u + 28u) return 10;
  if (__get_IPSR() != 0u) return 11;

  __set_BASEPRI(5u << 4);
  NVIC_SetPendingIRQ(TIM2_IRQn);
  if (tim_hits != 1u) return 12;
  __set_BASEPRI(0u);
  if (tim_hits != 2u) return 13;

  SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
  if (pendsv_hits != 1u) return 14;

  while (ticks < 10u) __WFI();

  SCB->CPACR |= (0xFu << 20);
  if (SCB->CPACR != (0xFu << 20)) return 15;
  if ((SCB->CPUID & 0xFFF0u) != 0xC240u) return 16;

  __NOP(); __DSB(); __ISB(); __DMB();
  if (__REV(0x12345678u) != 0x78563412u) return 17;
  if (__REV16(0x12345678u) != 0x34127856u) return 18;
  if (__REVSH((int16_t)0x0080) != (int16_t)0x8000) return 19;
  if (__RBIT(1u) != 0x80000000u) return 20;
  if (__CLZ(1u) != 31u || __CLZ(0u) != 32u) return 21;
  if (__ROR(0x80000001u, 1u) != 0xC0000000u) return 22;
  if (__SSAT(1000, 8) != 127 || __USAT(-5, 8) != 0u) return 23;
  {
    volatile uint32_t word = 5u;
    uint32_t v = __LDREXW(&word);
    if (__STREXW(v + 1u, &word) != 0u || word != 6u) return 24;
    __CLREX();
  }
  {
    uint8_t buf[8] = {0};
    __UNALIGNED_UINT32_WRITE(&buf[1], 0xA1B2C3D4u);
    if (__UNALIGNED_UINT32_READ(&buf[1]) != 0xA1B2C3D4u) return 25;
  }
  __set_CONTROL(__get_CONTROL());
  __set_MSP(__get_MSP());
  __set_PSP(__get_PSP());
  (void)__get_xPSR(); (void)__get_APSR(); (void)__get_FPSCR();
  return 0;
}

uint32_t fw_ticks(void) { return ticks; }
