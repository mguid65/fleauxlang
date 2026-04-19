#pragma once
// Path, File, Dir, OS, and file-streaming builtins.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
// #include "fleaux/runtime/value.hpp"
#include <array>
#include <cstdio>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace fleaux::runtime {
// Path/File/Dir/OS

namespace detail {
#if defined(__EMSCRIPTEN__)
[[nodiscard]] inline auto web_workspace_directory_path() -> std::filesystem::path {
  return std::filesystem::path{"/workspace"};
}

[[nodiscard]] inline auto web_current_working_directory_path_storage() -> std::filesystem::path& {
  static std::filesystem::path cwd = web_workspace_directory_path();
  return cwd;
}

[[nodiscard]] inline auto web_default_home_directory_path() -> std::filesystem::path {
  return std::filesystem::path{"/home/web_user"};
}

[[nodiscard]] inline auto web_default_temp_directory_path() -> std::filesystem::path {
  return std::filesystem::path{"/tmp"};
}

inline void ensure_web_virtual_filesystem() {
  static const bool initialized = [] {
    std::error_code ec;
    std::filesystem::create_directories(web_workspace_directory_path(), ec);
    ec.clear();
    std::filesystem::create_directories(web_default_home_directory_path(), ec);
    ec.clear();
    std::filesystem::create_directories(web_default_temp_directory_path(), ec);
    web_current_working_directory_path_storage() = web_workspace_directory_path();
    return true;
  }();
  (void)initialized;
}

inline void ensure_runtime_filesystem_ready() { ensure_web_virtual_filesystem(); }

[[nodiscard]] inline auto web_env_overrides() -> std::unordered_map<std::string, std::string>& {
  static std::unordered_map<std::string, std::string> vars;
  return vars;
}

[[nodiscard]] inline auto web_env_override(std::string_view key) -> std::optional<std::string> {
  ensure_web_virtual_filesystem();
  if (const auto it = web_env_overrides().find(std::string(key)); it != web_env_overrides().end()) {
    return it->second;
  }
  return std::nullopt;
}

[[nodiscard]] inline auto current_working_directory_path() -> std::filesystem::path {
  ensure_web_virtual_filesystem();
  return web_current_working_directory_path_storage();
}

[[nodiscard]] inline auto web_home_directory_path() -> std::filesystem::path {
  if (const auto home = web_env_override("HOME"); home.has_value()) { return std::filesystem::path(*home); }
  if (const auto userprofile = web_env_override("USERPROFILE"); userprofile.has_value()) {
    return std::filesystem::path(*userprofile);
  }
  ensure_web_virtual_filesystem();
  std::error_code ec;
  std::filesystem::create_directories(web_default_home_directory_path(), ec);
  return web_default_home_directory_path();
}

[[nodiscard]] inline auto web_temp_directory_path() -> std::filesystem::path {
  if (const auto tmpdir = web_env_override("TMPDIR"); tmpdir.has_value()) { return std::filesystem::path(*tmpdir); }
  if (const auto tmp = web_env_override("TMP"); tmp.has_value()) { return std::filesystem::path(*tmp); }
  if (const auto temp = web_env_override("TEMP"); temp.has_value()) { return std::filesystem::path(*temp); }
  ensure_web_virtual_filesystem();
  std::error_code ec;
  std::filesystem::create_directories(web_default_temp_directory_path(), ec);
  return web_default_temp_directory_path();
}

[[nodiscard]] inline auto web_env_value(std::string_view key) -> std::optional<std::string> {
  if (key == "PWD") { return current_working_directory_path().string(); }
  if (const auto overridden = web_env_override(key); overridden.has_value()) { return overridden; }
  if (key == "HOME" || key == "USERPROFILE") { return web_home_directory_path().string(); }
  if (key == "TMPDIR" || key == "TMP" || key == "TEMP") { return web_temp_directory_path().string(); }
  return std::nullopt;
}

[[nodiscard]] inline auto resolve_runtime_path(std::string_view raw_path) -> std::filesystem::path {
  ensure_web_virtual_filesystem();
  const std::filesystem::path path(raw_path);
  if (path.is_absolute()) { return path.lexically_normal(); }
  return (current_working_directory_path() / path).lexically_normal();
}

inline void set_web_env_value(std::string key, std::string value) {
  ensure_web_virtual_filesystem();
  std::filesystem::path resolved = resolve_runtime_path(value);
  std::error_code ec;
  if (key == "PWD") {
    std::filesystem::create_directories(resolved, ec);
    web_current_working_directory_path_storage() = resolved.lexically_normal();
    return;
  }

  if (key == "HOME" || key == "USERPROFILE" || key == "TMPDIR" || key == "TMP" || key == "TEMP") {
    std::filesystem::create_directories(resolved, ec);
    web_env_overrides()[key] = resolved.string();
    return;
  }

  web_env_overrides()[key] = value;
}

[[nodiscard]] inline auto unset_web_env_value(const std::string& key) -> bool {
  ensure_web_virtual_filesystem();
  if (key == "PWD") {
    const bool existed = true;
    web_current_working_directory_path_storage() = web_workspace_directory_path();
    return existed;
  }
  return web_env_overrides().erase(key) > 0;
}

[[noreturn]] inline void throw_web_unsupported(std::string_view builtin_name, std::string_view capability) {
  throw std::runtime_error(std::string(builtin_name) + " is unavailable on web/WASM targets: " + std::string(capability));
}
#else
[[nodiscard]] inline auto current_working_directory_path() -> std::filesystem::path {
  std::error_code ec;
  auto cwd = std::filesystem::current_path(ec);
  if (!ec) { return cwd; }
  return std::filesystem::path{"."};
}

[[nodiscard]] inline auto resolve_runtime_path(std::string_view raw_path) -> std::filesystem::path {
  return std::filesystem::path(raw_path);
}

inline void ensure_runtime_filesystem_ready() {}
#endif

}  // namespace detail

