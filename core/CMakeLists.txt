cmake_minimum_required(VERSION 3.20)
project(fleaux_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
target_include_directories(tl_expected INTERFACE ${THIRD_PARTY}/tl/include)

add_library(datatree INTERFACE)
target_include_directories(datatree INTERFACE ${THIRD_PARTY}/datatree/include)
target_link_libraries(datatree INTERFACE tl_expected)

set(FLEAUX_COMMON_SOURCES
        src/common/embedded_resource.cpp
)

add_library(fleaux_common STATIC)
target_include_directories(fleaux_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_common PRIVATE ${FLEAUX_COMMON_SOURCES})
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
target_include_directories(fleaux_frontend_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_frontend_diagnostics PRIVATE ${FLEAUX_FRONTEND_DIAGNOSTICS_SOURCES})
target_link_libraries(fleaux_frontend_diagnostics PUBLIC tl_expected datatree)
if(TARGET fleaux_emscripten_threads)
  target_link_libraries(fleaux_frontend_diagnostics PUBLIC fleaux_emscripten_threads)
endif()

set(FLEAUX_FRONTEND_PARSER_SOURCES
        src/frontend/parser.cpp
)

add_library(fleaux_frontend_parser STATIC)
target_include_directories(fleaux_frontend_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_frontend_parser PRIVATE ${FLEAUX_FRONTEND_PARSER_SOURCES})
target_link_libraries(fleaux_frontend_parser PUBLIC fleaux_frontend_diagnostics tl_expected datatree)
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
target_include_directories(fleaux_frontend_lowering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_frontend_lowering PRIVATE ${FLEAUX_FRONTEND_LOWERING_SOURCES})
target_link_libraries(fleaux_frontend_lowering PUBLIC fleaux_frontend_parser fleaux_frontend_diagnostics tl_expected datatree)
if(TARGET fleaux_emscripten_threads)
  target_link_libraries(fleaux_frontend_lowering PUBLIC fleaux_emscripten_threads)
endif()

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(fleaux_common_tests
      tests/test_main.cpp
      tests/embedded_resource_tests.cpp)
  target_link_libraries(fleaux_common_tests PRIVATE Catch2::Catch2 fleaux_common)

  catch_discover_tests(fleaux_common_tests
      TEST_PREFIX "fleaux_common_tests::"
      EXTRA_ARGS --reporter compact)

  add_executable(fleaux_core_tests
      tests/test_main.cpp
      tests/diagnostics_tests.cpp
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
target_include_directories(fleaux_bytecode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_bytecode PRIVATE ${FLEAUX_BYTECODE_SOURCES})
target_link_libraries(fleaux_bytecode PUBLIC fleaux_frontend_lowering tl_expected datatree)
if(TARGET fleaux_emscripten_threads)
  target_link_libraries(fleaux_bytecode PUBLIC fleaux_emscripten_threads)
endif()

if(FLEAUX_PCRE2_TARGET)
  add_library(fleaux_vm STATIC)
  target_include_directories(fleaux_vm
      PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/vm)

  set(FLEAUX_VM_SOURCES
          src/vm/runtime.cpp
  )

  target_sources(fleaux_vm PRIVATE ${FLEAUX_VM_SOURCES})
  target_link_libraries(fleaux_vm PUBLIC fleaux_common fleaux_bytecode tl_expected datatree ${FLEAUX_PCRE2_TARGET})
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
  target_sources(fleaux_core_tests PRIVATE
      tests/bytecode_tests.cpp
      tests/opcode_stringify_tests.cpp
      tests/vm_cli_tests.cpp
      tests/vm_import_contract_tests.cpp
      tests/vm_boundary_matrix_tests.cpp
      tests/vm_runtime_tests.cpp
      tests/vm_samples_tests.cpp)
  target_link_libraries(fleaux_core_tests PRIVATE fleaux_bytecode fleaux_vm)
  target_compile_definitions(fleaux_core_tests PRIVATE
      FLEAUX_CORE_BIN_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  add_dependencies(fleaux_core_tests fleaux)

endif()

set(FLEAUX_INSTALL_TARGETS)

if(TARGET fleaux)
  list(APPEND FLEAUX_INSTALL_TARGETS fleaux)
endif()

if(FLEAUX_INSTALL_TARGETS)
  install(TARGETS ${FLEAUX_INSTALL_TARGETS}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT Runtime)
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
set(CPACK_COMPONENTS_ALL Runtime)
if(FLEAUX_INSTALL_VISUAL_ASSETS)
  list(APPEND CPACK_COMPONENTS_ALL Visual)
endif()

include(CPack)

