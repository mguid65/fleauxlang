// The OpenGL extension: OpenGL 3.3 core, with a window and input from GLFW and functions loaded by glad. Each binding
// implements one '__native__' declaration in GL.fleaux and receives that declaration's parameters as one tuple. The
// type checker has already checked the parameters, so the bindings read them without validating their kinds.

#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "fleaux/embed/fleaux_binding.hpp"
#include "fleaux/runtime/value.hpp"

namespace {

namespace rt = fleaux::runtime;
using fleaux::embed::BindingContext;
using fleaux::embed::NativeInvokeResult;
using fleaux::embed::VmValue;

// One window, and with it the one OpenGL context every other binding uses.
struct GlState {
  GLFWwindow* window{nullptr};
  bool glfw_initialized{false};
};

auto gl_state() -> GlState& {
  static GlState state;
  return state;
}

auto fail(std::string message) -> NativeInvokeResult {
  return tl::unexpected(fleaux::embed::HostError{
      .category = fleaux::embed::HostErrorCategory::kBinding,
      .message = std::move(message),
  });
}

auto arg(const VmValue& args, const std::size_t index) -> const VmValue& { return rt::tuple_at(args, index); }

auto int_arg(const VmValue& args, const std::size_t index) -> std::int64_t { return rt::int64_value(arg(args, index)); }

auto float_arg(const VmValue& args, const std::size_t index) -> double { return rt::to_double(arg(args, index)); }

auto string_arg(const VmValue& args, const std::size_t index) -> const std::string& {
  return rt::as_string(arg(args, index));
}

// GL object names and enums are unsigned 32-bit in OpenGL and Int64 in Fleaux.
auto gl_uint(const std::int64_t value) -> GLuint { return static_cast<GLuint>(value); }
auto gl_enum(const std::int64_t value) -> GLenum { return static_cast<GLenum>(value); }
auto gl_int(const std::int64_t value) -> GLint { return static_cast<GLint>(value); }
auto gl_sizei(const std::int64_t value) -> GLsizei { return static_cast<GLsizei>(value); }

template <typename T>
auto tuple_items(const VmValue& tuple, T (*convert)(const VmValue&)) -> std::vector<T> {
  const auto& items = rt::as_tuple(tuple);
  std::vector<T> converted;
  converted.reserve(items.size());
  for (const VmValue& item : items) {
    converted.push_back(convert(item));
  }
  return converted;
}

auto to_float(const VmValue& value) -> float { return static_cast<float>(rt::to_double(value)); }

auto to_index(const VmValue& value) -> GLuint { return static_cast<GLuint>(rt::int64_value(value)); }

// Wraps a binding that needs the window and its context.
template <typename F>
auto with_window(const char* name, F&& body) -> NativeInvokeResult {
  if (gl_state().window == nullptr) {
    return fail(std::format("{}: no window is open. Call GL.OpenWindow first.", name));
  }
  return std::forward<F>(body)();
}

auto null_result() -> NativeInvokeResult { return rt::make_null(); }

// A Result value is a (Bool tag, payload) tuple, as Std.Result builds it. The VM checks a native function's result
// against its declared Result type, so a change to that layout shows up as a type error rather than bad data.
auto result_ok(VmValue value) -> VmValue { return rt::make_tuple(rt::make_bool(true), std::move(value)); }
auto result_err(std::string message) -> VmValue {
  return rt::make_tuple(rt::make_bool(false), rt::make_string(std::move(message)));
}

void framebuffer_resized(GLFWwindow* /*window*/, const int width, const int height) {
  glViewport(0, 0, width, height);
}

// glad's loader takes object pointers; GLFW returns function pointers of the same size.
auto load_gl_function(const char* name) -> void* { return std::bit_cast<void*>(glfwGetProcAddress(name)); }

auto glfw_error() -> std::string {
  const char* description = nullptr;
  glfwGetError(&description);
  return description != nullptr ? std::string{description} : std::string{"unknown GLFW error"};
}

// Window and frame loop

auto open_window(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  GlState& state = gl_state();
  if (state.window != nullptr) {
    return fail("GL.OpenWindow: a window is already open. Call GL.CloseWindow first.");
  }
  if (!state.glfw_initialized) {
    if (glfwInit() != GLFW_TRUE) {
      return fail("GL.OpenWindow: cannot initialize GLFW: " + glfw_error());
    }
    state.glfw_initialized = true;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  GLFWwindow* window =
      glfwCreateWindow(static_cast<int>(int_arg(args, 1)), static_cast<int>(int_arg(args, 2)),
                       string_arg(args, 0).c_str(), nullptr, nullptr);
  if (window == nullptr) {
    return fail("GL.OpenWindow: cannot create an OpenGL 3.3 core window: " + glfw_error());
  }

  glfwMakeContextCurrent(window);
  if (gladLoadGLLoader(load_gl_function) == 0) {
    glfwDestroyWindow(window);
    return fail("GL.OpenWindow: cannot load the OpenGL functions.");
  }
  glfwSwapInterval(1);
  glfwSetFramebufferSizeCallback(window, framebuffer_resized);
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window, &width, &height);
  glViewport(0, 0, width, height);
  state.window = window;
  return null_result();
}

auto close_window(const BindingContext&, const VmValue&) -> NativeInvokeResult {
  GlState& state = gl_state();
  if (state.window != nullptr) {
    glfwDestroyWindow(state.window);
    state.window = nullptr;
  }
  if (state.glfw_initialized) {
    glfwTerminate();
    state.glfw_initialized = false;
  }
  return null_result();
}

auto window_should_close(const BindingContext&, const VmValue&) -> NativeInvokeResult {
  return with_window("GL.WindowShouldClose",
                     [] { return rt::make_bool(glfwWindowShouldClose(gl_state().window) == GLFW_TRUE); });
}

auto poll_events(const BindingContext&, const VmValue&) -> NativeInvokeResult {
  return with_window("GL.PollEvents", [] {
    glfwPollEvents();
    return null_result();
  });
}

auto swap_buffers(const BindingContext&, const VmValue&) -> NativeInvokeResult {
  return with_window("GL.SwapBuffers", [] {
    glfwSwapBuffers(gl_state().window);
    return null_result();
  });
}

auto time(const BindingContext&, const VmValue&) -> NativeInvokeResult { return rt::make_float(glfwGetTime()); }

auto key_down(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.KeyDown", [&] {
    return rt::make_bool(glfwGetKey(gl_state().window, static_cast<int>(int_arg(args, 0))) == GLFW_PRESS);
  });
}

