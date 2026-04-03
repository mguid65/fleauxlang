cmake_minimum_required(VERSION 3.20)
project(fleaux_std_builtins_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# Header-only interface libraries for vendored dependencies
add_library(tl_expected INTERFACE)
target_include_directories(tl_expected INTERFACE ${THIRD_PARTY}/tl/include)

add_library(datatree INTERFACE)
target_include_directories(datatree INTERFACE ${THIRD_PARTY}/datatree/include)
target_link_libraries(datatree INTERFACE tl_expected)

add_library(fleaux_runtime INTERFACE)
target_include_directories(fleaux_runtime INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/)

# Runtime test (datatree-based)
add_executable(test_runtime test_runtime.cpp)
target_include_directories(test_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_runtime PRIVATE datatree)

