# selftest_standalone_partial_end: -DEXE=<faults_partial_end>
# A normal --run-ms end must emit unterminated stdout text as a partial event; exit code 0.
execute_process(COMMAND ${EXE} --run-ms 20 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "expected exit code 0, got '${rc}'\nstdout:\n${out}\nstderr:\n${err}")
endif()
if(NOT out MATCHES "EVT [0-9]+ [^\n]*\"partial\":true[^\n]*\"text\":\"tail\"")
  message(FATAL_ERROR "missing partial 'tail' event\nstdout:\n${out}\nstderr:\n${err}")
endif()
message(STATUS "partial end OK:\n${out}")