auto framebuffer_size(const BindingContext&, const VmValue&) -> NativeInvokeResult {
  return with_window("GL.FramebufferSize", [] {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(gl_state().window, &width, &height);
    return rt::make_tuple(rt::make_int(width), rt::make_int(height));
  });
}

// The cursor over the window, each coordinate scaled to 0..1 from the top left corner, so that it does not depend on
// the window's size or the display's scaling.
auto cursor_position(const BindingContext&, const VmValue&) -> NativeInvokeResult {
  return with_window("GL.CursorPosition", [] {
    double x = 0.0;
    double y = 0.0;
    int width = 0;
    int height = 0;
    glfwGetCursorPos(gl_state().window, &x, &y);
    glfwGetWindowSize(gl_state().window, &width, &height);
    const double scale_x = width > 0 ? 1.0 / static_cast<double>(width) : 0.0;
    const double scale_y = height > 0 ? 1.0 / static_cast<double>(height) : 0.0;
    return rt::make_tuple(rt::make_float(x * scale_x), rt::make_float(y * scale_y));
  });
}

auto mouse_button_down(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.MouseButtonDown", [&] {
    return rt::make_bool(glfwGetMouseButton(gl_state().window, static_cast<int>(int_arg(args, 0))) == GLFW_PRESS);
  });
}

// State and clearing

auto viewport(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.Viewport", [&] {
    glViewport(gl_int(int_arg(args, 0)), gl_int(int_arg(args, 1)), gl_sizei(int_arg(args, 2)),
               gl_sizei(int_arg(args, 3)));
    return null_result();
  });
}

auto clear_color(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.ClearColor", [&] {
    glClearColor(static_cast<GLfloat>(float_arg(args, 0)), static_cast<GLfloat>(float_arg(args, 1)),
                 static_cast<GLfloat>(float_arg(args, 2)), static_cast<GLfloat>(float_arg(args, 3)));
    return null_result();
  });
}

auto clear(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.Clear", [&] {
    glClear(static_cast<GLbitfield>(int_arg(args, 0)));
    return null_result();
  });
}

auto enable(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.Enable", [&] {
    glEnable(gl_enum(int_arg(args, 0)));
    return null_result();
  });
}

auto disable(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.Disable", [&] {
    glDisable(gl_enum(int_arg(args, 0)));
    return null_result();
  });
}

// Shaders

