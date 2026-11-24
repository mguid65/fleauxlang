#pragma once
// Path, File, Dir, OS, and file-streaming builtins.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
// #include "fleaux/runtime/value.hpp"
#include <array>
#include <cstdio>
#include <sstream>
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
  if (const auto home = web_env_override("HOME"); home.has_value()) {
    return std::filesystem::path(*home);
  }
  if (const auto userprofile = web_env_override("USERPROFILE"); userprofile.has_value()) {
    return std::filesystem::path(*userprofile);
  }
  ensure_web_virtual_filesystem();
  std::error_code ec;
  std::filesystem::create_directories(web_default_home_directory_path(), ec);
  return web_default_home_directory_path();
}

[[nodiscard]] inline auto web_temp_directory_path() -> std::filesystem::path {
  if (const auto tmpdir = web_env_override("TMPDIR"); tmpdir.has_value()) {
    return std::filesystem::path(*tmpdir);
  }
  if (const auto tmp = web_env_override("TMP"); tmp.has_value()) {
    return std::filesystem::path(*tmp);
  }
  if (const auto temp = web_env_override("TEMP"); temp.has_value()) {
    return std::filesystem::path(*temp);
  }
  ensure_web_virtual_filesystem();
  std::error_code ec;
  std::filesystem::create_directories(web_default_temp_directory_path(), ec);
  return web_default_temp_directory_path();
}

[[nodiscard]] inline auto web_env_value(std::string_view key) -> std::optional<std::string> {
  if (key == "PWD") {
    return current_working_directory_path().string();
  }
  if (const auto overridden = web_env_override(key); overridden.has_value()) {
    return overridden;
  }
  if (key == "HOME" || key == "USERPROFILE") {
    return web_home_directory_path().string();
  }
  if (key == "TMPDIR" || key == "TMP" || key == "TEMP") {
    return web_temp_directory_path().string();
  }
  return std::nullopt;
}

[[nodiscard]] inline auto resolve_runtime_path(std::string_view raw_path) -> std::filesystem::path {
  ensure_web_virtual_filesystem();
  const std::filesystem::path path(raw_path);
  if (path.is_absolute()) {
    return path.lexically_normal();
  }
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
  throw std::runtime_error(std::string(builtin_name) +
                           " is unavailable on web/WASM targets: " + std::string(capability));
}
#else
[[nodiscard]] inline auto current_working_directory_path() -> std::filesystem::path {
  std::error_code ec;
  auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    return cwd;
  }
  return std::filesystem::path{"."};
}

[[nodiscard]] inline auto resolve_runtime_path(std::string_view raw_path) -> std::filesystem::path {
  return {raw_path};
}

inline void ensure_runtime_filesystem_ready() {}

#if defined(_WIN32)
inline void close_runtime_handles_for_path(const std::filesystem::path& target_path) {
  const auto normalized_target = target_path.lexically_normal();
  open_files().for_each([&](FileHandleObject& handle) {
    if (!handle.closed && std::filesystem::path(handle.path).lexically_normal() == normalized_target) {
      handle.stream.close();
      handle.closed = true;
    }
  });
}
#endif
#endif

}  // namespace detail

[[nodiscard]] inline auto Cwd(const Arguments& /*args*/) -> Value {
  return make_string(detail::current_working_directory_path().string());
}

[[nodiscard]] inline auto PathJoin(const Arguments& args) -> Value {
  // args = [seg0, seg1, ...] with at least 2 validated segments.
  if (args.size() < 2U) {
    throw std::logic_error{"internal error: PathJoin called with unexpected validated arity"};
  }
  std::filesystem::path result = as_string(args[0]);
  for (std::size_t segment_index = 1; segment_index < args.size(); ++segment_index) {
    result /= as_string(args[segment_index]);
  }
  return make_string(result.string());
}

[[nodiscard]] inline auto PathNormalize(Value arg) -> Value {
  return make_string(std::filesystem::path(as_string(arg)).lexically_normal().string());
}

[[nodiscard]] inline auto PathBasename(Value arg) -> Value {
  return make_string(std::filesystem::path(as_string(arg)).filename().string());
}

[[nodiscard]] inline auto PathDirname(Value arg) -> Value {
  return make_string(std::filesystem::path(as_string(arg)).parent_path().string());
}

[[nodiscard]] inline auto PathExists(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  return make_bool(std::filesystem::exists(detail::resolve_runtime_path(as_string(arg))));
}

[[nodiscard]] inline auto PathIsFile(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  return make_bool(
      std::filesystem::is_regular_file(detail::resolve_runtime_path(as_string(arg))));
}

