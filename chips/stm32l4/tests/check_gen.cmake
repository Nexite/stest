# chip_gen_stm32l496rg: -DPYTHON -DGEN -DTOML -DOUT
execute_process(COMMAND ${PYTHON} ${GEN} ${TOML} ${OUT} RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_chip failed: ${err}")
endif()
file(READ ${OUT} text)
foreach(needle
    [[__attribute__((weak)) void TIM7_IRQHandler(void){ stest_rt_abort("unhandled IRQ TIM7 (55)"); }]]
    [[__attribute__((weak)) void DMA2D_IRQHandler(void){ stest_rt_abort("unhandled IRQ DMA2D (90)"); }]]
    [[__attribute__((weak)) void SysTick_Handler(void){ stest_rt_abort("unhandled exception SysTick (-1)"); }]]
    [[    /*   79 */ nullptr,]]
    [[extern const stest::ChipDesc stest_chip = {"STM32L496RG", 4u, kIrqTable, 107, &stest_chip_init, &chip_startup};]]
    [[  core.set_systick_external_divider(8);]]
    [[  bus.map(0x48000000u, 0x400u, &m_GPIOA);]]
    [[  ctx.set_clock_tree(&m_RCC);]]
    [[  bus.set_gate(&m_GPIOB, ctx.clocks().enable_word(0x4Cu), 1u << 1, "RCC AHB2ENR bit 1");]]
    [[  bus.set_gate(&m_GPIOI, ctx.clocks().enable_word(0x4Cu), 1u << 8, "RCC AHB2ENR bit 8");]]
    [[  bus.set_gate(&m_PWR, ctx.clocks().enable_word(0x58u), 1u << 28, "RCC APB1ENR1 bit 28");]]
    [[  bus.set_gate(&m_TIM7, ctx.clocks().enable_word(0x58u), 1u << 5, "RCC APB1ENR1 bit 5");]]
    [[  bus.set_gate(&m_TIM1, ctx.clocks().enable_word(0x60u), 1u << 11, "RCC APB2ENR bit 11");]]
    [[  bus.set_gate(&m_TIM17, ctx.clocks().enable_word(0x60u), 1u << 18, "RCC APB2ENR bit 18");]]
    [[  bus.map(0x50040000u, 0x400u, &m_ADC);]]
    [[  bus.set_gate(&m_ADC, ctx.clocks().enable_word(0x4Cu), 1u << 13, "RCC AHB2ENR bit 13");]]
    [[  bus.set_gate(&m_DAC1, ctx.clocks().enable_word(0x58u), 1u << 29, "RCC APB1ENR1 bit 29");]]
    [[{"adc":[1,2],"channel":16,"pin":"PB1"}]]
    [[{"adc":[1,2,3],"channel":1,"pin":"PC0"}]])
  string(FIND "${text}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "generated file lacks: ${needle}")
  endif()
endforeach()
string(REGEX MATCHALL "_IRQHandler\\(void\\)\\{" weak "${text}")
list(LENGTH weak n)
if(NOT n EQUAL 90)
  message(FATAL_ERROR "expected 90 weak IRQ handlers, got ${n}")
endif()
foreach(ungated RCC FLASH)
  string(FIND "${text}" "bus.set_gate(&m_${ungated}," pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "${ungated} must not be gated")
  endif()
endforeach()
