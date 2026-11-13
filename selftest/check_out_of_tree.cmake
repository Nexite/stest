# Configures (and for CASE=ok builds and tests) an out-of-tree consumer project of stest
# (selftest/out_of_tree/<CASE>), the supported add_subdirectory pattern.
#   ok:           configure + build succeed; the consumer's ctest lists exactly its own
#                 consumer_smoke test (none of stest's runtime_/chip_/selftest_/python_/spike
#                 tests) and consumer_smoke passes.
#   no_toolchain: StestToolchain.cmake not included before project(): configure fails and
#                 says so.
# Inputs: CASE (ok|no_toolchain), STEST_ROOT, WORK, STEST_CLANG, STEST_LLVM_ROOT, STEST_CLANGXX,
#         LLVM_DIR, CMAKE_BIN, CTEST_BIN, GEN, MAKE_PROGRAM; optional already-fetched sources
#         (JSON_SRC, ST_HAL_SRC, ST_DEV_SRC, ST_CORE_SRC) passed as FetchContent overrides so the
#         nested configure does not download them again.
file(REMOVE_RECURSE "${WORK}")
set(args -S "${STEST_ROOT}/selftest/out_of_tree/${CASE}" -B "${WORK}" -DSTEST_DIR=${STEST_ROOT}
    -DSTEST_CLANG=${STEST_CLANG} -DLLVM_DIR=${LLVM_DIR}
    -G ${GEN} -DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM} -DCMAKE_BUILD_TYPE=Release)
if(STEST_LLVM_ROOT)
  list(APPEND args -DSTEST_LLVM_ROOT=${STEST_LLVM_ROOT})
endif()
foreach(pair "NLOHMANN_JSON|JSON_SRC" "STEST_ST_STM32L4XX_HAL_DRIVER|ST_HAL_SRC"
             "STEST_ST_CMSIS_DEVICE_L4|ST_DEV_SRC" "STEST_ST_CMSIS_CORE|ST_CORE_SRC")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 dep)
  list(GET pair 1 var)
  if(${var})
    list(APPEND args "-DFETCHCONTENT_SOURCE_DIR_${dep}=${${var}}")
  endif()
endforeach()

if(CASE STREQUAL "ok")
  execute_process(COMMAND "${CMAKE_BIN}" ${args} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "consumer configure failed:\n${out}")
  endif()
  execute_process(COMMAND "${CMAKE_BIN}" --build "${WORK}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "consumer build failed:\n${out}")
  endif()
  execute_process(COMMAND "${CTEST_BIN}" --test-dir "${WORK}" -N
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0 OR NOT out MATCHES "consumer_smoke")
    message(FATAL_ERROR "consumer ctest -N does not list consumer_smoke:\n${out}")
  endif()
  # Match test lines only: the output starts with "Test project <WORK>", and WORK may itself
  # contain one of these prefixes (e.g. a checkout under ~/python_projects).
  if(out MATCHES "Test +#[0-9]+: (runtime_|chip_|selftest_|python_)")
    message(FATAL_ERROR "stest's own tests are registered in the consumer:\n${out}")
  endif()
  if(NOT out MATCHES "Total Tests: 1[\r\n]")
    message(FATAL_ERROR "consumer ctest -N lists tests other than consumer_smoke:\n${out}")
  endif()
  execute_process(COMMAND "${CTEST_BIN}" --test-dir "${WORK}" -R consumer_smoke --output-on-failure
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "consumer_smoke failed:\n${out}")
  endif()
elseif(CASE STREQUAL "no_toolchain")
  # The consumer chose its compilers itself (the Docker image has no `cc`, so without them
  # project() would fail before stest is reached) but never included StestToolchain.cmake.
  list(APPEND args -DCMAKE_C_COMPILER=${STEST_CLANG} -DCMAKE_CXX_COMPILER=${STEST_CLANGXX})
  execute_process(COMMAND "${CMAKE_BIN}" ${args} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0)
    message(FATAL_ERROR "configure unexpectedly succeeded without StestToolchain.cmake:\n${out}\n${err}")
  endif()
  # CMake word-wraps message() text by path length, so match on collapsed whitespace.
  string(REGEX REPLACE "[ \t\r\n]+" " " err_flat "${err}")
  if(NOT err_flat MATCHES "StestToolchain\\.cmake" OR NOT err_flat MATCHES "before project\\(\\)")
    message(FATAL_ERROR "configure failed without the expected StestToolchain.cmake / before project() message:\n${err}")
  endif()
else()
  message(FATAL_ERROR "unknown CASE ${CASE}")
endif()