[[nodiscard]] inline auto PathIsDir(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  return make_bool(std::filesystem::is_directory(detail::resolve_runtime_path(as_string(arg))));
}

[[nodiscard]] inline auto PathAbsolute(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(arg));
#if defined(__EMSCRIPTEN__)
  return make_string(path.string());
#else
  return make_string(std::filesystem::absolute(path).string());
#endif
}

[[nodiscard]] inline auto PathExtension(Value arg) -> Value {
  return make_string(std::filesystem::path(as_string(arg)).extension().string());
}

[[nodiscard]] inline auto PathStem(Value arg) -> Value {
  return make_string(std::filesystem::path(as_string(arg)).stem().string());
}

[[nodiscard]] inline auto PathWithExtension(const Arguments& args) -> Value {
  std::filesystem::path path = as_string(args[0]);
  std::string extension = as_string(args[1]);
  if (!extension.empty() && extension[0] != '.') {
    extension.insert(extension.begin(), '.');
  }
  path.replace_extension(extension);
  return make_string(path.string());
}

[[nodiscard]] inline auto PathWithBasename(const Arguments& args) -> Value {
  std::filesystem::path path = as_string(args[0]);
  path.replace_filename(as_string(args[1]));
  return make_string(path.string());
}

[[nodiscard]] inline auto FileReadText(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  std::ifstream in(detail::resolve_runtime_path(as_string(arg)));
  if (!in) {
    throw std::runtime_error{"FileReadText failed"};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return make_string(ss.str());
}

[[nodiscard]] inline auto FileWriteText(const Arguments& args) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(args[0]));
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error{"FileWriteText failed"};
  }
  out << as_string(args[1]);
  return make_string(path.string());
}

[[nodiscard]] inline auto FileAppendText(const Arguments& args) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(args[0]));
  std::ofstream out(path, std::ios::app);
  if (!out) {
    throw std::runtime_error{"FileAppendText failed"};
  }
  out << as_string(args[1]);
  return make_string(path.string());
}

[[nodiscard]] inline auto FileReadLines(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  std::ifstream in(detail::resolve_runtime_path(as_string(arg)));
  if (!in) {
    throw std::runtime_error{"FileReadLines failed"};
  }
  TupleBuilder out(4U);
  std::string line;
  while (std::getline(in, line)) {
    out.push(make_string(line));
  }
  return std::move(out).finish();
}

[[nodiscard]] inline auto FileDelete(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(arg));
  std::error_code ec;
  bool removed = std::filesystem::remove(path, ec);
#if defined(_WIN32)
  if (!removed && ec) {
    // Windows refuses to unlink files that are still held open by this process.
    // Retry after closing any Fleaux-managed handles targeting the same path.
    detail::close_runtime_handles_for_path(path);
    ec.clear();
    removed = std::filesystem::remove(path, ec);
  }
#endif
  throw_if_filesystem_error(ec, "FileDelete");
  return make_bool(removed);
}

[[nodiscard]] inline auto FileSize(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  return make_int(
      static_cast<Int>(std::filesystem::file_size(detail::resolve_runtime_path(as_string(arg)))));
}

[[nodiscard]] inline auto DirCreate(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(arg));
  std::filesystem::create_directories(path);
  return make_string(path.string());
}

[[nodiscard]] inline auto DirDelete(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  std::error_code ec;
  const auto removed =
      std::filesystem::remove_all(detail::resolve_runtime_path(as_string(arg)), ec);
  throw_if_filesystem_error(ec, "DirDelete");
  return make_bool(removed > 0);
}

[[nodiscard]] inline auto DirList(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  std::vector<std::string> names;
  for (const auto& entry :
       std::filesystem::directory_iterator(detail::resolve_runtime_path(as_string(arg)))) {
    names.push_back(entry.path().filename().string());
  }
  TupleBuilder out(names.size());
  for (const auto& name : names) {
    out.push(make_string(name));
  }
  return std::move(out).finish();
}

[[nodiscard]] inline auto DirListFull(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  std::vector<std::string> names;
  for (const auto& entry :
       std::filesystem::directory_iterator(detail::resolve_runtime_path(as_string(arg)))) {
    names.push_back(entry.path().string());
  }
  TupleBuilder out(names.size());
  for (const auto& name : names) {
    out.push(make_string(name));
  }
  return std::move(out).finish();
}

[[nodiscard]] inline auto OSEnv(Value arg) -> Value {
  const std::string key = as_string(arg);
#if defined(__EMSCRIPTEN__)
  if (const auto emulated = detail::web_env_value(key); emulated.has_value()) {
    return make_string(*emulated);
  }
  return make_null();
#else
  if (const char* env_value = std::getenv(key.c_str())) {
    return make_string(env_value);
  }
  return make_null();
#endif
}

