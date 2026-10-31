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
    [[  bus.map(0x48000000u, 0x400u, &m_GPIOA);]])
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