auto shader_log(const GLuint shader) -> std::string {
  GLint length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
  std::string log(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
  if (length > 0) {
    glGetShaderInfoLog(shader, length, nullptr, log.data());
  }
  return log;
}

auto program_log(const GLuint program) -> std::string {
  GLint length = 0;
  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
  std::string log(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
  if (length > 0) {
    glGetProgramInfoLog(program, length, nullptr, log.data());
  }
  return log;
}

auto compile_shader(const GLenum kind, const std::string& source) -> tl::expected<GLuint, std::string> {
  const GLuint shader = glCreateShader(kind);
  const char* text = source.c_str();
  glShaderSource(shader, 1, &text, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != GL_TRUE) {
    std::string log = shader_log(shader);
    glDeleteShader(shader);
    return tl::unexpected(std::move(log));
  }
  return shader;
}

auto compile_program(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.CompileProgram", [&]() -> NativeInvokeResult {
    const auto vertex = compile_shader(GL_VERTEX_SHADER, string_arg(args, 0));
    if (!vertex) {
      return result_err("vertex shader: " + vertex.error());
    }
    const auto fragment = compile_shader(GL_FRAGMENT_SHADER, string_arg(args, 1));
    if (!fragment) {
      glDeleteShader(*vertex);
      return result_err("fragment shader: " + fragment.error());
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, *vertex);
    glAttachShader(program, *fragment);
    glLinkProgram(program);
    glDeleteShader(*vertex);
    glDeleteShader(*fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
      std::string log = program_log(program);
      glDeleteProgram(program);
      return result_err("link: " + log);
    }
    return result_ok(rt::make_int(static_cast<rt::Int>(program)));
  });
}

auto use_program(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.UseProgram", [&] {
    glUseProgram(gl_uint(int_arg(args, 0)));
    return null_result();
  });
}

auto uniform_location(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.UniformLocation", [&] {
    return rt::make_int(glGetUniformLocation(gl_uint(int_arg(args, 0)), string_arg(args, 1).c_str()));
  });
}

auto uniform1f(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.Uniform1f", [&] {
    glUniform1f(gl_int(int_arg(args, 0)), static_cast<GLfloat>(float_arg(args, 1)));
    return null_result();
  });
}

auto uniform3f(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.Uniform3f", [&] {
    glUniform3f(gl_int(int_arg(args, 0)), static_cast<GLfloat>(float_arg(args, 1)),
                static_cast<GLfloat>(float_arg(args, 2)), static_cast<GLfloat>(float_arg(args, 3)));
    return null_result();
  });
}

auto uniform4f(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.Uniform4f", [&] {
    glUniform4f(gl_int(int_arg(args, 0)), static_cast<GLfloat>(float_arg(args, 1)),
                static_cast<GLfloat>(float_arg(args, 2)), static_cast<GLfloat>(float_arg(args, 3)),
                static_cast<GLfloat>(float_arg(args, 4)));
    return null_result();
  });
}

auto uniform_matrix4(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.UniformMatrix4", [&]() -> NativeInvokeResult {
    const auto values = tuple_items(arg(args, 1), to_float);
    if (values.size() != 16U) {
      return fail(std::format("GL.UniformMatrix4: a 4x4 matrix has 16 values, got {}.", values.size()));
    }
    glUniformMatrix4fv(gl_int(int_arg(args, 0)), 1, GL_FALSE, values.data());
    return null_result();
  });
}

// Buffers and vertex arrays

auto create_vertex_array(const BindingContext&, const VmValue&) -> NativeInvokeResult {
  return with_window("GL.CreateVertexArray", [] {
    GLuint vertex_array = 0;
    glGenVertexArrays(1, &vertex_array);
    glBindVertexArray(vertex_array);
    return rt::make_int(static_cast<rt::Int>(vertex_array));
  });
}

auto bind_vertex_array(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.BindVertexArray", [&] {
    glBindVertexArray(gl_uint(int_arg(args, 0)));
    return null_result();
  });
}

auto create_buffer(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.CreateBuffer", [&] {
    const auto data = tuple_items(arg(args, 0), to_float);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
    return rt::make_int(static_cast<rt::Int>(buffer));
  });
}

auto create_index_buffer(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.CreateIndexBuffer", [&] {
    const auto indices = tuple_items(arg(args, 0), to_index);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(GLuint)), indices.data(),
                 GL_STATIC_DRAW);
    return rt::make_int(static_cast<rt::Int>(buffer));
  });
}

auto vertex_attribute(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.VertexAttribute", [&] {
    const GLuint index = gl_uint(int_arg(args, 0));
    const auto stride = static_cast<std::size_t>(int_arg(args, 2)) * sizeof(float);
    const auto offset = static_cast<std::size_t>(int_arg(args, 3)) * sizeof(float);
    glVertexAttribPointer(index, gl_int(int_arg(args, 1)), GL_FLOAT, GL_FALSE, static_cast<GLsizei>(stride),
                          reinterpret_cast<const void*>(offset));
    glEnableVertexAttribArray(index);
    return null_result();
  });
}

