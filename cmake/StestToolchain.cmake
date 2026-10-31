# Locates upstream LLVM/clang (never Apple clang) and defines stest_enable_mmio().
# Include this BEFORE project() so the compilers are chosen from the same LLVM
# that the pass plugin is built against.

set(STEST_LLVM_ROOT "" CACHE PATH "LLVM install prefix (contains bin/clang and lib/cmake/llvm)")

set(_stest_hints)
if(STEST_LLVM_ROOT)
  list(APPEND _stest_hints "${STEST_LLVM_ROOT}/bin")
elseif(DEFINED ENV{STEST_LLVM_ROOT})
  list(APPEND _stest_hints "$ENV{STEST_LLVM_ROOT}/bin")
endif()

find_program(_stest_brew brew)
if(_stest_brew)
  execute_process(COMMAND "${_stest_brew}" --prefix llvm
                  OUTPUT_VARIABLE _stest_brew_llvm OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _stest_brew_rc)
  if(_stest_brew_rc EQUAL 0 AND EXISTS "${_stest_brew_llvm}/bin/clang")
    list(APPEND _stest_hints "${_stest_brew_llvm}/bin")
  endif()
endif()

find_program(_stest_llvm_config
  NAMES llvm-config llvm-config-23 llvm-config-22 llvm-config-21 llvm-config-20
        llvm-config-19 llvm-config-18
  HINTS ${_stest_hints})
if(_stest_llvm_config)
  execute_process(COMMAND "${_stest_llvm_config}" --bindir
                  OUTPUT_VARIABLE _stest_cfg_bindir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${_stest_llvm_config}" --cmakedir
                  OUTPUT_VARIABLE _stest_cfg_cmakedir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_stest_cfg_bindir)
    list(APPEND _stest_hints "${_stest_cfg_bindir}")
  endif()
  if(_stest_cfg_cmakedir AND NOT LLVM_DIR)
    set(LLVM_DIR "${_stest_cfg_cmakedir}" CACHE PATH "LLVM cmake dir")
  endif()
endif()

find_program(STEST_CLANG NAMES clang HINTS ${_stest_hints} NO_DEFAULT_PATH)
if(NOT STEST_CLANG)
  find_program(STEST_CLANG NAMES clang clang-23 clang-22 clang-21 clang-20 clang-19 clang-18
               HINTS ${_stest_hints})
endif()
if(NOT STEST_CLANG)
  message(FATAL_ERROR "stest: upstream clang not found. Set -DSTEST_LLVM_ROOT=<llvm prefix>.")
endif()

execute_process(COMMAND "${STEST_CLANG}" --version
                OUTPUT_VARIABLE _stest_clang_version ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_stest_clang_version MATCHES "Apple")
  message(FATAL_ERROR "stest: ${STEST_CLANG} is Apple clang, which cannot load LLVM pass "
                      "plugins. Use upstream LLVM (brew install llvm) via -DSTEST_LLVM_ROOT.")
endif()

# clang++ lives next to clang (clang-18 -> clang++-18 on Debian).
get_filename_component(_stest_clang_dir "${STEST_CLANG}" DIRECTORY)
find_program(STEST_CLANGXX NAMES clang++ HINTS "${_stest_clang_dir}" NO_DEFAULT_PATH)
if(NOT STEST_CLANGXX)
  message(FATAL_ERROR "stest: clang++ not found next to ${STEST_CLANG}")
endif()

if(NOT CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "${STEST_CLANG}" CACHE FILEPATH "" FORCE)
endif()
if(NOT CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER "${STEST_CLANGXX}" CACHE FILEPATH "" FORCE)
endif()

# macOS: Xcode's ld (via xcrun). A cctools-port ld earlier in PATH (e.g. Anaconda's)
# produces plugin bundles dyld rejects and cannot read current DWARF (no debug map ->
# empty dSYM). Used for the pass plugin and firmware executables.
set(STEST_XCODE_LD "")
if(APPLE OR CMAKE_HOST_APPLE)
  execute_process(COMMAND xcrun -f ld OUTPUT_VARIABLE _stest_xcode_ld
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _stest_xcrun_rc)
  if(_stest_xcrun_rc EQUAL 0 AND EXISTS "${_stest_xcode_ld}")
    set(STEST_XCODE_LD "${_stest_xcode_ld}")
  endif()
endif()

# stest_enable_mmio(<target>): compile <target> with the MMIO interception pass.
function(stest_enable_mmio target)
  add_dependencies(${target} stest_mmio_pass)
  target_compile_options(${target} PRIVATE "-fpass-plugin=$<TARGET_FILE:stest_mmio_pass>")
  # add_dependencies alone does not order compile steps after a MODULE library's
  # link step (Ninja), so make every source of <target> depend on the plugin file
  # (path computed in compiler/mmio_pass/CMakeLists.txt; genexes are not allowed here).
  get_property(_plugin GLOBAL PROPERTY STEST_MMIO_PLUGIN_FILE)
  get_target_property(_srcs ${target} SOURCES)
  set_property(SOURCE ${_srcs} TARGET_DIRECTORY ${target} APPEND PROPERTY
               OBJECT_DEPENDS "${_plugin}")
endfunction()