struct Cwd {
  auto operator()(Value arg) const -> Value {
    (void)require_args(arg, 0, "Cwd");
    return make_string(detail::current_working_directory_path().string());
  }
};

struct PathJoin {
  // arg = [seg0, seg1, ...]  — at least 2 segments required.
  auto operator()(Value arg) const -> Value {
    const auto& args = as_array(arg);
    if (args.Size() < 2) { throw std::invalid_argument{"PathJoin expects at least 2 arguments"}; }
    std::filesystem::path result = to_string(*args.TryGet(0));
    for (std::size_t segment_index = 1; segment_index < args.Size(); ++segment_index) {
      result /= to_string(*args.TryGet(segment_index));
    }
    return make_string(result.string());
  }
};

struct PathNormalize {
  auto operator()(Value arg) const -> Value {
    return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).lexically_normal().string());
  }
};

struct PathBasename {
  auto operator()(Value arg) const -> Value {
    return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).filename().string());
  }
};

struct PathDirname {
  auto operator()(Value arg) const -> Value {
    return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).parent_path().string());
  }
};

struct PathExists {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    return make_bool(std::filesystem::exists(detail::resolve_runtime_path(as_path_string_unary(std::move(arg)))));
  }
};

struct PathIsFile {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    return make_bool(std::filesystem::is_regular_file(detail::resolve_runtime_path(as_path_string_unary(std::move(arg)))));
  }
};

struct PathIsDir {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    return make_bool(std::filesystem::is_directory(detail::resolve_runtime_path(as_path_string_unary(std::move(arg)))));
  }
};

struct PathAbsolute {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    const auto path = detail::resolve_runtime_path(as_path_string_unary(std::move(arg)));
#if defined(__EMSCRIPTEN__)
    return make_string(path.string());
#else
    return make_string(std::filesystem::absolute(path).string());
#endif
  }
};

struct PathExtension {
  auto operator()(Value arg) const -> Value {
    return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).extension().string());
  }
};

struct PathStem {
  auto operator()(Value arg) const -> Value {
    return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).stem().string());
  }
};

struct PathWithExtension {
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "PathWithExtension");
    std::filesystem::path path = to_string(*args.TryGet(0));
    std::string extension = to_string(*args.TryGet(1));
    if (!extension.empty() && extension[0] != '.') { extension.insert(extension.begin(), '.'); }
    path.replace_extension(extension);
    return make_string(path.string());
  }
};

struct PathWithBasename {
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "PathWithBasename");
    std::filesystem::path path = to_string(*args.TryGet(0));
    path.replace_filename(to_string(*args.TryGet(1)));
    return make_string(path.string());
  }
};

struct FileReadText {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    std::ifstream in(detail::resolve_runtime_path(as_path_string_unary(std::move(arg))));
    if (!in) { throw std::runtime_error{"FileReadText failed"}; }
    std::ostringstream ss;
    ss << in.rdbuf();
    return make_string(ss.str());
  }
};

struct FileWriteText {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    const auto& args = require_args(arg, 2, "FileWriteText");
    const auto path = detail::resolve_runtime_path(to_string(*args.TryGet(0)));
    std::ofstream out(path, std::ios::trunc);
    if (!out) { throw std::runtime_error{"FileWriteText failed"}; }
    out << to_string(*args.TryGet(1));
    return make_string(path.string());
  }
};

