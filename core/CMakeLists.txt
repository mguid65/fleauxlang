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

option(FLEAUX_BUILD_WASM_COORDINATOR "Build browser-facing Fleaux WASM coordinator" OFF)
option(FLEAUX_INSTALL_VISUAL_ASSETS "Install prebuilt fleaux-visual assets if available" OFF)
set(FLEAUX_VISUAL_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fleaux-visual/dist" CACHE PATH "Path to prebuilt fleaux-visual dist directory")

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

if(EMSCRIPTEN)
  find_package(PCRE2 QUIET)
else()
  find_package(PCRE2 REQUIRED)
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

add_library(fleaux_frontend_diagnostics STATIC)
target_include_directories(fleaux_frontend_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_frontend_diagnostics PRIVATE src/frontend/diagnostics.cpp)
target_link_libraries(fleaux_frontend_diagnostics PUBLIC tl_expected)

add_library(fleaux_frontend_parser STATIC)
target_include_directories(fleaux_frontend_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_frontend_parser PRIVATE src/frontend/parser.cpp)
target_link_libraries(fleaux_frontend_parser PUBLIC fleaux_frontend_diagnostics tl_expected)

add_library(fleaux_frontend_lowering STATIC)
target_include_directories(fleaux_frontend_lowering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_frontend_lowering PRIVATE src/frontend/lowering.cpp src/frontend/type_check.cpp)
target_link_libraries(fleaux_frontend_lowering PUBLIC fleaux_frontend_parser fleaux_frontend_diagnostics tl_expected)

add_library(fleaux_frontend_cpp_transpiler STATIC)
target_include_directories(fleaux_frontend_cpp_transpiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_frontend_cpp_transpiler PRIVATE src/frontend/cpp_transpiler.cpp)
target_link_libraries(fleaux_frontend_cpp_transpiler PUBLIC fleaux_frontend_lowering fleaux_frontend_parser fleaux_frontend_diagnostics tl_expected)

add_executable(fleaux2cpp src/cli/main.cpp)
target_link_libraries(fleaux2cpp PRIVATE fleaux_frontend_cpp_transpiler)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)

  add_executable(fleaux_core_tests
      tests/diagnostics_tests.cpp
      tests/parser_tests.cpp
      tests/lowering_tests.cpp
      tests/transpiler_tests.cpp
      tests/runtime_builtins_tests.cpp)
  target_compile_definitions(fleaux_core_tests PRIVATE
      FLEAUX_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_link_libraries(fleaux_core_tests PRIVATE
      Catch2::Catch2WithMain
      fleaux_frontend_diagnostics
      fleaux_frontend_parser
      fleaux_frontend_lowering
      fleaux_frontend_cpp_transpiler
      datatree)

  add_test(NAME fleaux_core_tests COMMAND fleaux_core_tests)
endif()

add_library(fleaux_bytecode STATIC)
target_include_directories(fleaux_bytecode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_bytecode PRIVATE src/bytecode/compiler.cpp)
target_link_libraries(fleaux_bytecode PUBLIC fleaux_frontend_lowering tl_expected)

if(FLEAUX_PCRE2_TARGET)
  add_library(fleaux_vm STATIC)
  target_include_directories(fleaux_vm
      PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/vm)
  target_sources(fleaux_vm PRIVATE src/vm/runtime.cpp)
  target_link_libraries(fleaux_vm PUBLIC fleaux_bytecode tl_expected datatree ${FLEAUX_PCRE2_TARGET})

  add_library(fleaux_vm_interpreter STATIC)
  target_include_directories(fleaux_vm_interpreter
      PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_sources(fleaux_vm_interpreter PRIVATE src/vm/interpreter.cpp)
  target_link_libraries(fleaux_vm_interpreter PUBLIC
      fleaux_frontend_parser
      fleaux_frontend_lowering
      fleaux_frontend_diagnostics
      tl_expected
      datatree
      ${FLEAUX_PCRE2_TARGET})

  add_executable(fleaux src/cli/vm_main.cpp)
  target_link_libraries(fleaux PRIVATE fleaux_vm_interpreter fleaux_vm)
elseif(EMSCRIPTEN)
  message(STATUS "PCRE2 not found under Emscripten; skipping fleaux_vm, fleaux_vm_interpreter, and fleaux targets")
endif()

if(EMSCRIPTEN AND FLEAUX_BUILD_WASM_COORDINATOR)
  set(FLEAUX_WASM_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fleaux-visual/public/wasm")
  file(MAKE_DIRECTORY "${FLEAUX_WASM_OUT_DIR}")

  add_executable(fleaux_wasm_coordinator
      ${CMAKE_CURRENT_SOURCE_DIR}/../fleaux-visual/wasm/fleaux_wasm_coordinator.cpp)
  target_include_directories(fleaux_wasm_coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(fleaux_wasm_coordinator PRIVATE
      fleaux_frontend_parser
      fleaux_frontend_lowering
      fleaux_frontend_diagnostics
      fleaux_bytecode
      datatree
      tl_expected)

  if(TARGET fleaux_vm)
    target_link_libraries(fleaux_wasm_coordinator PRIVATE fleaux_vm)
    target_compile_definitions(fleaux_wasm_coordinator PRIVATE FLEAUX_WASM_HAS_RUNTIME=1)
  else()
    target_compile_definitions(fleaux_wasm_coordinator PRIVATE FLEAUX_WASM_HAS_RUNTIME=0)
  endif()

  target_link_options(fleaux_wasm_coordinator PRIVATE
      "SHELL:-sWASM=1"
      "SHELL:-sALLOW_MEMORY_GROWTH=1"
      "SHELL:-sMODULARIZE=1"
      "SHELL:-sEXPORT_ES6=1"
      "SHELL:-sENVIRONMENT=web,worker"
      "SHELL:-sNO_EXIT_RUNTIME=1"
      "SHELL:-sEXPORTED_FUNCTIONS=[\"_main\",\"_fleaux_wasm_version\",\"_fleaux_wasm_last_error\",\"_fleaux_wasm_last_output\",\"_fleaux_wasm_last_exit_code\",\"_fleaux_wasm_parse_and_lower\",\"_fleaux_wasm_run_source\"]"
      "SHELL:-sEXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\",\"UTF8ToString\"]")
  set_target_properties(fleaux_wasm_coordinator PROPERTIES
      OUTPUT_NAME "fleaux_wasm_coordinator"
      RUNTIME_OUTPUT_DIRECTORY "${FLEAUX_WASM_OUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_DEBUG "${FLEAUX_WASM_OUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${FLEAUX_WASM_OUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${FLEAUX_WASM_OUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${FLEAUX_WASM_OUT_DIR}")
endif()

if(BUILD_TESTING AND TARGET fleaux_vm AND TARGET fleaux_vm_interpreter)
  target_sources(fleaux_core_tests PRIVATE
      tests/bytecode_tests.cpp
      tests/vm_runtime_tests.cpp
      tests/vm_samples_tests.cpp)
  target_link_libraries(fleaux_core_tests PRIVATE fleaux_bytecode fleaux_vm fleaux_vm_interpreter)

  # Fast CI target: only the interpreter↔bytecode parity subset.
  # Run with:  ctest -R fleaux_vm_parity_tests  (or cmake --build . --target test)
  add_test(NAME fleaux_vm_parity_tests
           COMMAND fleaux_core_tests "[vm][samples][parity]")

  # Fast CI target: native jump opcode tests.
  add_test(NAME fleaux_vm_jump_tests
           COMMAND fleaux_core_tests "[vm][jump]")
endif()

set(FLEAUX_INSTALL_TARGETS fleaux2cpp)

if(TARGET fleaux)
  list(APPEND FLEAUX_INSTALL_TARGETS fleaux)
endif()

install(TARGETS ${FLEAUX_INSTALL_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT Runtime)

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../Std.fleaux
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

