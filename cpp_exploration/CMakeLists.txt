cmake_minimum_required(VERSION 3.20)
project(FlexLang CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Add fleaux runtime library (header-only)
add_library(fleaux_runtime INTERFACE)
target_include_directories(fleaux_runtime INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Function to add a Fleaux target
function(add_fleaux_target target_name fleaux_file)
    # Generate the C++20 files using the Python transpiler
    add_custom_command(
        OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/fleaux_generated_${target_name}.hpp
            ${CMAKE_CURRENT_BINARY_DIR}/fleaux_generated_${target_name}.cpp
        COMMAND python3 fleaux_cpp_transpiler.py ${fleaux_file}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS ${fleaux_file}
        COMMENT "Transpiling Fleaux: ${fleaux_file}"
    )

    # Create executable or library
    add_executable(${target_name}
        ${CMAKE_CURRENT_BINARY_DIR}/fleaux_generated_${target_name}.cpp
    )
    target_link_libraries(${target_name} PRIVATE fleaux_runtime)
    target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endfunction()

# Example targets
add_fleaux_target(test test.fleaux)