struct FileAppendText {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    const auto& args = require_args(arg, 2, "FileAppendText");
    const auto path = detail::resolve_runtime_path(to_string(*args.TryGet(0)));
    std::ofstream out(path, std::ios::app);
    if (!out) { throw std::runtime_error{"FileAppendText failed"}; }
    out << to_string(*args.TryGet(1));
    return make_string(path.string());
  }
};

struct FileReadLines {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    std::ifstream in(detail::resolve_runtime_path(as_path_string_unary(std::move(arg))));
    if (!in) { throw std::runtime_error{"FileReadLines failed"}; }
    Array out;
    std::string line;
    while (std::getline(in, line)) { out.PushBack(make_string(line)); }
    return Value{std::move(out)};
  }
};

struct FileDelete {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    std::error_code ec;
    const bool removed = std::filesystem::remove(detail::resolve_runtime_path(as_path_string_unary(std::move(arg))), ec);
    throw_if_filesystem_error(ec, "FileDelete");
    return make_bool(removed);
  }
};

struct FileSize {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    return make_int(static_cast<Int>(std::filesystem::file_size(detail::resolve_runtime_path(as_path_string_unary(std::move(arg))))));
  }
};

struct DirCreate {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    const auto path = detail::resolve_runtime_path(as_path_string_unary(std::move(arg)));
    std::filesystem::create_directories(path);
    return make_string(path.string());
  }
};

struct DirDelete {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(detail::resolve_runtime_path(as_path_string_unary(std::move(arg))), ec);
    throw_if_filesystem_error(ec, "DirDelete");
    return make_bool(removed > 0);
  }
};

struct DirList {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(detail::resolve_runtime_path(as_path_string_unary(std::move(arg))))) {
      names.push_back(entry.path().filename().string());
    }
    Array out;
    out.Reserve(names.size());
    for (const auto& name : names) { out.PushBack(make_string(name)); }
    return Value{std::move(out)};
  }
};

struct DirListFull {
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(detail::resolve_runtime_path(as_path_string_unary(std::move(arg))))) {
      names.push_back(entry.path().string());
    }
    Array out;
    out.Reserve(names.size());
    for (const auto& name : names) { out.PushBack(make_string(name)); }
    return Value{std::move(out)};
  }
};

struct OSEnv {
  auto operator()(Value arg) const -> Value {
    const std::string key = as_path_string_unary(std::move(arg));
#if defined(__EMSCRIPTEN__)
    if (const auto emulated = detail::web_env_value(key); emulated.has_value()) { return make_string(*emulated); }
    return make_null();
#else
    if (const char* env_value = std::getenv(key.c_str())) { return make_string(env_value); }
    return make_null();
#endif
  }
};

struct OSHasEnv {
  auto operator()(Value arg) const -> Value {
    const std::string key = as_path_string_unary(std::move(arg));
#if defined(__EMSCRIPTEN__)
    return make_bool(detail::web_env_value(key).has_value());
#else
    return make_bool(std::getenv(key.c_str()) != nullptr);
#endif
  }
};

struct OSSetEnv {
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "OSSetEnv");
    const std::string key = to_string(*args.TryGet(0));
    const std::string value = to_string(*args.TryGet(1));
#if defined(__EMSCRIPTEN__)
    detail::set_web_env_value(key, value);
    return make_string(value);
#else
#if defined(_WIN32)
    const int rc = _putenv_s(key.c_str(), value.c_str());
    if (rc != 0) { throw std::runtime_error{"OSSetEnv failed"}; }
#else
    if (setenv(key.c_str(), value.c_str(), 1) != 0) { throw std::runtime_error{"OSSetEnv failed"}; }
#endif
    return make_string(value);
#endif
  }
};

struct OSUnsetEnv {
  auto operator()(Value arg) const -> Value {
    const std::string key = as_path_string_unary(std::move(arg));
#if defined(__EMSCRIPTEN__)
    return make_bool(detail::unset_web_env_value(key));
#else
    const bool existed = std::getenv(key.c_str()) != nullptr;
#if defined(_WIN32)
    const int rc = _putenv_s(key.c_str(), "");
    if (rc != 0) { throw std::runtime_error{"OSUnsetEnv failed"}; }
#else
    if (unsetenv(key.c_str()) != 0) { throw std::runtime_error{"OSUnsetEnv failed"}; }
#endif
    return make_bool(existed);
#endif
  }
};

