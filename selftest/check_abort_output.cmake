# selftest_standalone_abort_output: -DEXE=<faults_abort_output>
# Runs the firmware standalone; it prints two printf lines, one ITM line and an unterminated
# printf fragment, then makes a strict-abort access, all within one quantum. Asserts exit 70,
# the stest: message on stderr, and EVT lines for all of the output, in order, on stdout.
# stdout goes to a file (fully buffered EVT output, as with `> log`): earlier quanta must survive.
execute_process(COMMAND ${EXE} --run-ms 50 OUTPUT_FILE ${CMAKE_CURRENT_BINARY_DIR}/abort_output.log
                RESULT_VARIABLE rc ERROR_VARIABLE err)
file(READ ${CMAKE_CURRENT_BINARY_DIR}/abort_output.log out)
if(NOT rc EQUAL 70)
  message(FATAL_ERROR "expected exit code 70, got '${rc}'\nstdout:\n${out}\nstderr:\n${err}")
endif()
if(NOT err MATCHES "stest: unmodelled write32 0x40013800")
  message(FATAL_ERROR "missing abort message on stderr\nstdout:\n${out}\nstderr:\n${err}")
endif()
set(pos 0)
foreach(needle
    "\"text\":\"early 1\""
    "\"text\":\"early 2\""
    "\"text\":\"out 1\""
    "\"text\":\"out 2\""
    "\"kind\":\"itm\""
    "\"text\":\"itm 1\""
    "\"text\":\"frag\"")
  string(FIND "${out}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "missing '${needle}' in standalone output\nstdout:\n${out}\nstderr:\n${err}")
  endif()
  if(at LESS pos)
    message(FATAL_ERROR "'${needle}' out of order\nstdout:\n${out}")
  endif()
  set(pos ${at})
endforeach()
if(NOT out MATCHES "\"text\":\"frag\"[^\n]*\"partial\":true|\"partial\":true[^\n]*\"text\":\"frag\"")
  message(FATAL_ERROR "fragment not marked partial\nstdout:\n${out}")
endif()
message(STATUS "abort output OK:\n${out}")
