# selftest_blinky_quantum_determinism / selftest_wfiloop_quantum_determinism:
# -DEXE=<firmware> [-DRUN_MS=<ms> (default 2100)] [-DQUANTA=<us;us;...> (default 1000;7;333;100000)]
# The event log must not depend on the quantum size.
if(NOT DEFINED RUN_MS)
  set(RUN_MS 2100)
endif()
if(NOT DEFINED QUANTA)
  set(QUANTA 1000 7 333 100000)
endif()
set(logs)
foreach(q ${QUANTA})
  execute_process(COMMAND ${EXE} --run-ms ${RUN_MS} --quantum-us ${q}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "quantum ${q}: exit '${rc}'\n${err}")
  endif()
  if(NOT DEFINED ref)
    set(ref "${out}")
    set(ref_q ${q})
  elseif(NOT out STREQUAL ref)
    message(FATAL_ERROR "quantum ${q} log differs:\n${out}\nvs quantum ${ref_q}:\n${ref}")
  endif()
endforeach()
message(STATUS "identical logs:\n${ref}")
