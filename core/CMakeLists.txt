cmake_minimum_required(VERSION 3.20)
project(fleaux_core LANGUAGES CXX)

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

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

find_package(PCRE2)

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
target_sources(fleaux_frontend_lowering PRIVATE src/frontend/lowering.cpp)
target_link_libraries(fleaux_frontend_lowering PUBLIC fleaux_frontend_parser fleaux_frontend_diagnostics tl_expected)

add_library(fleaux_frontend_cpp_transpiler STATIC)
target_include_directories(fleaux_frontend_cpp_transpiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fleaux_frontend_cpp_transpiler PRIVATE src/frontend/cpp_transpiler.cpp)
target_link_libraries(fleaux_frontend_cpp_transpiler PUBLIC fleaux_frontend_lowering fleaux_frontend_parser fleaux_frontend_diagnostics tl_expected)

add_executable(fleaux_transpile_cli src/cli/main.cpp)
target_link_libraries(fleaux_transpile_cli PRIVATE fleaux_frontend_cpp_transpiler)

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

add_library(fleaux_vm STATIC)
target_include_directories(fleaux_vm
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/vm)
target_sources(fleaux_vm PRIVATE src/vm/runtime.cpp)
target_link_libraries(fleaux_vm PUBLIC fleaux_bytecode tl_expected datatree)

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
    datatree)

add_executable(fleaux_vm_cli src/cli/vm_main.cpp)
target_link_libraries(fleaux_vm_cli PRIVATE fleaux_vm_interpreter fleaux_vm)

if(BUILD_TESTING)
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
