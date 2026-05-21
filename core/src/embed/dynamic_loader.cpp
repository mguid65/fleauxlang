#include "fleaux/embed/dynamic_loader.hpp"

#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fleaux::embed {
namespace {

#if defined(_WIN32)

auto format_message_buffer_arg(LPSTR& buffer) -> LPSTR { return reinterpret_cast<LPSTR>(&buffer); }

auto symbol_address(FARPROC proc) -> void* { return reinterpret_cast<void*>(proc); }

[[nodiscard]] auto last_error_message() -> std::string {
  const DWORD code = GetLastError();
  if (code == 0) {
    return "Dynamic loader error with no OS error code.";
  }

  LPSTR buffer = nullptr;
  constexpr DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = FormatMessageA(flags, nullptr, code, 0, format_message_buffer_arg(buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return "Dynamic loader error code: " + std::to_string(code);
  }

  std::string message(buffer, length);
  LocalFree(buffer);
  return message;
}

class SystemDynamicLibrary final : public DynamicLibrary {
public:
  explicit SystemDynamicLibrary(HMODULE handle) : handle_(handle) {}

  ~SystemDynamicLibrary() override {
    if (handle_ != nullptr) {
      FreeLibrary(handle_);
    }
  }

  [[nodiscard]] auto symbol(const std::string_view symbol_name) const
      -> tl::expected<void*, DynamicLoadError> override {
    if (symbol_name.empty()) {
      return tl::unexpected(DynamicLoadError{
          .message = "Symbol name cannot be empty.",
          .hint = "Pass an exported symbol name.",
      });
    }

    const std::string symbol_name_str{symbol_name};
    FARPROC proc = GetProcAddress(handle_, symbol_name_str.c_str());
    if (proc == nullptr) {
      return tl::unexpected(DynamicLoadError{
          .message = "Failed to resolve symbol '" + std::string(symbol_name) + "': " + last_error_message(),
          .hint = "Check exported symbol names and calling convention.",
      });
    }

    return symbol_address(proc);
  }

private:
  HMODULE handle_{nullptr};
};

class SystemDynamicLoader final : public DynamicLoader {
public:
  [[nodiscard]] auto open(const std::filesystem::path& library_path) const
      -> tl::expected<std::unique_ptr<DynamicLibrary>, DynamicLoadError> override {
    if (library_path.empty()) {
      return tl::unexpected(DynamicLoadError{
          .message = "Library path cannot be empty.",
          .hint = "Pass an absolute or relative shared library path.",
      });
    }

    HMODULE handle = LoadLibraryW(library_path.wstring().c_str());
    if (handle == nullptr) {
      return tl::unexpected(DynamicLoadError{
          .message = "Failed to load dynamic library '" + library_path.string() + "': " + last_error_message(),
          .hint = "Verify the path and dependency availability.",
      });
    }

    return std::make_unique<SystemDynamicLibrary>(handle);
  }
};

#else

[[nodiscard]] auto last_error_message() -> std::string {
  if (const char* error = dlerror(); error != nullptr) {
    return error;
  }
  return "Dynamic loader error with no OS error message.";
}

class SystemDynamicLibrary final : public DynamicLibrary {
public:
  explicit SystemDynamicLibrary(void* handle) : handle_(handle) {}

  ~SystemDynamicLibrary() override {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
  }

  [[nodiscard]] auto symbol(const std::string_view symbol_name) const
      -> tl::expected<void*, DynamicLoadError> override {
    if (symbol_name.empty()) {
      return tl::unexpected(DynamicLoadError{
          .message = "Symbol name cannot be empty.",
          .hint = "Pass an exported symbol name.",
      });
    }

    dlerror();
    const std::string symbol_name_str{symbol_name};
    void* proc = dlsym(handle_, symbol_name_str.c_str());
    if (proc == nullptr) {
      return tl::unexpected(DynamicLoadError{
          .message = "Failed to resolve symbol '" + std::string(symbol_name) + "': " + last_error_message(),
          .hint = "Check exported symbol names and visibility.",
      });
    }

    return proc;
  }

private:
  void* handle_{nullptr};
};

class SystemDynamicLoader final : public DynamicLoader {
public:
  [[nodiscard]] auto open(const std::filesystem::path& library_path) const
      -> tl::expected<std::unique_ptr<DynamicLibrary>, DynamicLoadError> override {
    if (library_path.empty()) {
      return tl::unexpected(DynamicLoadError{
          .message = "Library path cannot be empty.",
          .hint = "Pass an absolute or relative shared library path.",
      });
    }

    dlerror();
    void* handle = dlopen(library_path.string().c_str(), RTLD_LOCAL | RTLD_NOW);
    if (handle == nullptr) {
      return tl::unexpected(DynamicLoadError{
          .message = "Failed to load dynamic library '" + library_path.string() + "': " + last_error_message(),
          .hint = "Verify the path and dependency availability.",
      });
    }

    return std::make_unique<SystemDynamicLibrary>(handle);
  }
};

#endif

auto binding_module_entrypoint(void* entrypoint) -> RegisterBindingModuleFn {
  return reinterpret_cast<RegisterBindingModuleFn>(entrypoint);
}

}  // namespace

auto make_system_dynamic_loader() -> std::unique_ptr<DynamicLoader> { return std::make_unique<SystemDynamicLoader>(); }

auto resolve_binding_module_entrypoint(const DynamicLibrary& library)
    -> tl::expected<RegisterBindingModuleFn, DynamicLoadError> {
  const auto entrypoint = library.symbol(kBindingPluginRegisterEntrypoint);
  if (!entrypoint) {
    return tl::unexpected(DynamicLoadError{
        .message = entrypoint.error().message,
        .hint = std::optional<std::string>{"Binding module is missing the required registration entrypoint."},
    });
  }

  // Dynamic-library APIs surface resolved symbols as untyped addresses. Keep
  // the platform-boundary conversion to the typed registration entrypoint
  // isolated here rather than spreading it through higher-level host logic.
  const auto register_module = binding_module_entrypoint(*entrypoint);
  if (register_module == nullptr) {
    return tl::unexpected(DynamicLoadError{
        .message = "Binding module entrypoint pointer is null.",
        .hint = "Verify the exported registration function signature.",
    });
  }

  return register_module;
}

}  // namespace fleaux::embed
