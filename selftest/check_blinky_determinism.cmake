# selftest_blinky_quantum_determinism: -DEXE=<blinky>
# The event log must not depend on the quantum size.
set(logs)
foreach(q 1000 7 333 100000)
  execute_process(COMMAND ${EXE} --run-ms 2100 --quantum-us ${q}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "quantum ${q}: exit '${rc}'\n${err}")
  endif()
  if(NOT DEFINED ref)
    set(ref "${out}")
  elseif(NOT out STREQUAL ref)
    message(FATAL_ERROR "quantum ${q} log differs:\n${out}\nvs quantum 1000:\n${ref}")
  endif()
endforeach()
message(STATUS "identical logs:\n${ref}")
