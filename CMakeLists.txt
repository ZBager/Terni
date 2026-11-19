# SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
# Copyright (C) 2026 Rafał Zając
cmake_minimum_required(VERSION 3.21)
project(Terni VERSION 0.1.0 LANGUAGES CXX)

# Terni can be the top-level project (development: tools + tests) or embedded in a
# host with add_subdirectory()/FetchContent (then only the library is built):
#
#   add_subdirectory(external/terni)
#   target_link_libraries(my_game PRIVATE Terni::core)      # or Terni::runtime (no shaderc)
#
if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
  endif()
endif()
option(TERNI_RULE_COMPILER "Build Terni::core, which compiles rules with shaderc (Terni::runtime is always built)" ON)
set(_terni_develop OFF)
if(PROJECT_IS_TOP_LEVEL AND TERNI_RULE_COMPILER)
  set(_terni_develop ON)
endif()
option(TERNI_BUILD_TOOLS "Build terni_run, terni_check, terni_diff and terni_script_check" ${_terni_develop})
option(TERNI_BUILD_TESTS "Build the test suite (needs glslc)" ${_terni_develop})
option(TERNI_BUILD_BENCHMARK "Build terni_bench, the benchmark against a CPU reference" ${_terni_develop})
option(TERNI_COMPILE_TOOL "Build terni_compile_rules, the rule compiler a game ships beside itself"
       ${TERNI_RULE_COMPILER})
if(TERNI_COMPILE_TOOL AND NOT TERNI_RULE_COMPILER)
  message(FATAL_ERROR "TERNI_COMPILE_TOOL needs TERNI_RULE_COMPILER")
endif()
option(TERNI_C_API "Build the C API (include/terni/terni.h) as shared libraries: terni and terni_runtime"
       ${PROJECT_IS_TOP_LEVEL})
if((TERNI_BUILD_TOOLS OR TERNI_BUILD_TESTS OR TERNI_BUILD_BENCHMARK) AND NOT TERNI_RULE_COMPILER)
  message(FATAL_ERROR "TERNI_BUILD_TOOLS, TERNI_BUILD_TESTS and TERNI_BUILD_BENCHMARK need TERNI_RULE_COMPILER")
endif()

find_package(Vulkan REQUIRED)
if(TERNI_RULE_COMPILER)
  # shaderc: compiles game rules (GLSL) at load time, so mods can change them
  # (the Vulkan SDK on Windows ships it next to the Vulkan loader and headers)
  get_filename_component(_terni_vulkan_libdir "${Vulkan_LIBRARY}" DIRECTORY)
  find_library(SHADERC_LIB NAMES shaderc_shared shaderc HINTS ${_terni_vulkan_libdir} "$ENV{VULKAN_SDK}/Lib" REQUIRED)
  find_path(SHADERC_INCLUDE shaderc/shaderc.hpp HINTS ${Vulkan_INCLUDE_DIRS} "$ENV{VULKAN_SDK}/Include" REQUIRED)
endif()
find_package(Threads REQUIRED)

