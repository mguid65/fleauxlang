cmake_minimum_required(VERSION 3.20)
project(fleaux_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
  if(MSVC)
    add_compile_options(/bigobj)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wa,-mbig-obj)
  endif()
endif()

# Keep build artifacts in predictable folders inside the active build tree.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(_cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${_cfg}" _cfg_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg_upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg_upper} "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg_upper} "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
endforeach()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(FLEAUX_BUILD_WASM_COORDINATOR "Build browser-facing Fleaux WASM coordinator" OFF)
option(FLEAUX_INSTALL_VISUAL_ASSETS "Install prebuilt fleaux-visual assets if available" OFF)
option(FLEAUX_ENABLE_ASAN "Build native targets with AddressSanitizer" OFF)
option(FLEAUX_ENABLE_TSAN "Build native targets with ThreadSanitizer" OFF)
option(FLEAUX_ENABLE_UBSAN "Build native targets with UndefinedBehaviorSanitizer" OFF)
set(FLEAUX_VISUAL_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fleaux-visual/dist" CACHE PATH "Path to prebuilt fleaux-visual dist directory")
set(FLEAUX_STD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../stdlib/Std.fleaux" CACHE FILEPATH "Canonical Std.fleaux source file to embed")

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

set(FLEAUX_SANITIZERS)
if(FLEAUX_ENABLE_ASAN)
  list(APPEND FLEAUX_SANITIZERS address)
endif()
if(FLEAUX_ENABLE_TSAN)
  list(APPEND FLEAUX_SANITIZERS thread)
endif()
if(FLEAUX_ENABLE_UBSAN)
  list(APPEND FLEAUX_SANITIZERS undefined)
endif()

if(FLEAUX_SANITIZERS)
  if(EMSCRIPTEN)
    message(FATAL_ERROR "Sanitizers are only supported for native Fleaux builds")
  endif()

  if(FLEAUX_ENABLE_TSAN AND (FLEAUX_ENABLE_ASAN OR FLEAUX_ENABLE_UBSAN))
    message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer or UndefinedBehaviorSanitizer")
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    message(FATAL_ERROR "Sanitizer options are currently supported only with GCC- and Clang-compatible compilers")
  endif()

  list(JOIN FLEAUX_SANITIZERS "," FLEAUX_SANITIZER_FLAG)
  add_compile_options(-fno-omit-frame-pointer -fsanitize=${FLEAUX_SANITIZER_FLAG})
  add_link_options(-fno-omit-frame-pointer -fsanitize=${FLEAUX_SANITIZER_FLAG})
  message(STATUS "Fleaux native sanitizers enabled: ${FLEAUX_SANITIZER_FLAG}")
endif()

if(EMSCRIPTEN)
  find_package(PCRE2 QUIET)
else()
  find_package(PCRE2 REQUIRED)
endif()

if(EMSCRIPTEN AND FLEAUX_BUILD_WASM_COORDINATOR)
  add_library(fleaux_emscripten_threads INTERFACE)
  # Every object that participates in the threaded coordinator link must be
  # compiled with Emscripten pthread support, not just the final executable.
  target_compile_options(fleaux_emscripten_threads INTERFACE -pthread)
  target_link_options(fleaux_emscripten_threads INTERFACE -pthread)
endif()

set(FLEAUX_PCRE2_TARGET "")
if(TARGET PCRE2::8BIT)
  set(FLEAUX_PCRE2_TARGET PCRE2::8BIT)
elseif(TARGET PCRE2::pcre2-8)
  set(FLEAUX_PCRE2_TARGET PCRE2::pcre2-8)
else()
  if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "PCRE2 target not found (expected PCRE2::8BIT or PCRE2::pcre2-8)")
  endif()
endif()

add_library(tl_expected INTERFACE)
set_target_properties(tl_expected PROPERTIES EXPORT_NAME tl_expected)
target_include_directories(tl_expected INTERFACE
    $<BUILD_INTERFACE:${THIRD_PARTY}/tl/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(datatree INTERFACE)
set_target_properties(datatree PROPERTIES EXPORT_NAME datatree)
target_include_directories(datatree INTERFACE
    $<BUILD_INTERFACE:${THIRD_PARTY}/datatree/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(datatree INTERFACE tl_expected)

function(fleaux_set_project_warnings project_name)
  set(options WARNINGS_AS_ERRORS)
  set(one_value_args)
  set(multi_value_args MSVC_WARNINGS CLANG_WARNINGS GCC_WARNINGS)
  cmake_parse_arguments(FLEAUX_WARN "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if("${FLEAUX_WARN_MSVC_WARNINGS}" STREQUAL "")
    set(FLEAUX_WARN_MSVC_WARNINGS
        /W4
        /w14242
        /w14254
        /w14263
        /w14265
        /w14287
        /we4289
        /w14296
        /w14311
        /w14545
        /w14546
        /w14547
        /w14549
        /w14555
        /w14619
        /w14640
        /w14826
        /w14905
        /w14906
        /w14928
        /permissive-)
  endif()

  if("${FLEAUX_WARN_CLANG_WARNINGS}" STREQUAL "")
    set(FLEAUX_WARN_CLANG_WARNINGS
        -Wall
        -Wextra
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wpedantic
        -Wconversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wno-c2y-extensions)
  endif()

  if("${FLEAUX_WARN_GCC_WARNINGS}" STREQUAL "")
    set(FLEAUX_WARN_GCC_WARNINGS
        ${FLEAUX_WARN_CLANG_WARNINGS}
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wuseless-cast
        -Wsuggest-override)
  endif()

  if(MSVC)
    set(FLEAUX_PROJECT_WARNINGS ${FLEAUX_WARN_MSVC_WARNINGS})
    set(FLEAUX_RELEASE_ERROR_FLAG /WX)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
    set(FLEAUX_PROJECT_WARNINGS ${FLEAUX_WARN_CLANG_WARNINGS})
    set(FLEAUX_RELEASE_ERROR_FLAG -Werror)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(FLEAUX_PROJECT_WARNINGS ${FLEAUX_WARN_GCC_WARNINGS})
    set(FLEAUX_RELEASE_ERROR_FLAG -Werror)
  else()
    message(AUTHOR_WARNING "No compiler warnings configured for CXX compiler: '${CMAKE_CXX_COMPILER_ID}'")
    return()
  endif()

  target_compile_options(${project_name} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:${FLEAUX_PROJECT_WARNINGS}>)

  if(FLEAUX_WARN_WARNINGS_AS_ERRORS)
    target_compile_options(${project_name} PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:${FLEAUX_RELEASE_ERROR_FLAG}>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:RelWithDebInfo>>:${FLEAUX_RELEASE_ERROR_FLAG}>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:MinSizeRel>>:${FLEAUX_RELEASE_ERROR_FLAG}>)
  endif()
endfunction()

set(FLEAUX_COMMON_SOURCES
        src/common/embedded_resource.cpp
)

add_library(fleaux_common STATIC)
set_target_properties(fleaux_common PROPERTIES EXPORT_NAME common)
target_include_directories(fleaux_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_sources(fleaux_common PRIVATE ${FLEAUX_COMMON_SOURCES})
fleaux_set_project_warnings(fleaux_common WARNINGS_AS_ERRORS)
if(TARGET fleaux_emscripten_threads)
  target_link_libraries(fleaux_common PUBLIC fleaux_emscripten_threads)
endif()

set(FLEAUX_GENERATED_RESOURCE_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_resources.cpp")
add_custom_command(
    OUTPUT ${FLEAUX_GENERATED_RESOURCE_CPP}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_embedded_resources.py
            --output ${FLEAUX_GENERATED_RESOURCE_CPP}
            --resource Std.fleaux=${FLEAUX_STD_SOURCE}
    DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_embedded_resources.py
            ${FLEAUX_STD_SOURCE}
    VERBATIM)

set(FLEAUX_FRONTEND_DIAGNOSTICS_SOURCES
        src/frontend/diagnostics.cpp
)

add_library(fleaux_frontend_diagnostics STATIC)
set_target_properties(fleaux_frontend_diagnostics PROPERTIES EXPORT_NAME frontend_diagnostics)
target_include_directories(fleaux_frontend_diagnostics PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_sources(fleaux_frontend_diagnostics PRIVATE ${FLEAUX_FRONTEND_DIAGNOSTICS_SOURCES})
target_link_libraries(fleaux_frontend_diagnostics PUBLIC tl_expected datatree)
fleaux_set_project_warnings(fleaux_frontend_diagnostics WARNINGS_AS_ERRORS)
if(TARGET fleaux_emscripten_threads)
  target_link_libraries(fleaux_frontend_diagnostics PUBLIC fleaux_emscripten_threads)
endif()

set(FLEAUX_FRONTEND_PARSER_SOURCES
        src/frontend/lexer.cpp
        src/frontend/parser.cpp
)

add_library(fleaux_frontend_parser STATIC)
set_target_properties(fleaux_frontend_parser PROPERTIES EXPORT_NAME frontend_parser)
target_include_directories(fleaux_frontend_parser PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_sources(fleaux_frontend_parser PRIVATE ${FLEAUX_FRONTEND_PARSER_SOURCES})
target_link_libraries(fleaux_frontend_parser PUBLIC fleaux_frontend_diagnostics tl_expected datatree)
fleaux_set_project_warnings(fleaux_frontend_parser WARNINGS_AS_ERRORS)
if(TARGET fleaux_emscripten_threads)
  target_link_libraries(fleaux_frontend_parser PUBLIC fleaux_emscripten_threads)
endif()

set(FLEAUX_FRONTEND_LOWERING_SOURCES
        src/frontend/lowering.cpp
        src/frontend/type_check.cpp
        src/frontend/type_system/type.cpp
        src/frontend/type_system/function_index.cpp
        src/frontend/type_system/call_shape.cpp
        src/frontend/type_system/builtin_contracts.cpp
        src/frontend/type_system/checker.cpp
        src/frontend/type_system/check_expr.cpp
        src/frontend/type_system/check_pipeline.cpp
        src/frontend/type_system/check_decl.cpp
)

add_library(fleaux_frontend_lowering STATIC)
set_target_properties(fleaux_frontend_lowering PROPERTIES EXPORT_NAME frontend_lowering)
target_include_directories(fleaux_frontend_lowering PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_sources(fleaux_frontend_lowering PRIVATE ${FLEAUX_FRONTEND_LOWERING_SOURCES})
target_link_libraries(fleaux_frontend_lowering PUBLIC fleaux_frontend_parser fleaux_frontend_diagnostics tl_expected datatree)
fleaux_set_project_warnings(fleaux_frontend_lowering WARNINGS_AS_ERRORS)
if(TARGET fleaux_emscripten_threads)
  target_link_libraries(fleaux_frontend_lowering PUBLIC fleaux_emscripten_threads)
endif()

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(fleaux_common_tests
      tests/test_main.cpp
      tests/box_tests.cpp
      tests/embedded_resource_tests.cpp
      tests/indirect_optional_tests.cpp
      tests/structured_visit_tests.cpp)
  target_link_libraries(fleaux_common_tests PRIVATE Catch2::Catch2 fleaux_common)
  fleaux_set_project_warnings(fleaux_common_tests WARNINGS_AS_ERRORS)

  catch_discover_tests(fleaux_common_tests
      TEST_PREFIX "fleaux_common_tests::"
      EXTRA_ARGS --reporter compact)

  add_executable(fleaux_core_tests
      tests/test_main.cpp
      tests/diagnostics_tests.cpp
      tests/embed_api_surface_tests.cpp
      tests/parser_tests.cpp
      tests/parser_type_syntax_tests.cpp
      tests/lowering_tests.cpp
      tests/typecheck_tests.cpp
      tests/runtime_builtins_tests.cpp
      tests/embedded_resource_integration_tests.cpp
      tests/repl_line_editor_tests.cpp
      src/cli/line_editor.cpp
      ${FLEAUX_GENERATED_RESOURCE_CPP})
  target_compile_definitions(fleaux_core_tests PRIVATE
      FLEAUX_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_link_libraries(fleaux_core_tests PRIVATE
      Catch2::Catch2
      fleaux_common
      fleaux_frontend_diagnostics
      fleaux_frontend_parser
      fleaux_frontend_lowering
      datatree)
  fleaux_set_project_warnings(fleaux_core_tests WARNINGS_AS_ERRORS)

  catch_discover_tests(fleaux_core_tests
      TEST_SPEC ~[benchmark]
      TEST_PREFIX "fleaux_core_tests::"
      EXTRA_ARGS --reporter compact)
endif()

set(FLEAUX_BYTECODE_SOURCES
        src/bytecode/compiler.cpp
        src/bytecode/escape_analyzer.cpp
        src/bytecode/optimizer.cpp
        src/bytecode/serialization.cpp
        src/bytecode/module_loader.cpp
)

add_library(fleaux_bytecode STATIC)
set_target_properties(fleaux_bytecode PROPERTIES EXPORT_NAME bytecode)
target_include_directories(fleaux_bytecode PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_sources(fleaux_bytecode PRIVATE ${FLEAUX_BYTECODE_SOURCES})
target_link_libraries(fleaux_bytecode PUBLIC fleaux_frontend_lowering tl_expected datatree)
fleaux_set_project_warnings(fleaux_bytecode WARNINGS_AS_ERRORS)
if(TARGET fleaux_emscripten_threads)
  target_link_libraries(fleaux_bytecode PUBLIC fleaux_emscripten_threads)
endif()

if(FLEAUX_PCRE2_TARGET)
  add_library(fleaux_vm STATIC)
  set_target_properties(fleaux_vm PROPERTIES EXPORT_NAME vm)
  target_include_directories(fleaux_vm
      PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/vm)

  set(FLEAUX_VM_SOURCES
          src/vm/runtime.cpp
  )

  target_sources(fleaux_vm PRIVATE ${FLEAUX_VM_SOURCES})
  target_link_libraries(fleaux_vm PUBLIC fleaux_common fleaux_bytecode tl_expected datatree ${FLEAUX_PCRE2_TARGET})
  fleaux_set_project_warnings(fleaux_vm WARNINGS_AS_ERRORS)
  if(TARGET fleaux_emscripten_threads)
    target_link_libraries(fleaux_vm PUBLIC fleaux_emscripten_threads)
  endif()

  set(FLEAUX_MAIN_SOURCES
          src/cli/vm_main.cpp src/cli/repl_driver.cpp src/cli/line_editor.cpp
          include/fleaux/runtime/builtins_parallel.hpp
          include/fleaux/runtime/builtins_math.hpp
  )

  add_executable(fleaux ${FLEAUX_MAIN_SOURCES})
  target_sources(fleaux PRIVATE ${FLEAUX_GENERATED_RESOURCE_CPP})
  target_link_libraries(fleaux PRIVATE fleaux_vm)
  fleaux_set_project_warnings(fleaux WARNINGS_AS_ERRORS)

  add_library(fleaux_embed STATIC
      src/embed/dynamic_loader.cpp
      src/embed/native_bindings.cpp
      src/embed/vm_host.cpp)
  set_target_properties(fleaux_embed PROPERTIES EXPORT_NAME embed)
  target_include_directories(fleaux_embed PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(fleaux_embed PUBLIC fleaux_vm)
  if(CMAKE_DL_LIBS)
    target_link_libraries(fleaux_embed PRIVATE ${CMAKE_DL_LIBS})
  endif()
  fleaux_set_project_warnings(fleaux_embed WARNINGS_AS_ERRORS)
  if(TARGET fleaux_emscripten_threads)
    target_link_libraries(fleaux_embed PUBLIC fleaux_emscripten_threads)
  endif()
elseif(EMSCRIPTEN)
  message(STATUS "PCRE2 not found under Emscripten; skipping fleaux_vm and fleaux targets")
endif()

if(EMSCRIPTEN AND FLEAUX_BUILD_WASM_COORDINATOR)
  set(FLEAUX_WASM_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fleaux-visual/public/wasm")
  file(MAKE_DIRECTORY "${FLEAUX_WASM_OUT_DIR}")

  set(FLEAUX_WASM_COORDINATOR_SOURCES
          ${CMAKE_CURRENT_SOURCE_DIR}/../fleaux-visual/wasm/fleaux_wasm_coordinator.cpp
  )

  add_executable(fleaux_wasm_coordinator ${FLEAUX_WASM_COORDINATOR_SOURCES})
  target_include_directories(fleaux_wasm_coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(fleaux_wasm_coordinator PRIVATE
      fleaux_frontend_parser
      fleaux_frontend_lowering
      fleaux_frontend_diagnostics
      fleaux_bytecode
      datatree
      tl_expected)
  fleaux_set_project_warnings(fleaux_wasm_coordinator WARNINGS_AS_ERRORS)
  if(TARGET fleaux_emscripten_threads)
    target_link_libraries(fleaux_wasm_coordinator PRIVATE fleaux_emscripten_threads)
  endif()

  if(TARGET fleaux_vm)
    target_sources(fleaux_wasm_coordinator PRIVATE ${FLEAUX_GENERATED_RESOURCE_CPP})
    target_link_libraries(fleaux_wasm_coordinator PRIVATE fleaux_vm)
    target_compile_definitions(fleaux_wasm_coordinator PRIVATE FLEAUX_WASM_HAS_RUNTIME=1)
  else()
    target_compile_definitions(fleaux_wasm_coordinator PRIVATE FLEAUX_WASM_HAS_RUNTIME=0)
  endif()

  target_link_options(fleaux_wasm_coordinator PRIVATE "-pthread"
      "SHELL:-sWASM=1"
      "SHELL:-sALLOW_MEMORY_GROWTH=1"
      "SHELL:-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency"
      "SHELL:-sMODULARIZE=1"
      "SHELL:-sEXPORT_ES6=1"
      "SHELL:-sENVIRONMENT=web,worker"
      "SHELL:-sNO_EXIT_RUNTIME=1"
      "SHELL:-sASSERTIONS=1"
      "SHELL:-sEXPORTED_FUNCTIONS=[\"_main\",\"_fleaux_wasm_version\",\"_fleaux_wasm_last_error\",\"_fleaux_wasm_last_output\",\"_fleaux_wasm_last_exit_code\",\"_fleaux_wasm_last_status\",\"_fleaux_wasm_parse_and_lower\",\"_fleaux_wasm_run_source\"]"
      "SHELL:-sEXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\",\"UTF8ToString\"]")
  set_target_properties(fleaux_wasm_coordinator PROPERTIES
      OUTPUT_NAME "fleaux_wasm_coordinator"
      RUNTIME_OUTPUT_DIRECTORY "${FLEAUX_WASM_OUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_DEBUG "${FLEAUX_WASM_OUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${FLEAUX_WASM_OUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${FLEAUX_WASM_OUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${FLEAUX_WASM_OUT_DIR}")
endif()

if(BUILD_TESTING AND TARGET fleaux_vm)
  add_library(fleaux_test_binding_plugin SHARED
      tests/fixtures/binding_plugin_echo.cpp)
  target_include_directories(fleaux_test_binding_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(fleaux_test_binding_plugin PRIVATE tl_expected datatree)
  fleaux_set_project_warnings(fleaux_test_binding_plugin WARNINGS_AS_ERRORS)

  add_library(fleaux_test_binding_plugin_abi_mismatch SHARED
      tests/fixtures/binding_plugin_abi_mismatch.cpp)
  target_include_directories(fleaux_test_binding_plugin_abi_mismatch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(fleaux_test_binding_plugin_abi_mismatch PRIVATE tl_expected datatree)
  fleaux_set_project_warnings(fleaux_test_binding_plugin_abi_mismatch WARNINGS_AS_ERRORS)

  add_library(fleaux_test_binding_plugin_missing_entrypoint SHARED
      tests/fixtures/binding_plugin_missing_entrypoint.cpp)
  target_include_directories(fleaux_test_binding_plugin_missing_entrypoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(fleaux_test_binding_plugin_missing_entrypoint PRIVATE tl_expected datatree)
  fleaux_set_project_warnings(fleaux_test_binding_plugin_missing_entrypoint WARNINGS_AS_ERRORS)

  add_library(fleaux_test_binding_plugin_registration_failed SHARED
      tests/fixtures/binding_plugin_registration_failed.cpp)
  target_include_directories(fleaux_test_binding_plugin_registration_failed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(fleaux_test_binding_plugin_registration_failed PRIVATE tl_expected datatree)
  fleaux_set_project_warnings(fleaux_test_binding_plugin_registration_failed WARNINGS_AS_ERRORS)

  add_library(fleaux_test_binding_plugin_partial_registration_failed SHARED
      tests/fixtures/binding_plugin_partial_registration_failed.cpp)
  target_include_directories(fleaux_test_binding_plugin_partial_registration_failed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(fleaux_test_binding_plugin_partial_registration_failed PRIVATE tl_expected datatree)
  fleaux_set_project_warnings(fleaux_test_binding_plugin_partial_registration_failed WARNINGS_AS_ERRORS)

  target_sources(fleaux_core_tests PRIVATE
      tests/bytecode_tests.cpp
      tests/embed_dynamic_loader_tests.cpp
      tests/embed_native_bindings_tests.cpp
      tests/opcode_stringify_tests.cpp
      tests/embed_vm_host_tests.cpp
      tests/vm_cli_tests.cpp
      tests/vm_import_contract_tests.cpp
      tests/vm_boundary_matrix_tests.cpp
      tests/vm_runtime_tests.cpp
      tests/vm_samples_tests.cpp)
  target_link_libraries(fleaux_core_tests PRIVATE fleaux_bytecode fleaux_vm fleaux_embed)
  target_compile_definitions(fleaux_core_tests PRIVATE
      FLEAUX_CORE_BIN_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
      FLEAUX_TEST_BINDING_PLUGIN_PATH="$<TARGET_FILE:fleaux_test_binding_plugin>"
      FLEAUX_TEST_BINDING_PLUGIN_ABI_MISMATCH_PATH="$<TARGET_FILE:fleaux_test_binding_plugin_abi_mismatch>"
      FLEAUX_TEST_BINDING_PLUGIN_MISSING_ENTRYPOINT_PATH="$<TARGET_FILE:fleaux_test_binding_plugin_missing_entrypoint>"
      FLEAUX_TEST_BINDING_PLUGIN_REGISTRATION_FAILED_PATH="$<TARGET_FILE:fleaux_test_binding_plugin_registration_failed>"
      FLEAUX_TEST_BINDING_PLUGIN_PARTIAL_REGISTRATION_FAILED_PATH="$<TARGET_FILE:fleaux_test_binding_plugin_partial_registration_failed>")
  add_dependencies(fleaux_core_tests fleaux_test_binding_plugin)
  add_dependencies(fleaux_core_tests fleaux_test_binding_plugin_abi_mismatch)
  add_dependencies(fleaux_core_tests fleaux_test_binding_plugin_missing_entrypoint)
  add_dependencies(fleaux_core_tests fleaux_test_binding_plugin_registration_failed)
  add_dependencies(fleaux_core_tests fleaux_test_binding_plugin_partial_registration_failed)
  add_dependencies(fleaux_core_tests fleaux)

endif()

set(FLEAUX_INSTALL_TARGETS)
set(FLEAUX_DEVELOPMENT_TARGETS tl_expected datatree fleaux_common fleaux_frontend_diagnostics fleaux_frontend_parser fleaux_frontend_lowering fleaux_bytecode)

if(TARGET fleaux)
  list(APPEND FLEAUX_INSTALL_TARGETS fleaux)
endif()

if(TARGET fleaux_vm)
  list(APPEND FLEAUX_DEVELOPMENT_TARGETS fleaux_vm)
endif()

if(TARGET fleaux_embed)
  list(APPEND FLEAUX_DEVELOPMENT_TARGETS fleaux_embed)
endif()

if(FLEAUX_INSTALL_TARGETS)
  install(TARGETS ${FLEAUX_INSTALL_TARGETS}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT Runtime)
endif()

if(FLEAUX_DEVELOPMENT_TARGETS)
  install(TARGETS ${FLEAUX_DEVELOPMENT_TARGETS}
      EXPORT fleaux-targets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      COMPONENT Development)
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Development)

install(DIRECTORY ${THIRD_PARTY}/tl/include/tl/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tl
    COMPONENT Development)

install(DIRECTORY ${THIRD_PARTY}/datatree/include/data_tree/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/data_tree
    COMPONENT Development)

if(FLEAUX_DEVELOPMENT_TARGETS)
  write_basic_package_version_file(
      "${CMAKE_CURRENT_BINARY_DIR}/fleaux-config-version.cmake"
      VERSION ${PROJECT_VERSION}
      COMPATIBILITY SameMajorVersion)

  configure_package_config_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fleaux-config.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/fleaux-config.cmake"
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fleaux)

  install(EXPORT fleaux-targets
      FILE fleaux-targets.cmake
      NAMESPACE fleaux::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fleaux
      COMPONENT Development)

  install(FILES
      "${CMAKE_CURRENT_BINARY_DIR}/fleaux-config.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/fleaux-config-version.cmake"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fleaux
      COMPONENT Development)
endif()

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../stdlib/Std.fleaux
    DESTINATION ${CMAKE_INSTALL_DATADIR}/fleaux
    COMPONENT Runtime)

file(GLOB FLEAUX_SAMPLE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../samples/*.fleaux")
install(FILES ${FLEAUX_SAMPLE_FILES}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/fleaux/samples
    COMPONENT Runtime)

if(FLEAUX_INSTALL_VISUAL_ASSETS)
  if(EXISTS "${FLEAUX_VISUAL_DIST_DIR}")
    install(DIRECTORY ${FLEAUX_VISUAL_DIST_DIR}/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fleaux/visual
        COMPONENT Visual)
  else()
    message(WARNING "FLEAUX_INSTALL_VISUAL_ASSETS is ON, but no dist directory exists at: ${FLEAUX_VISUAL_DIST_DIR}")
  endif()
endif()

set(CPACK_PACKAGE_NAME "fleaux")
set(CPACK_PACKAGE_VENDOR "Fleaux")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fleaux language tools and runtime")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_GENERATOR "TGZ")
set(CPACK_COMPONENTS_ALL Runtime Development)
if(FLEAUX_INSTALL_VISUAL_ASSETS)
  list(APPEND CPACK_COMPONENTS_ALL Visual)
endif()

include(CPack)

