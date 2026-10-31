include(FetchContent)
set(FETCHCONTENT_QUIET ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # doctest v2.4.11 declares an old minimum
FetchContent_Declare(doctest GIT_REPOSITORY https://github.com/doctest/doctest.git GIT_TAG v2.4.11 GIT_SHALLOW TRUE)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3 GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
set(DOCTEST_WITH_TESTS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(doctest nlohmann_json)

# Never instrumented by the MMIO pass: runtime volatile accesses must not recurse.
find_package(Threads REQUIRED)
add_library(stest_runtime STATIC src/mmio.cpp src/bus.cpp src/clock.cpp src/peripheral.cpp src/cortexm.cpp
                                 src/node.cpp src/control_channel.cpp src/model_context.cpp
                                 # Core-generic ITM/DWT/CoreDebug models (chips/cortexm), mapped by Cortexm.
                                 ../chips/cortexm/coredebug.cpp ../chips/cortexm/itm.cpp ../chips/cortexm/dwt.cpp)
target_include_directories(stest_runtime PUBLIC include ${PROJECT_SOURCE_DIR}/chips)
target_link_libraries(stest_runtime PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
# Hot path of every firmware access: always optimised, even in Debug/unset configs.
target_compile_options(stest_runtime PRIVATE -O2)

add_executable(runtime_tests tests/main.cpp tests/test_bus.cpp tests/test_clock.cpp tests/test_mmio.cpp)
target_link_libraries(runtime_tests PRIVATE stest_runtime doctest::doctest)
add_test(NAME runtime_unit COMMAND runtime_tests)

add_executable(runtime_unmodelled_c tests/unmodelled.c)
target_link_libraries(runtime_unmodelled_c PRIVATE stest_runtime)
add_test(NAME runtime_unmodelled_exit70
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:runtime_unmodelled_c>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exit70.cmake)

add_executable(runtime_abort_cpp tests/abort_cpp.cpp)
target_link_libraries(runtime_abort_cpp PRIVATE stest_runtime)
foreach(mode model_throw_int hook_throw_int hook_throw_std)
  add_test(NAME runtime_abort_${mode}
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:runtime_abort_cpp> -DARGS=${mode}
            "-DPATTERN=stest: (unknown )?exception in (model|delivery hook)"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exit70.cmake)
endforeach()

# ---- Task 4: Cortex-M core ----
add_executable(runtime_cortexm_tests tests/main.cpp tests/test_cortexm.cpp)
target_link_libraries(runtime_cortexm_tests PRIVATE stest_runtime doctest::doctest)
add_test(NAME runtime_cortexm COMMAND runtime_cortexm_tests)

add_executable(runtime_cortexm_abort tests/cortexm_abort.cpp)
target_link_libraries(runtime_cortexm_abort PRIVATE stest_runtime)
set(_stest_abort_cases
  "clock_throw_int|stest: unknown exception in clock event"
  "clock_throw_std|stest: exception in clock event: tick"
  "capi_no_core|stest: no Cortex-M core installed"
  "not_modelled_read|^stest: FAKE1: frobnication \\(CR.FROB\\) not modelled \\(read16 0x40001002\\)"
  "not_modelled_clock|^stest: FAKE2: widgets not modelled\n"
  "sysresetreq|^stest: SCB: system reset \\(AIRCR.SYSRESETREQ\\) not modelled \\(write32 0xe000ed0c = 0x05fa0004\\)"
  "null_handler|stest: .*no handler for exception 16"
  "wfi_deadlock|stest: .*wfi.*nothing can wake"
  "systick_no_getter|stest: exception in model at 0xe000e010: .*core clock getter")
foreach(case IN LISTS _stest_abort_cases)
  string(REPLACE "|" ";" case "${case}")
  list(GET case 0 mode)
  list(GET case 1 pattern)
  add_test(NAME runtime_cortexm_abort_${mode}
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:runtime_cortexm_abort> -DARGS=${mode}
            "-DPATTERN=${pattern}" -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exit70.cmake)
endforeach()

# CMSIS-Core: ST's cmsis_core at the pinned commit (or STEST_CMSIS_CORE_DIR), shared
# with firmware builds (cmake/StestFetchST.cmake).
stest_cmsis_core_include(_stest_cmsis_core)

# End-to-end: firmware using real core_cm4.h + the host shim, compiled with the MMIO
# pass, driving NVIC/SCB/SysTick and intrinsics through the runtime.
foreach(opt O0 O2)
  add_library(runtime_cmsis_fw_${opt} OBJECT tests/cmsis_e2e/fw.c)
  target_include_directories(runtime_cmsis_fw_${opt} BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/chips/cmsis_host)
  target_include_directories(runtime_cmsis_fw_${opt} PRIVATE ${_stest_cmsis_core} include)
  target_compile_options(runtime_cmsis_fw_${opt} PRIVATE -std=c11 -${opt} -Wall -Wextra -Werror)
  stest_enable_mmio(runtime_cmsis_fw_${opt})
  add_executable(runtime_cmsis_e2e_${opt} tests/cmsis_e2e/harness.cpp $<TARGET_OBJECTS:runtime_cmsis_fw_${opt}>)
  target_link_libraries(runtime_cmsis_e2e_${opt} PRIVATE stest_runtime)
  add_test(NAME runtime_cortexm_cmsis_e2e_${opt} COMMAND runtime_cmsis_e2e_${opt})
endforeach()

# ---- Task 5: node (firmware thread + baton) ----
add_executable(runtime_node_tests tests/main.cpp tests/test_node.cpp)
target_link_libraries(runtime_node_tests PRIVATE stest_runtime doctest::doctest)
add_test(NAME runtime_node COMMAND runtime_node_tests)

# ---- Task 5 fix: poll fast-forward ----
add_executable(runtime_poll_skip_tests tests/main.cpp tests/test_poll_skip.cpp)
target_link_libraries(runtime_poll_skip_tests PRIVATE stest_runtime doctest::doctest)
add_test(NAME runtime_poll_skip COMMAND runtime_poll_skip_tests)

# ---- Plan 2 Task 2: model context, signals, clock-enable gating ----
add_executable(runtime_model_context_tests tests/main.cpp tests/test_model_context.cpp)
target_link_libraries(runtime_model_context_tests PRIVATE stest_runtime doctest::doctest)
add_test(NAME runtime_model_context COMMAND runtime_model_context_tests)

# ---- Plan 2 Task 8: ITM / DWT / CoreDebug (chips/cortexm) ----
add_executable(runtime_trace_tests tests/main.cpp tests/test_trace.cpp)
target_link_libraries(runtime_trace_tests PRIVATE stest_runtime doctest::doctest)
add_test(NAME runtime_trace COMMAND runtime_trace_tests)

# Every CTest that runs a firmware/node executable gets a 30 s timeout.
get_property(_stest_rt_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_stest_rt_tests} PROPERTIES TIMEOUT 30)