struct OSIsWindows {
  auto operator()(Value arg) const -> Value {
    (void)require_args(arg, 0, "OSIsWindows");
#if defined(__EMSCRIPTEN__)
    return make_bool(false);
#elif defined(_WIN32)
    return make_bool(true);
#else
    return make_bool(false);
#endif
  }
};

struct OSIsLinux {
  auto operator()(Value arg) const -> Value {
    (void)require_args(arg, 0, "OSIsLinux");
#if defined(__EMSCRIPTEN__)
    return make_bool(false);
#elif defined(__linux__)
    return make_bool(true);
#else
    return make_bool(false);
#endif
  }
};

struct OSIsMacOS {
  auto operator()(Value arg) const -> Value {
    (void)require_args(arg, 0, "OSIsMacOS");
#if defined(__EMSCRIPTEN__)
    return make_bool(false);
#elif defined(__APPLE__)
    return make_bool(true);
#else
    return make_bool(false);
#endif
  }
};

struct OSHome {
  auto operator()(Value arg) const -> Value {
    (void)require_args(arg, 0, "OSHome");
#if defined(__EMSCRIPTEN__)
    return make_string(detail::web_home_directory_path().string());
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') { return make_string(home); }
    if (const char* userprofile = std::getenv("USERPROFILE"); userprofile != nullptr && userprofile[0] != '\0') {
      return make_string(userprofile);
    }
    const char* homedrive = std::getenv("HOMEDRIVE");
    const char* homepath = std::getenv("HOMEPATH");
    if (homedrive != nullptr && homepath != nullptr && homedrive[0] != '\0' && homepath[0] != '\0') {
      return make_string(std::string(homedrive) + std::string(homepath));
    }

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) { return make_string(cwd.string()); }
    return make_string(".");
#endif
  }
};

struct OSTempDir {
  auto operator()(Value arg) const -> Value {
    (void)require_args(arg, 0, "OSTempDir");
#if defined(__EMSCRIPTEN__)
    return make_string(detail::web_temp_directory_path().string());
#else
    return make_string(std::filesystem::temp_directory_path().string());
#endif
  }
};

struct OSMakeTempFile {
  auto operator()(Value arg) const -> Value {
    (void)require_args(arg, 0, "OSMakeTempFile");
    std::error_code ec;
#if defined(__EMSCRIPTEN__)
    const auto dir = detail::web_temp_directory_path();
#else
    const auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) { return make_null(); }
#endif
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (const auto candidate = dir / ("fleaux_" + random_suffix() + ".tmp");
          !std::filesystem::exists(candidate, ec) && !ec) {
        if (std::ofstream out(candidate); out.good()) { return make_string(candidate.string()); }
      }
    }
    return make_null();
  }
};

struct OSMakeTempDir {
  auto operator()(Value arg) const -> Value {
    (void)require_args(arg, 0, "OSMakeTempDir");
    std::error_code ec;
#if defined(__EMSCRIPTEN__)
    const auto dir = detail::web_temp_directory_path();
#else
    const auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) { return make_null(); }
#endif
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (const auto candidate = dir / ("fleaux_" + random_suffix());
          std::filesystem::create_directory(candidate, ec) && !ec) { return make_string(candidate.string()); }
    }
    return make_null();
  }
};

struct OSExec {
  // arg = command -> (exit_code, combined_output)
  auto operator()(Value arg) const -> Value {
    const std::string command = as_path_string_unary(std::move(arg));

#if defined(__EMSCRIPTEN__)
    return make_tuple(make_int(-1), make_string("Std.OS.Exec is unavailable on web/WASM targets: shell command execution is not supported in the browser"));
#endif

#if defined(_WIN32)
    const std::string wrapped = command + " 2>&1";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    const std::string wrapped = command + " 2>&1";
    FILE* pipe = popen(wrapped.c_str(), "r");
#endif
    if (pipe == nullptr) {
      throw std::runtime_error{"OSExec: failed to start command"};
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) { output += buffer.data(); }

#if defined(_WIN32)
    const int close_status = _pclose(pipe);
    const Int exit_code = static_cast<Int>(close_status);
#else
    const int close_status = pclose(pipe);
    Int exit_code = static_cast<Int>(close_status);
    if (WIFEXITED(close_status)) { exit_code = static_cast<Int>(WEXITSTATUS(close_status)); }
#endif

    return make_tuple(make_int(exit_code), make_string(std::move(output)));
  }
};

// File streaming builtins

