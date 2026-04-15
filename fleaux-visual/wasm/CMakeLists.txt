cmake_minimum_required(VERSION 3.20)
project(fleaux_wasm_standalone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Get the path to the core directory
set(FLEAUX_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../core")
set(FLEAUX_VISUAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(THIRD_PARTY "${FLEAUX_CORE_DIR}/../third_party")

# Include the main core CMakeLists.txt
include("${FLEAUX_CORE_DIR}/CMakeLists.txt")

# This CMakeLists.txt is primarily for IDE support when working
# directly in the fleaux-visual/wasm directory.
# For actual builds, use the core/CMakeLists.txt with the FLEAUX_BUILD_WASM_COORDINATOR option.