[[nodiscard]] inline auto OSHasEnv(Value arg) -> Value {
  const std::string key = as_string(arg);
#if defined(__EMSCRIPTEN__)
  return make_bool(detail::web_env_value(key).has_value());
#else
  return make_bool(std::getenv(key.c_str()) != nullptr);
#endif
}

[[nodiscard]] inline auto OSSetEnv(const Arguments& args) -> Value {
  const std::string key = as_string(args[0]);
  const std::string value = as_string(args[1]);
#if defined(__EMSCRIPTEN__)
  detail::set_web_env_value(key, value);
  return make_string(value);
#else
#if defined(_WIN32)
  if (const int rc = _putenv_s(key.c_str(), value.c_str()); rc != 0) {
    throw std::runtime_error{"OSSetEnv failed"};
  }
#else
  if (setenv(key.c_str(), value.c_str(), 1) != 0) {
    throw std::runtime_error{"OSSetEnv failed"};
  }
#endif
  return make_string(value);
#endif
}

[[nodiscard]] inline auto OSUnsetEnv(Value arg) -> Value {
  const std::string key = as_string(arg);
#if defined(__EMSCRIPTEN__)
  return make_bool(detail::unset_web_env_value(key));
#else
  const bool existed = std::getenv(key.c_str()) != nullptr;
#if defined(_WIN32)
  if (const int rc = _putenv_s(key.c_str(), ""); rc != 0) {
    throw std::runtime_error{"OSUnsetEnv failed"};
  }
#else
  if (unsetenv(key.c_str()) != 0) {
    throw std::runtime_error{"OSUnsetEnv failed"};
  }
#endif
  return make_bool(existed);
#endif
}

[[nodiscard]] inline auto OSIsWindows(const Arguments& /*args*/) -> Value {
#if defined(__EMSCRIPTEN__)
  return make_bool(false);
#elif defined(_WIN32)
  return make_bool(true);
#else
  return make_bool(false);
#endif
}

[[nodiscard]] inline auto OSIsLinux(const Arguments& /*args*/) -> Value {
#if defined(__EMSCRIPTEN__)
  return make_bool(false);
#elif defined(__linux__)
  return make_bool(true);
#else
  return make_bool(false);
#endif
}

[[nodiscard]] inline auto OSIsMacOS(const Arguments& /*args*/) -> Value {
#if defined(__EMSCRIPTEN__)
  return make_bool(false);
#elif defined(__APPLE__)
  return make_bool(true);
#else
  return make_bool(false);
#endif
}

[[nodiscard]] inline auto OSHome(const Arguments& /*args*/) -> Value {
#if defined(__EMSCRIPTEN__)
  return make_string(detail::web_home_directory_path().string());
#else
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return make_string(home);
  }
  if (const char* userprofile = std::getenv("USERPROFILE"); userprofile != nullptr && userprofile[0] != '\0') {
    return make_string(userprofile);
  }
  const char* homedrive = std::getenv("HOMEDRIVE");
  if (const char* homepath = std::getenv("HOMEPATH");
      homedrive != nullptr && homepath != nullptr && homedrive[0] != '\0' && homepath[0] != '\0') {
    return make_string(std::string(homedrive) + std::string(homepath));
  }

  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    return make_string(cwd.string());
  }
  return make_string(".");
#endif
}

[[nodiscard]] inline auto OSTempDir(const Arguments& /*args*/) -> Value {
#if defined(__EMSCRIPTEN__)
  return make_string(detail::web_temp_directory_path().string());
#else
  return make_string(std::filesystem::temp_directory_path().string());
#endif
}

[[nodiscard]] inline auto OSMakeTempFile(const Arguments& /*args*/) -> Value {
  std::error_code ec;
#if defined(__EMSCRIPTEN__)
  const auto dir = detail::web_temp_directory_path();
#else
  const auto dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return make_null();
  }
#endif
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (const auto candidate = dir / ("fleaux_" + random_suffix() + ".tmp");
        !std::filesystem::exists(candidate, ec) && !ec) {
      if (std::ofstream out(candidate); out.good()) {
        return make_string(candidate.string());
      }
    }
  }
  return make_null();
}

[[nodiscard]] inline auto OSMakeTempDir(const Arguments& /*args*/) -> Value {
  std::error_code ec;
#if defined(__EMSCRIPTEN__)
  const auto dir = detail::web_temp_directory_path();
#else
  const auto dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return make_null();
  }