auto draw_arrays(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.DrawArrays", [&] {
    glDrawArrays(gl_enum(int_arg(args, 0)), gl_int(int_arg(args, 1)), gl_sizei(int_arg(args, 2)));
    return null_result();
  });
}

// Textures

// A width x height texture of one 8-bit channel, read without filtering, so that each texel stays one flat cell.
auto create_texture(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.CreateTexture", [&] {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, gl_sizei(int_arg(args, 0)), gl_sizei(int_arg(args, 1)), 0, GL_RED,
                 GL_UNSIGNED_BYTE, nullptr);
    return rt::make_int(static_cast<rt::Int>(texture));
  });
}

auto update_texture(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.UpdateTexture", [&]() -> NativeInvokeResult {
    const std::int64_t width = int_arg(args, 1);
    const std::int64_t height = int_arg(args, 2);
    const auto& values = rt::as_tuple(arg(args, 3));
    if (width < 0 || height < 0 || values.size() != static_cast<std::size_t>(width * height)) {
      return fail(std::format("GL.UpdateTexture: a {} x {} texture takes {} values, but {} were given", width, height,
                              width * height, values.size()));
    }
    std::vector<std::uint8_t> texels;
    texels.reserve(values.size());
    for (const VmValue& value : values) {
      texels.push_back(static_cast<std::uint8_t>(std::min<std::uint64_t>(rt::uint64_value(value), 255U)));
    }
    glBindTexture(GL_TEXTURE_2D, gl_uint(int_arg(args, 0)));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gl_sizei(width), gl_sizei(height), GL_RED, GL_UNSIGNED_BYTE,
                    texels.data());
    return null_result();
  });
}

auto draw_elements(const BindingContext&, const VmValue& args) -> NativeInvokeResult {
  return with_window("GL.DrawElements", [&] {
    glDrawElements(gl_enum(int_arg(args, 0)), gl_sizei(int_arg(args, 1)), GL_UNSIGNED_INT, nullptr);
    return null_result();
  });
}

struct Binding {
  const char* symbol;
  NativeInvokeResult (*function)(const BindingContext&, const VmValue&);
};

constexpr Binding kBindings[] = {
    {"GL.OpenWindow", open_window},
    {"GL.CloseWindow", close_window},
    {"GL.WindowShouldClose", window_should_close},
    {"GL.PollEvents", poll_events},
    {"GL.SwapBuffers", swap_buffers},
    {"GL.Time", time},
    {"GL.KeyDown", key_down},
    {"GL.FramebufferSize", framebuffer_size},
    {"GL.CursorPosition", cursor_position},
    {"GL.MouseButtonDown", mouse_button_down},
    {"GL.Viewport", viewport},
    {"GL.ClearColor", clear_color},
    {"GL.Clear", clear},
    {"GL.Enable", enable},
    {"GL.Disable", disable},
    {"GL.CompileProgram", compile_program},
    {"GL.UseProgram", use_program},
    {"GL.UniformLocation", uniform_location},
    {"GL.Uniform1f", uniform1f},
    {"GL.Uniform3f", uniform3f},
    {"GL.Uniform4f", uniform4f},
    {"GL.UniformMatrix4", uniform_matrix4},
    {"GL.CreateVertexArray", create_vertex_array},
    {"GL.BindVertexArray", bind_vertex_array},
    {"GL.CreateBuffer", create_buffer},
    {"GL.CreateIndexBuffer", create_index_buffer},
    {"GL.VertexAttribute", vertex_attribute},
    {"GL.DrawArrays", draw_arrays},
    {"GL.DrawElements", draw_elements},
    {"GL.CreateTexture", create_texture},
    {"GL.UpdateTexture", update_texture},
};

auto register_bindings(const fleaux::embed::BindingPluginHostApi* host_api, const char** error_message)
    -> fleaux::embed::BindingPluginStatus {
  if (!fleaux::embed::require_abi(host_api, error_message)) {
    return fleaux::embed::BindingPluginStatus::kAbiMismatch;
  }
  if (!fleaux::embed::require_host_api(host_api, error_message)) {
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }
  for (const auto& [symbol, function] : kBindings) {
    const auto registered = fleaux::embed::register_native(host_api, fleaux::embed::NativeBinding{
                                                                         .symbol = symbol,
                                                                         .signature = fleaux::embed::BindingSignature{},
                                                                         .callable = function,
                                                                     });
    if (!registered) {
      if (error_message != nullptr) {
        *error_message = "the OpenGL extension could not register its functions";
      }
      return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
    }
  }
  return fleaux::embed::BindingPluginStatus::kOk;
}

}  // namespace

FLEAUX_BINDING_ENTRYPOINT(register_bindings)
