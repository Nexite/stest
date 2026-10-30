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
add_library(stest_runtime STATIC src/mmio.cpp src/bus.cpp src/clock.cpp src/peripheral.cpp src/cortexm.cpp)
target_include_directories(stest_runtime PUBLIC include)
target_link_libraries(stest_runtime PUBLIC nlohmann_json::nlohmann_json)

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
  "sysresetreq|stest: exception in model at 0xe000ed0c: .*SYSRESETREQ"
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

# CMSIS-Core 5.9.0 headers (only the three core_cm4.h needs; cmsis_compiler.h is
# replaced by chips/cmsis_host). Pinned by SHA-256; STEST_CMSIS_CORE_DIR overrides.
set(STEST_CMSIS_CORE_DIR "" CACHE PATH "CMSIS-Core Include dir containing core_cm4.h (downloaded if empty)")
if(STEST_CMSIS_CORE_DIR)
  set(_stest_cmsis_core "${STEST_CMSIS_CORE_DIR}")
else()
  set(_stest_cmsis_core "${CMAKE_BINARY_DIR}/_deps/cmsis_core_5.9.0")
  set(_stest_cmsis_files
    "core_cm4.h|f5b63d52dd1557b15ca414cb59c264f595a7b5669db2a5878dffe22f4caedc8c"
    "cmsis_version.h|184c19fd3ee73632edf35a0b4d49cd48be75fbf49e6ccb19d9db05fa83bea4b3"
    "mpu_armv7.h|29206b52ee02290ed6f5a5415ebd4187de802cf176d9b3cb844390d8e5571372")
  foreach(entry IN LISTS _stest_cmsis_files)
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 f)
    list(GET entry 1 h)
    if(NOT EXISTS "${_stest_cmsis_core}/${f}")
      file(DOWNLOAD "https://raw.githubusercontent.com/ARM-software/CMSIS_5/5.9.0/CMSIS/Core/Include/${f}"
           "${_stest_cmsis_core}/${f}" EXPECTED_HASH SHA256=${h} STATUS _st)
      list(GET _st 0 _rc)
      if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "stest: downloading CMSIS-Core ${f} failed: ${_st}")
      endif()
    endif()
  endforeach()
endif()

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