#endif
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (const auto candidate = dir / ("fleaux_" + random_suffix());
        std::filesystem::create_directory(candidate, ec) && !ec) {
      return make_string(candidate.string());
    }
  }
  return make_null();
}

[[nodiscard]] inline auto OSExec(Value arg) -> Value {
  // arg = command -> (exit_code, combined_output)
  const std::string command = as_string(arg);

#if defined(__EMSCRIPTEN__)
  return make_tuple(
      make_int(-1),
      make_string(
          "Std.OS.Exec is unavailable on web/WASM targets: shell command execution is not supported in the browser"));
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
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

#if defined(_WIN32)
  const int close_status = _pclose(pipe);
  const Int exit_code = static_cast<Int>(close_status);
#else
  const int close_status = pclose(pipe);
  Int exit_code = static_cast<Int>(close_status);
  if (WIFEXITED(close_status)) {
    exit_code = static_cast<Int>(WEXITSTATUS(close_status));
  }
#endif

  return make_tuple(make_int(exit_code), make_string(std::move(output)));
}

// File streaming builtins

[[nodiscard]] inline auto FileOpen(const Arguments& args) -> Value {
  // args = (path, mode)
  detail::ensure_runtime_filesystem_ready();
  const std::string path = as_string(args[0]);
  const std::string mode = as_string(args[1]);
  return open_file_handle(detail::resolve_runtime_path(path).string(), mode);
}

[[nodiscard]] inline auto FileReadLine(Value arg) -> Value {
  // arg = handle_token   ->  (handle_token, line, eof_bool)
  const Value token = std::move(arg);
  auto& handle_entry = require_handle(token, "FileReadLine");
  if (handle_entry.mode.find('r') == std::string::npos) {
    throw std::runtime_error{"FileReadLine: read failed"};
  }
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
  return make_tuple(Value{token}, make_string(std::move(line)), make_bool(eof));
}

[[nodiscard]] inline auto FileReadChunk(const Arguments& args) -> Value {
  // args = (handle_token, nbytes)  ->  (handle_token, chunk_string, eof_bool)
  auto& handle_entry = require_handle(args[0], "FileReadChunk");
  const std::size_t nbytes = uint64_index(args[1]);
  std::string buf(nbytes, '\0');
  handle_entry.stream.read(buf.data(), static_cast<std::streamsize>(nbytes));
  const std::streamsize bytes_read = handle_entry.stream.gcount();
  buf.resize(static_cast<std::size_t>(bytes_read));
  const bool eof = (bytes_read == 0 || handle_entry.stream.eof());
  return make_tuple(Value{args[0]}, make_string(std::move(buf)), make_bool(eof));
}

[[nodiscard]] inline auto FileWriteChunk(const Arguments& args) -> Value {
  // args = (handle_token, data_string)  ->  handle_token
  auto& handle_entry = require_handle(args[0], "FileWriteChunk");
  const std::string& data = as_string(args[1]);
  handle_entry.stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!handle_entry.stream)
    throw std::runtime_error{"FileWriteChunk: write failed"};
  return args[0];
}

[[nodiscard]] inline auto FileFlush(Value arg) -> Value {
  // arg = handle_token ->  handle_token
  const Value token = std::move(arg);
  auto& handle_entry = require_handle(token, "FileFlush");
  handle_entry.stream.flush();
  if (!handle_entry.stream)
    throw std::runtime_error{"FileFlush: flush failed"};
  return token;
}

[[nodiscard]] inline auto FileClose(Value arg) -> Value {
  // arg = handle_token  ->  Bool (true if was open, false if already closed)
  const Value token = std::move(arg);
  if (!token.is_file_handle() || token.file_handle().closed) {
    return make_bool(false);
  }
  FileHandleObject& handle = token.file_handle();
  handle.stream.close();
  handle.closed = true;
  return make_bool(true);
}

[[nodiscard]] inline auto FileWithOpen(const Arguments& args) -> Value {
  // args = (path, mode, func_ref)  ->  result of func_ref(handle_token)
  // Guarantees close even if func throws.
  detail::ensure_runtime_filesystem_ready();
  const std::string path = detail::resolve_runtime_path(as_string(args[0])).string();
  const std::string mode = as_string(args[1]);
  const Value& fn = args[2];
  const Value handle = open_file_handle(path, mode);
  const auto close = [&handle]() -> void {
    if (FileHandleObject& file = handle.file_handle(); !file.closed) {
      file.stream.close();
      file.closed = true;
    }
  };
  try {
    Value result = invoke_callable(fn, handle);
    close();
    return result;
  } catch (...) {
    close();
    throw;
  }
}

}  // namespace fleaux::runtime
