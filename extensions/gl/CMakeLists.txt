# The OpenGL extension, built with the core when FLEAUX_BUILD_GL_EXTENSION is on. Its library and GL.fleaux go to one
# directory, which 'fleaux --extension' takes.

find_package(glfw3 CONFIG)
find_package(glad CONFIG)
if(NOT glfw3_FOUND OR NOT glad_FOUND)
  message(FATAL_ERROR
      "The OpenGL extension needs GLFW and glad from Conan. Give Conan the option '-o &:build_gl_extension=True', "
      "for example with -DCONAN_INSTALL_ARGS=\"--build=missing;-o=&:build_gl_extension=True\".")
endif()

set(FLEAUX_GL_EXTENSION_DIR "${CMAKE_CURRENT_BINARY_DIR}/fleaux_gl")

add_library(fleaux_gl SHARED src/fleaux_gl.cpp)
target_include_directories(fleaux_gl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include")
target_link_libraries(fleaux_gl PRIVATE fleaux_heap tl_expected glfw glad::glad)
fleaux_set_project_warnings(fleaux_gl WARNINGS_AS_ERRORS)

# One library in the directory, so that the directory alone names the extension. The import library goes elsewhere.
set_target_properties(fleaux_gl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${FLEAUX_GL_EXTENSION_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${FLEAUX_GL_EXTENSION_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(fleaux_gl PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${FLEAUX_GL_EXTENSION_DIR}"
      LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${FLEAUX_GL_EXTENSION_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_CURRENT_BINARY_DIR}/lib")
endforeach()

add_custom_command(TARGET fleaux_gl POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/GL.fleaux"
            "${FLEAUX_GL_EXTENSION_DIR}/GL.fleaux"
    COMMENT "Copying GL.fleaux next to the OpenGL extension")