set(TERNI_SHADER_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/shaders CACHE INTERNAL "Terni GLSL library folder")
if(MSVC)
  # /utf-8: the sources, and the GLSL library embedded from them, are UTF-8. Without it
  # MSVC reads them in the system code page, and the embedded bytes (and with them every
  # SPIR-V cache key) could differ from other compilers.
  set(TERNI_COMPILE_OPTIONS /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
  set(TERNI_COMPILE_OPTIONS -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)  # Info{VK_STRUCTURE_TYPE_...} idiom
endif()

# ---------------------------------------------------------------------------
# The GLSL library, embedded into the binary (rebuilt when any .glsl changes)
# ---------------------------------------------------------------------------
file(GLOB TERNI_GLSL_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/terni/*.glsl)
set(TERNI_GLSL_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/terni_glsl_library.cpp)
add_custom_command(
  OUTPUT ${TERNI_GLSL_CPP}
  COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/shaders -DOUT=${TERNI_GLSL_CPP}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_glsl.cmake
  DEPENDS ${TERNI_GLSL_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_glsl.cmake
  COMMENT "Embedding the Terni GLSL library"
  VERBATIM)

# ---------------------------------------------------------------------------
# The library: fixed point, rng, script parser, world description, modifiers,
# save/load, Vulkan compute runner. Two builds of the same API:
#   Terni::core     also compiles rules (GLSL -> SPIR-V) in process with shaderc
#   Terni::runtime  no rule compiler and no shaderc: every rule is loaded from a SPIR-V
#                   cache filled beforehand by terni::compile_rules() (Terni::core)
# ---------------------------------------------------------------------------
set(TERNI_SOURCES
  src/vk_context.cpp
  src/compute_runner.cpp
  src/script.cpp
  src/game_builder.cpp
  src/game_load.cpp
  src/game_sim.cpp
  src/order_lint.cpp
  ${TERNI_GLSL_CPP}
)
function(terni_library target export_name compiler_source)
  add_library(${target} STATIC ${TERNI_SOURCES} ${compiler_source})
  add_library(Terni::${export_name} ALIAS ${target})
  set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})  # the same name for find_package users
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${target} PUBLIC Vulkan::Vulkan PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  target_compile_options(${target} PRIVATE ${TERNI_COMPILE_OPTIONS})
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)  # also goes into shared libraries
endfunction()

terni_library(terni_runtime runtime src/rule_compiler_none.cpp)
set(TERNI_LIBRARIES terni_runtime)
if(TERNI_RULE_COMPILER)
  terni_library(terni_core core src/rule_compiler_shaderc.cpp)
  target_include_directories(terni_core PRIVATE ${SHADERC_INCLUDE})
  target_link_libraries(terni_core PRIVATE ${SHADERC_LIB})
  list(APPEND TERNI_LIBRARIES terni_core)
endif()

# ---------------------------------------------------------------------------
# The C API: shared libraries that export only the terni_* functions of terni.h.
#   terni           over Terni::core (compiles rules; needs shaderc beside it)
#   terni_runtime   over Terni::runtime (rules from a SPIR-V cache only)
# ---------------------------------------------------------------------------
function(terni_c_library target export_name output_name static_library)
  add_library(${target} SHARED src/c_api.cpp src/c_api_gpu.cpp)
  add_library(Terni::${export_name} ALIAS ${target})
  target_link_libraries(${target} PRIVATE ${static_library})
  target_include_directories(${target} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                              $<INSTALL_INTERFACE:include>)
  target_compile_definitions(${target} PRIVATE TERNI_C_BUILD TERNI_VERSION_STRING="${PROJECT_VERSION}")
  target_compile_options(${target} PRIVATE ${TERNI_COMPILE_OPTIONS})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name} EXPORT_NAME ${export_name}
                        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  if(WIN32)
    target_link_libraries(${target} PRIVATE dxgi)  # the adapter of a DirectX renderer
    # terni.dll, not libterni.dll (MinGW), so that every language finds it by its plain name;
    # the import library must not overwrite the static library of the same name (MSVC)
    set_target_properties(${target} PROPERTIES PREFIX "" ARCHIVE_OUTPUT_NAME ${output_name}_dll)
  elseif(NOT APPLE)
    target_link_options(${target} PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/terni_c.map
                                          -Wl,--no-undefined)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/terni_c.map)
  endif()
endfunction()
if(TERNI_C_API)
  terni_c_library(terni_c_runtime c_runtime terni_runtime terni_runtime)
  list(APPEND TERNI_LIBRARIES terni_c_runtime)
  if(TERNI_RULE_COMPILER)
    terni_c_library(terni_c c terni terni_core)
    list(APPEND TERNI_LIBRARIES terni_c)
  endif()
endif()

# ---------------------------------------------------------------------------
# terni_add_shaders(<target> OUTPUT_DIR <dir> SOURCES <*.comp>... [INCLUDE_DIRS <dir>...])
# Offline-compiles compute shaders with the Terni GLSL library on the include path
# (for hosts that ship SPIR-V instead of compiling rules at load time).
# ---------------------------------------------------------------------------
function(terni_add_shaders target)
  cmake_parse_arguments(ARG "" "OUTPUT_DIR" "SOURCES;INCLUDE_DIRS" ${ARGN})
  find_program(TERNI_GLSLC glslc REQUIRED HINTS ${Vulkan_GLSLC_EXECUTABLE})
  file(MAKE_DIRECTORY ${ARG_OUTPUT_DIR})
  set(includes -I ${TERNI_SHADER_INCLUDE})
  foreach(dir ${ARG_INCLUDE_DIRS})
    list(APPEND includes -I ${dir})
  endforeach()
  set(outputs)
  foreach(src ${ARG_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    set(spv ${ARG_OUTPUT_DIR}/${name}.spv)
    add_custom_command(
      OUTPUT ${spv}
      COMMAND ${TERNI_GLSLC} --target-env=vulkan1.3 -O ${includes} -MD -MF ${spv}.d ${src} -o ${spv}
      DEPENDS ${src}
      DEPFILE ${spv}.d
      COMMENT "glslc ${name}.comp"
      VERBATIM)
    list(APPEND outputs ${spv})
  endforeach()
  add_custom_target(${target}_shaders DEPENDS ${outputs})
  add_dependencies(${target} ${target}_shaders)
endfunction()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(TERNI_COMPILE_TOOL)
  # Fills the SPIR-V cache of a game whose program has no compiler (Terni::runtime), e.g.
  # when mods change rules. Found beside it: the shaderc libraries a game ships with it.
  add_executable(terni_compile_rules tools/terni_compile_rules.cpp)
  target_link_libraries(terni_compile_rules PRIVATE terni_core)
  target_compile_options(terni_compile_rules PRIVATE ${TERNI_COMPILE_OPTIONS})
  if(NOT WIN32 AND NOT APPLE)
    set_target_properties(terni_compile_rules PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
  install(TARGETS terni_compile_rules RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TERNI_BUILD_TOOLS)
  find_package(OpenMP REQUIRED)
  add_executable(terni_script_check tools/script_check.cpp)
  target_link_libraries(terni_script_check PRIVATE terni_core OpenMP::OpenMP_CXX)
  target_compile_options(terni_script_check PRIVATE ${TERNI_COMPILE_OPTIONS})

  add_executable(terni_run tools/terni_run.cpp)
  target_link_libraries(terni_run PRIVATE terni_core)
  target_compile_options(terni_run PRIVATE ${TERNI_COMPILE_OPTIONS})
  add_executable(terni_check tools/terni_check.cpp)  # what a modder runs before publishing
  target_link_libraries(terni_check PRIVATE terni_core)
  target_compile_options(terni_check PRIVATE ${TERNI_COMPILE_OPTIONS})
  add_executable(terni_diff tools/terni_diff.cpp)  # compares saves on the host: no compiler, no GPU work
  target_link_libraries(terni_diff PRIVATE terni_runtime)
  target_compile_options(terni_diff PRIVATE ${TERNI_COMPILE_OPTIONS})

  # docs/getting-started.md walks through this one
  add_executable(hello_towns examples/hello_towns/hello_towns.cpp)
  target_link_libraries(hello_towns PRIVATE terni_core)
  target_compile_options(hello_towns PRIVATE ${TERNI_COMPILE_OPTIONS})
endif()

# ---------------------------------------------------------------------------
# Benchmark: the same world on Vulkan devices and on a CPU reference (bench/)
# ---------------------------------------------------------------------------
if(TERNI_BUILD_BENCHMARK)
  add_library(terni_bench_world STATIC bench/bench_world.cpp bench/cpu_reference.cpp)
  target_include_directories(terni_bench_world PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bench)
  target_link_libraries(terni_bench_world PUBLIC terni_core PRIVATE Threads::Threads)
  target_compile_options(terni_bench_world PRIVATE ${TERNI_COMPILE_OPTIONS})

  # results name the commit they were measured at; configuring again follows new commits
  set(_terni_commit "unknown")
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE _terni_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _terni_git)
    if(_terni_git EQUAL 0 AND _terni_head)
      set(_terni_commit ${_terni_head})
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git/logs/HEAD)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/.git/logs/HEAD)
    endif()
  endif()
  add_executable(terni_bench bench/terni_bench.cpp bench/machine.cpp bench/frame_load.cpp)
  target_link_libraries(terni_bench PRIVATE terni_bench_world)
  target_include_directories(terni_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)  # the frame load compiles a shader
  target_compile_options(terni_bench PRIVATE ${TERNI_COMPILE_OPTIONS})
  target_compile_definitions(terni_bench PRIVATE TERNI_BENCH_TERNI_VERSION="${PROJECT_VERSION}"
                                                 TERNI_BENCH_COMMIT="${_terni_commit}"
                                                 TERNI_BENCH_BUILD_TYPE="$<CONFIG>")
endif()

# ---------------------------------------------------------------------------
# Tests: `ctest --test-dir build`
# ---------------------------------------------------------------------------
if(TERNI_BUILD_TESTS)
  enable_testing()
  function(terni_test name source)  # [library]: terni_core unless given
    set(library terni_core)
    if(ARGC GREATER 2)
      set(library ${ARGV2})
    endif()
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${library})
    target_compile_options(${name} PRIVATE ${TERNI_COMPILE_OPTIONS})
    target_compile_definitions(${name} PRIVATE TERNI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
                                               TERNI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                                               TERNI_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  endfunction()

  terni_test(test_fixed tests/test_fixed.cpp)
  add_test(NAME fixed_point COMMAND test_fixed)
  terni_test(test_script tests/test_script.cpp)
  add_test(NAME script_parser COMMAND test_script)
  terni_test(test_game tests/test_game.cpp)
  add_test(NAME game_loading COMMAND test_game)
  if(TERNI_BUILD_TOOLS)  # terni_check on a good mod, a mod with an order problem, and one that does not compile
    set(_mods ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/galaxy_mods)
    set(_galaxy ${CMAKE_CURRENT_SOURCE_DIR}/examples/mini_galaxy)
    add_test(NAME mod_check_good COMMAND terni_check ${_galaxy} ${_mods}/good_mod --ticks 20)
    set_tests_properties(mod_check_good PROPERTIES PASS_REGULAR_EXPRESSION
                         "good_mod replaces rules/planet_growth.glsl.*good_mod adds common/modifiers/festivals.txt.*ready: 0 errors, 0 warnings")
    add_test(NAME mod_check_broken COMMAND terni_check ${_galaxy} ${_mods}/broken_mod --ticks 20)
    set_tests_properties(mod_check_broken PROPERTIES PASS_REGULAR_EXPRESSION
                         "notes/todo.txt is never read.*broken_mod/rules/planet_growth.glsl:5: stage 'planet_growth' reads.*old_trade.glsl is not used.*depending on the order its threads run in.*NOT READY: 2 errors, 3 warnings")
    add_test(NAME mod_check_typo COMMAND terni_check ${_galaxy} ${_mods}/typo_mod --no-run)
    set_tests_properties(mod_check_typo PROPERTIES PASS_REGULAR_EXPRESSION
                         "rules/empire_income.glsl:5: error.*rules/empire_income.glsl is typo_mod/rules/empire_income.glsl.*NOT READY: 1 error")
  endif()
  terni_test(test_modifiers tests/test_modifiers.cpp)
  add_test(NAME modifiers COMMAND test_modifiers)
  terni_test(test_save tests/test_save.cpp)
  add_test(NAME save_load COMMAND test_save)
  terni_test(test_embedding tests/test_embedding.cpp)
  add_test(NAME embedding COMMAND test_embedding)
  terni_test(test_inherit tests/test_inherit.cpp)
  add_test(NAME inherited_modifiers COMMAND test_inherit)
  terni_test(test_order_check tests/test_order_check.cpp)
  add_test(NAME order_check COMMAND test_order_check)
  terni_test(test_portable_text tests/test_portable_text.cpp)
  target_include_directories(test_portable_text PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME portable_text COMMAND test_portable_text)
  terni_test(test_schedule tests/test_schedule.cpp)
  add_test(NAME stage_schedule COMMAND test_schedule)
  terni_test(test_partial_write tests/test_partial_write.cpp)
  add_test(NAME partial_writes COMMAND test_partial_write)
  terni_test(test_capacity tests/test_capacity.cpp)
  add_test(NAME capacities COMMAND test_capacity)
  terni_test(test_async_tick tests/test_async_tick.cpp)
  add_test(NAME ticks_that_do_not_block COMMAND test_async_tick)
  terni_test(test_dynamic tests/test_dynamic.cpp)
  add_test(NAME entities_at_run_time COMMAND test_dynamic)
  terni_test(test_device_health tests/test_device_health.cpp)
  target_include_directories(test_device_health PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME device_health COMMAND test_device_health)
  terni_test(test_broken_rules tests/test_broken_rules.cpp)
  add_test(NAME broken_rules COMMAND test_broken_rules)

  # These two check the shader math against __int128 on the CPU (GCC and Clang, including
  # MinGW). The SPIR-V they test is the same whichever compiler built the host.
  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles("#ifndef __SIZEOF_INT128__\n#error no __int128\n#endif\nint main() { return 0; }"
                            TERNI_HAVE_INT128)
  if(TERNI_HAVE_INT128)
    terni_test(test_gpu_parity tests/test_gpu_parity.cpp)
    target_compile_definitions(test_gpu_parity PRIVATE TERNI_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/tests/shaders")
    terni_add_shaders(test_gpu_parity
      OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/tests/shaders
      SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/parity_ops.comp
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/parity_reduce.comp)
    add_test(NAME gpu_parity COMMAND test_gpu_parity)

    terni_test(test_wide tests/test_wide.cpp)
    target_compile_definitions(test_wide PRIVATE TERNI_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/tests/shaders")
    terni_add_shaders(test_wide
      OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/tests/shaders
      SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/parity_wide.comp)
    add_test(NAME wide_integers COMMAND test_wide)
  endif()

  # Terni::runtime: runs rules from a cache that a Terni::core build filled
  terni_test(test_cache_fill tests/test_cache_fill.cpp)
  add_test(NAME runtime_cache_fill COMMAND test_cache_fill)
  set_tests_properties(runtime_cache_fill PROPERTIES FIXTURES_SETUP runtime_cache)
  terni_test(test_runtime tests/test_runtime.cpp terni_runtime)
  add_test(NAME runtime_cache_only COMMAND test_runtime)
  set_tests_properties(runtime_cache_only PROPERTIES FIXTURES_REQUIRED runtime_cache)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_READELF)
    # ... and needs no shaderc at all
    add_test(NAME runtime_without_shaderc
             COMMAND sh -c "d=$('${CMAKE_READELF}' -d \"$1\") || exit 1; printf '%s\\n' \"$d\" | grep -q NEEDED || exit 1; ! printf '%s\\n' \"$d\" | grep -i shaderc"
                     sh $<TARGET_FILE:test_runtime>)
  endif()

  if(TERNI_C_API)
    # the C API, from a program written in C, through the shared library
    enable_language(C)
    add_executable(test_c_api tests/test_c_api.c)
    target_link_libraries(test_c_api PRIVATE terni_c Vulkan::Vulkan)  # a renderer's own Vulkan instance
    set_target_properties(test_c_api PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(NOT MSVC)
      target_compile_options(test_c_api PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_compile_definitions(test_c_api PRIVATE TERNI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
                                                  TERNI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                                                  TERNI_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME c_api COMMAND test_c_api)
    add_executable(test_c_api_runtime tests/test_c_api_runtime.c)
    target_link_libraries(test_c_api_runtime PRIVATE terni_c_runtime)
    target_compile_definitions(test_c_api_runtime PRIVATE TERNI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                                                          TERNI_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME c_api_runtime COMMAND test_c_api_runtime)
    set_tests_properties(c_api_runtime PROPERTIES FIXTURES_REQUIRED runtime_cache)
    if(TERNI_COMPILE_TOOL)
      # ... from a cache that terni_compile_rules filled, with a mod on top
      add_test(NAME compile_rules_tool
               COMMAND terni_compile_rules ${CMAKE_CURRENT_SOURCE_DIR}/examples/mini_galaxy
                       ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/galaxy_mods/good_mod ${CMAKE_CURRENT_BINARY_DIR}/tool_cache)
      set_tests_properties(compile_rules_tool PROPERTIES FIXTURES_SETUP tool_cache
                           PASS_REGULAR_EXPRESSION "rules: [0-9]+ compiled, [0-9]+ cached")
      add_test(NAME compile_rules_tool_errors
               COMMAND terni_compile_rules ${CMAKE_CURRENT_SOURCE_DIR}/examples/mini_galaxy
                       ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/galaxy_mods/typo_mod ${CMAKE_CURRENT_BINARY_DIR}/tool_unused)
      set_tests_properties(compile_rules_tool_errors PROPERTIES PASS_REGULAR_EXPRESSION
                           "rule .empire_income. failed to compile:.*rules/empire_income.glsl:5: error")
      add_test(NAME c_api_runtime_mod COMMAND test_c_api_runtime --mod ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/galaxy_mods/good_mod
                                              ${CMAKE_CURRENT_BINARY_DIR}/tool_cache)
      set_tests_properties(c_api_runtime_mod PROPERTIES FIXTURES_REQUIRED tool_cache)
    endif()
    # ... and the C# wrapper over it, where a .NET SDK is installed
    find_program(TERNI_DOTNET dotnet DOC ".NET SDK for the C# wrapper's test (c_api_csharp)")
    if(TERNI_DOTNET)
      add_test(NAME c_api_csharp
               COMMAND ${TERNI_DOTNET} run -c Release --project ${CMAKE_CURRENT_SOURCE_DIR}/bindings/csharp/tests
                       -- $<TARGET_FILE:terni_c> ${CMAKE_CURRENT_SOURCE_DIR})
      set_tests_properties(c_api_csharp PROPERTIES TIMEOUT 1200  # the first run restores and builds
                           ENVIRONMENT "TERNI_DOTNET_ARTIFACTS=${CMAKE_CURRENT_BINARY_DIR}/dotnet;DOTNET_CLI_TELEMETRY_OPTOUT=1;DOTNET_NOLOGO=1")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_NM)
      # ... whose libraries export the functions of terni.h and nothing else, and the
      # runtime one needs no shaderc
      add_test(NAME c_api_exports
               COMMAND sh -c "for l in \"$1\" \"$2\"; do n=$('${CMAKE_NM}' -D --defined-only \"$l\" | awk '{print $3}') || exit 1; [ \"$(printf '%s\\n' \"$n\" | grep -c '^terni_')\" -ge 90 ] || exit 1; ! printf '%s\\n' \"$n\" | grep -v '^terni_' || exit 1; done; ! '${CMAKE_READELF}' -d \"$2\" | grep -i shaderc"
                       sh $<TARGET_FILE:terni_c> $<TARGET_FILE:terni_c_runtime>)
    endif()
  endif()

  # A software Vulkan driver given by the game (add_vulkan_driver). Found where Mesa's
  # lavapipe is installed as a library; elsewhere pass -DTERNI_SOFTWARE_DRIVER=<library>.
  find_library(TERNI_SOFTWARE_DRIVER NAMES vulkan_lvp
               DOC "Software Vulkan driver library (lavapipe) for the software_driver tests")
  if(TERNI_SOFTWARE_DRIVER)
    if(TERNI_BUILD_TOOLS)
      # ... is one more device, and agrees with all the others
      add_test(NAME software_driver_parity
               COMMAND terni_run ${CMAKE_CURRENT_SOURCE_DIR}/examples/mini_galaxy --ticks 200
                       --driver ${TERNI_SOFTWARE_DRIVER})
    endif()
    if(NOT WIN32)  # the Vulkan loader ignores VK_DRIVER_FILES in elevated processes, as on Windows CI
      if(TERNI_C_API AND TERNI_SOFTWARE_DRIVER MATCHES "libvulkan_lvp\\.so$")
        # ... and the C API adds the one shipped beside a game when the system has none
        get_filename_component(_terni_driver_dir ${TERNI_SOFTWARE_DRIVER} DIRECTORY)
        add_test(NAME c_api_shipped_driver COMMAND test_c_api --shipped-driver-alone ${_terni_driver_dir})
        set_tests_properties(c_api_shipped_driver PROPERTIES
                             ENVIRONMENT "VK_DRIVER_FILES=${CMAKE_CURRENT_BINARY_DIR}/no_vulkan_drivers")
      endif()
      # ... and is enough on a machine with no Vulkan driver of its own
      terni_test(test_software_driver tests/test_software_driver.cpp)
      add_test(NAME software_driver_alone COMMAND test_software_driver ${TERNI_SOFTWARE_DRIVER})
      set_tests_properties(software_driver_alone PROPERTIES
                           ENVIRONMENT "VK_DRIVER_FILES=${CMAKE_CURRENT_BINARY_DIR}/no_vulkan_drivers")
    endif()
  endif()

  if(TERNI_BUILD_BENCHMARK)
    # the CPU reference of the benchmark stays in step with its rules
    terni_test(test_bench tests/test_bench.cpp terni_bench_world)
    add_test(NAME benchmark_world COMMAND test_bench)
    terni_test(test_soak tests/test_soak.cpp terni_bench_world)
    add_test(NAME long_run COMMAND test_soak)
  endif()

  if(TERNI_BUILD_TOOLS)
    # the example game must produce identical state on every Vulkan device
    add_test(NAME example_mini_galaxy COMMAND terni_run ${CMAKE_CURRENT_SOURCE_DIR}/examples/mini_galaxy --ticks 200)
    add_test(NAME example_hello_towns COMMAND hello_towns)
    # Optional: parse real game files, e.g.
    #   -DTERNI_SCRIPT_CORPUS="~/.steam/steam/steamapps/common/Stellaris;..."
    set(TERNI_SCRIPT_CORPUS "" CACHE STRING "Paradox game folders to parse in the script_corpus test")
    if(TERNI_SCRIPT_CORPUS)
      add_test(NAME script_corpus COMMAND terni_script_check ${TERNI_SCRIPT_CORPUS})
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Install / find_package(Terni) support
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ${TERNI_LIBRARIES} EXPORT TerniTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/terni DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# The notices a game ships beside its program: Terni-NOTICES.txt with Terni::core (the rule
# compiler's libraries), Terni-runtime-NOTICES.txt with Terni::runtime alone. TERNI_NOTICES_DIR
# names their folder, in this source tree or (from TerniConfig.cmake) where they are installed.
set(TERNI_NOTICES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/notices CACHE INTERNAL "Terni's licence notices")
install(FILES LICENSE LICENSE-EXCEPTION notices/Terni-NOTICES.txt notices/Terni-runtime-NOTICES.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/terni)
install(DIRECTORY shaders/terni DESTINATION ${CMAKE_INSTALL_DATADIR}/terni/shaders)  # for terni_add_shaders users
install(EXPORT TerniTargets NAMESPACE Terni:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Terni)
file(RELATIVE_PATH _terni_notices_from_config ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/Terni
     ${CMAKE_INSTALL_FULL_DATADIR}/licenses/terni)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/TerniConfig.cmake
"include(CMakeFindDependencyMacro)
find_dependency(Vulkan)
find_dependency(Threads)
include(\${CMAKE_CURRENT_LIST_DIR}/TerniTargets.cmake)
get_filename_component(TERNI_NOTICES_DIR \"\${CMAKE_CURRENT_LIST_DIR}/${_terni_notices_from_config}\" ABSOLUTE)
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/TerniConfig.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Terni)