struct FileOpen {
  // arg = (path, mode)  OR  (path,) defaults mode to "r"
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    const auto& arr = arg.TryGetArray();
    std::string path, mode = "r";
    if (arr && arr->Size() == 2) {
      path = as_string(*arr->TryGet(0));
      mode = as_string(*arr->TryGet(1));
    } else if (arr && arr->Size() == 1) {
      path = as_string(*arr->TryGet(0));
    } else if (arr) {
      throw std::runtime_error{"FileOpen: expected (path,) or (path, mode)"};
    } else {
      path = as_string(arg);
    }
    const UInt slot = handle_registry().open(detail::resolve_runtime_path(path).string(), mode);
    const UInt gen = handle_registry().entries[slot].generation;
    return make_handle_token(slot, gen);
  }
};

struct FileReadLine {
  // arg = handle_token   ->  (handle_token, line, eof_bool)
  auto operator()(Value arg) const -> Value {
    const Value token = unwrap_singleton_arg(std::move(arg));
    auto& handle_entry = require_handle(token, "FileReadLine");
    if (handle_entry.mode.find('r') == std::string::npos) { throw std::runtime_error{"FileReadLine: read failed"}; }
    std::string line;
    bool eof = false;
    if (!std::getline(handle_entry.stream, line)) {
      if (handle_entry.stream.eof()) {
        eof = true;
        line.clear();
      } else {
        throw std::runtime_error{"FileReadLine: read failed"};
      }
    }
    auto [slot, gen] = handle_id_from_value(token).value();
    return make_tuple(make_handle_token(slot, gen), make_string(std::move(line)), make_bool(eof));
  }
};

struct FileReadChunk {
  // arg = (handle_token, nbytes)  ->  (handle_token, chunk_string, eof_bool)
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "FileReadChunk");
    auto& handle_entry = require_handle(*args.TryGet(0), "FileReadChunk");
    const std::size_t nbytes = as_index_strict(*args.TryGet(1), "FileReadChunk nbytes");
    std::string buf(nbytes, '\0');
    handle_entry.stream.read(buf.data(), static_cast<std::streamsize>(nbytes));
    const std::streamsize bytes_read = handle_entry.stream.gcount();
    buf.resize(static_cast<std::size_t>(bytes_read));
    const bool eof = (bytes_read == 0 || handle_entry.stream.eof());
    auto [slot, gen] = handle_id_from_value(*args.TryGet(0)).value();
    return make_tuple(make_handle_token(slot, gen), make_string(std::move(buf)), make_bool(eof));
  }
};

struct FileWriteChunk {
  // arg = (handle_token, data_string)  ->  handle_token
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "FileWriteChunk");
    auto& handle_entry = require_handle(*args.TryGet(0), "FileWriteChunk");
    const std::string& data = as_string(*args.TryGet(1));
    handle_entry.stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!handle_entry.stream) throw std::runtime_error{"FileWriteChunk: write failed"};
    auto [slot, gen] = handle_id_from_value(*args.TryGet(0)).value();
    return make_handle_token(slot, gen);
  }
};

struct FileFlush {
  // arg = handle_token ->  handle_token
  auto operator()(Value arg) const -> Value {
    const Value token = unwrap_singleton_arg(std::move(arg));
    auto& handle_entry = require_handle(token, "FileFlush");
    handle_entry.stream.flush();
    if (!handle_entry.stream) throw std::runtime_error{"FileFlush: flush failed"};
    auto [slot, gen] = handle_id_from_value(token).value();
    return make_handle_token(slot, gen);
  }
};

struct FileClose {
  // arg = handle_token  ->  Bool (true if was open, false if already closed)
  auto operator()(Value arg) const -> Value {
    const Value token = unwrap_singleton_arg(std::move(arg));
    const auto id = handle_id_from_value(token);
    if (!id) return make_bool(false);
    return make_bool(handle_registry().close(id->slot, id->gen));
  }
};

struct FileWithOpen {
  // arg = (path, mode, func_ref)  ->  result of func_ref(handle_token)
  // Guarantees close even if func throws.
  auto operator()(Value arg) const -> Value {
    detail::ensure_runtime_filesystem_ready();
    const auto& args = require_args(arg, 3, "FileWithOpen");
    const std::string path = detail::resolve_runtime_path(as_string(*args.TryGet(0))).string();
    const std::string mode = as_string(*args.TryGet(1));
    const Value& fn = *args.TryGet(2);
    const UInt slot = handle_registry().open(path, mode);
    const UInt gen = handle_registry().entries[slot].generation;
    const Value token = make_handle_token(slot, gen);
    try {
      Value result = invoke_callable_ref(fn, token);
      handle_registry().close(slot, gen);
      return result;
    } catch (...) {
      handle_registry().close(slot, gen);
      throw;
    }
  }
};

}  // namespace fleaux::runtime
