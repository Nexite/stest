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
add_library(stest_runtime STATIC src/mmio.cpp src/bus.cpp src/clock.cpp src/peripheral.cpp)
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
