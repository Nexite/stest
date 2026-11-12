# stest_add_firmware(<name>
#   CHIP <part>               # e.g. STM32L496RG -> chips/*/parts/stm32l496rg.toml
#   SOURCES <files...>        # firmware sources incl. main.c, *_it.c, *_hal_msp.c
#   [INCLUDE_DIRS <dirs...>]  # firmware include dirs (stm32l4xx_hal_conf.h lives here)
#   [DEFINES <defs...>]       # extra definitions for firmware + HAL
#   [HAL_SOURCES <files...>]  # default: every <family>xx_hal*.c of the HAL (not *_template.c)
#   [MODELS <TYPE>=<register|hal>...])  # per peripheral type, default register (see below)
#
# MODELS: TYPE is a peripheral type of the part (model kind upper-cased: GPIO, TIM, ...).
# `register` (default): ST's HAL drives the model through its registers. `hal`: the family's
# HAL shim (chips/<family>/family.cmake: STEST_<FAMILY>_HAL_SHIMS, ..._HAL_SHIM_<TYPE>_REPLACES
# / _SOURCE) replaces that ST HAL file in the HAL archive and calls the model's C API
# (runtime/include/stest/hal_api.h); the generated chip glue (gen_chip.py --hal) marks the
# type's blocks hal-owned, so firmware MMIO to them warns once per register. An unknown TYPE,
# a type without a shim, or another mode is a configure error listing what is supported.
#
# Builds executable <name> = firmware objects + HAL/system archive (all compiled with
# STEST_CLANG + the MMIO pass, -g -O0, -D<device_define> -DUSE_HAL_DRIVER) + the
# generated chip object (weak IRQ defaults, models, IRQ table) + node_main.cpp, linked
# against the uninstrumented runtime and chip models. Firmware sources (only) get
# -Dmain=stest_firmware_main. On macOS a POST_BUILD dsymutil writes <exe>.dSYM.
#
# Include order: chips/cmsis_host (host cmsis_compiler.h) first, then INCLUDE_DIRS,
# HAL Inc, CMSIS device Include, CMSIS-Core Include, runtime/include.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/StestFetchST.cmake)
get_filename_component(_STEST_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

if(APPLE)
  get_filename_component(_stest_llvm_bin "${STEST_CLANG}" DIRECTORY)
  find_program(STEST_DSYMUTIL NAMES dsymutil llvm-dsymutil HINTS "${_stest_llvm_bin}")
  if(NOT STEST_DSYMUTIL)
    message(FATAL_ERROR "stest: dsymutil not found (needed for firmware debug info on macOS)")
  endif()
endif()

function(stest_add_firmware name)
  cmake_parse_arguments(PARSE_ARGV 1 FW "" "CHIP" "SOURCES;INCLUDE_DIRS;DEFINES;HAL_SOURCES;MODELS")
  if(FW_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "stest_add_firmware(${name}): unknown arguments ${FW_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT FW_CHIP OR NOT FW_SOURCES)
    message(FATAL_ERROR "stest_add_firmware(${name}): CHIP and SOURCES are required")
  endif()

  # ---- toolchain / include sanity ----
  # Firmware + HAL must be compiled by the clang the MMIO pass was built against
  # (CMake has no per-target compiler, so this is enforced rather than overridden).
  get_filename_component(_fw_cc "${CMAKE_C_COMPILER}" REALPATH)
  get_filename_component(_fw_clang "${STEST_CLANG}" REALPATH)
  if(NOT _fw_cc STREQUAL _fw_clang)
    message(FATAL_ERROR "stest_add_firmware(${name}): the C compiler is ${CMAKE_C_COMPILER} but firmware "
            "must be built with ${STEST_CLANG} (STEST_CLANG). Configure with "
            "-DCMAKE_C_COMPILER=${STEST_CLANG} (or leave CMAKE_C_COMPILER unset).")
  endif()
  execute_process(COMMAND "${STEST_CLANG}" --version OUTPUT_VARIABLE _fw_cv ERROR_QUIET)
  get_property(_fw_llvm_ver GLOBAL PROPERTY STEST_LLVM_VERSION)
  if(NOT _fw_llvm_ver)
    message(FATAL_ERROR "stest_add_firmware(${name}): add_subdirectory(compiler/mmio_pass) must come first")
  endif()
  string(REGEX MATCH "clang version ([0-9]+)" _ "${_fw_cv}")
  set(_fw_clang_major "${CMAKE_MATCH_1}")
  string(REGEX MATCH "^[0-9]+" _fw_llvm_major "${_fw_llvm_ver}")
  if(NOT _fw_clang_major STREQUAL _fw_llvm_major)
    message(FATAL_ERROR "stest_add_firmware(${name}): clang major version ${_fw_clang_major} (${STEST_CLANG}) "
            "differs from LLVM ${_fw_llvm_major} the MMIO pass was built against; use a matching clang.")
  endif()
  foreach(_d IN LISTS FW_INCLUDE_DIRS)
    file(GLOB _bad "${_d}/core_cm*.h" "${_d}/cmsis_gcc.h" "${_d}/cmsis_compiler.h")
    if(_bad)
      message(FATAL_ERROR "stest_add_firmware(${name}): INCLUDE_DIRS entry ${_d} contains CMSIS-Core headers "
              "(${_bad}). Drop your Drivers/CMSIS/Include from INCLUDE_DIRS: stest provides CMSIS-Core "
              "and its host shim (chips/cmsis_host).")
    endif()
  endforeach()

  # ---- chip part ----
  string(TOLOWER "${FW_CHIP}" part)
  file(GLOB toml "${_STEST_ROOT}/chips/*/parts/${part}.toml")
  list(LENGTH toml n)
  if(NOT n EQUAL 1)
    message(FATAL_ERROR "stest_add_firmware(${name}): chip '${FW_CHIP}' -> ${n} matches for chips/*/parts/${part}.toml")
  endif()
  set(gen_py "${_STEST_ROOT}/tools/gen_chip.py")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${gen_py}" --info "${toml}"
                  OUTPUT_VARIABLE info ERROR_VARIABLE err RESULT_VARIABLE rc
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "stest_add_firmware(${name}): ${err}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${toml}" "${gen_py}")
  list(GET info 1 family)
  list(GET info 2 device_define)

  # ---- MODELS: per-type register/hal selection ----
  string(TOUPPER "${family}" _fam)
  set(STEST_${_fam}_HAL_SHIMS "")
  include("${_STEST_ROOT}/chips/${family}/family.cmake" OPTIONAL RESULT_VARIABLE _fam_cmake)
  if(_fam_cmake)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fam_cmake}")
  endif()
  set(_shims "${STEST_${_fam}_HAL_SHIMS}")
  if(_shims)
    string(REPLACE ";" " " _hal_supported "${_shims}")
  else()
    set(_hal_supported "(none)")
  endif()
  set(hal_types "")
  set(_seen_types "")
  if(FW_MODELS)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${gen_py}" --types "${toml}"
                    OUTPUT_VARIABLE _types ERROR_VARIABLE err RESULT_VARIABLE rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "stest_add_firmware(${name}): ${err}")
    endif()
    string(REPLACE ";" " " _types_txt "${_types}")
  endif()
  foreach(_spec IN LISTS FW_MODELS)
    set(_ctx "stest_add_firmware(${name}): MODELS ${_spec}")
    set(_supported "supported: every type of ${FW_CHIP} in register mode (${_types_txt}); hal mode is supported for: ${_hal_supported}")
    if(NOT _spec MATCHES "^([A-Za-z0-9_]+)=(.*)$")
      message(FATAL_ERROR "${_ctx}: expected <TYPE>=register or <TYPE>=hal; ${_supported}")
    endif()
    string(TOUPPER "${CMAKE_MATCH_1}" _type)
    set(_mode "${CMAKE_MATCH_2}")
    if(NOT _type IN_LIST _types)
      message(FATAL_ERROR "${_ctx}: unknown peripheral type ${_type} for ${FW_CHIP}; ${_supported}")
    endif()
    if(_type IN_LIST _seen_types)
      message(FATAL_ERROR "${_ctx}: ${_type} is listed more than once")
    endif()
    list(APPEND _seen_types ${_type})
    if(_mode STREQUAL "hal")
      if(NOT _type IN_LIST _shims)
        message(FATAL_ERROR "${_ctx}: ${_type} has no hal shim on ${family} (register mode only); ${_supported}")
      endif()
      list(APPEND hal_types ${_type})
    elseif(NOT _mode STREQUAL "register")
      message(FATAL_ERROR "${_ctx}: unknown mode '${_mode}' (want register or hal); ${_supported}")
    endif()
  endforeach()

  # ---- ST sources ----
  stest_st_family_dirs(${family} hal_dir device_dir)
  stest_cmsis_core_include(core_inc)
  if(NOT FW_HAL_SOURCES)
    file(GLOB FW_HAL_SOURCES "${hal_dir}/Src/${family}xx_hal*.c")
    list(FILTER FW_HAL_SOURCES EXCLUDE REGEX "_template\\.c$")
  endif()
  # hal mode: the shim replaces the whole ST file (ST sources are never patched).
  foreach(_type IN LISTS hal_types)
    set(_repl "${STEST_${_fam}_HAL_SHIM_${_type}_REPLACES}")
    set(_shim "${STEST_${_fam}_HAL_SHIM_${_type}_SOURCE}")
    if(NOT _repl OR NOT EXISTS "${_shim}")
      message(FATAL_ERROR "stest_add_firmware(${name}): chips/${family}/family.cmake: incomplete hal shim for ${_type}")
    endif()
    string(REPLACE "." "\\." _repl_re "${_repl}")
    list(FILTER FW_HAL_SOURCES EXCLUDE REGEX "(^|/)${_repl_re}$")
    list(APPEND FW_HAL_SOURCES "${_shim}")
  endforeach()
  set(system_c "${device_dir}/Source/Templates/system_${family}xx.c")

  # ---- common compile settings for firmware + HAL ----
  add_library(${name}_fwcfg INTERFACE)
  target_include_directories(${name}_fwcfg INTERFACE
    ${FW_INCLUDE_DIRS} "${hal_dir}/Inc" "${device_dir}/Include" "${core_inc}"
    "${_STEST_ROOT}/runtime/include")
  target_compile_definitions(${name}_fwcfg INTERFACE ${device_define} USE_HAL_DRIVER ${FW_DEFINES})
  target_compile_options(${name}_fwcfg INTERFACE -g -O0)

  # HAL + CMSIS system file: an archive, so firmware copies (e.g. a CubeMX
  # Core/Src/system_*.c) and strong overrides of __weak HAL functions win.
  add_library(${name}_hal STATIC ${FW_HAL_SOURCES} "${system_c}")
  target_include_directories(${name}_hal BEFORE PRIVATE "${_STEST_ROOT}/chips/cmsis_host")
  target_link_libraries(${name}_hal PRIVATE ${name}_fwcfg)
  stest_enable_mmio(${name}_hal)

  add_library(${name}_fw OBJECT ${FW_SOURCES})
  target_include_directories(${name}_fw BEFORE PRIVATE "${_STEST_ROOT}/chips/cmsis_host")
  target_link_libraries(${name}_fw PRIVATE ${name}_fwcfg)
  target_compile_definitions(${name}_fw PRIVATE main=stest_firmware_main)
  stest_enable_mmio(${name}_fw)

  # Generated chip glue: compiled into the executable as an object (not archived) so
  # firmware handlers override its weak defaults on ELF and Mach-O.
  set(gen "${CMAKE_CURRENT_BINARY_DIR}/${name}_chip.cpp")
  set(gen_hal "")
  if(hal_types)
    string(REPLACE ";" "," _hal_csv "${hal_types}")
    set(gen_hal --hal "${_hal_csv}")
  endif()
  add_custom_command(OUTPUT "${gen}"
    COMMAND "${Python3_EXECUTABLE}" "${gen_py}" "${toml}" "${gen}" ${gen_hal}
    DEPENDS "${toml}" "${gen_py}"
    COMMENT "Generating ${FW_CHIP} chip glue for ${name}")

  add_executable(${name} $<TARGET_OBJECTS:${name}_fw> "${gen}" "${_STEST_ROOT}/runtime/src/node_main.cpp")
  target_link_libraries(${name} PRIVATE ${name}_hal stest_chip_${family})
  if(APPLE)
    if(STEST_XCODE_LD)
      target_link_options(${name} PRIVATE "-fuse-ld=${STEST_XCODE_LD}")
    endif()
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND "${STEST_DSYMUTIL}" "$<TARGET_FILE:${name}>" -o "$<TARGET_FILE:${name}>.dSYM"
      COMMENT "dsymutil ${name}")
  endif()
endfunction()
