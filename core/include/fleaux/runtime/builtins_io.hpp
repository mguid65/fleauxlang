#pragma once
// Path, File, Dir, OS, and file-streaming builtins.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
//
// What the world can refuse (a file that does not exist, a directory that cannot be listed, a process that does not
// start) is an Err, whose message names the builtin, what it could not do, and why. Using a file handle after it is
// closed, or reading from one opened only to write, is the program's mistake, and a runtime error.
#include <array>
#include <cerrno>
#include <cstdio>
#include <format>
#include <sstream>
#include <system_error>

#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/runtime/value.hpp"
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

// An Err of `builtin` that could not do `action` with `path`: "Std.File.ReadText: cannot read 'data.txt': No such
// file or directory". `detail` follows the path ("with mode 'r'").
[[nodiscard]] inline auto path_error(const std::string_view builtin, const std::string_view action, const std::filesystem::path& path,
                                     const std::string& reason, const std::string_view detail = {}) -> Value {
  return ResultErr(
      make_string(std::format("{}: cannot {} '{}'{}{}: {}", builtin, action, path.string(), detail.empty() ? "" : " ", detail, reason)));
}

[[nodiscard]] inline auto errc_message(const std::errc code) -> std::string { return std::make_error_code(code).message(); }

// Why a file could not be opened, from what the file system says of it: a stream that fails to open gives no reason
// of its own.
[[nodiscard]] inline auto open_failure_reason(const std::filesystem::path& path, const bool writing) -> std::string {
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (std::filesystem::is_directory(status)) {
    return errc_message(std::errc::is_a_directory);
  }
  if (!writing && !std::filesystem::exists(status)) {
    return errc_message(std::errc::no_such_file_or_directory);
  }
  if (writing && path.has_parent_path() && !std::filesystem::is_directory(path.parent_path(), ec)) {
    return errc_message(std::errc::no_such_file_or_directory);
  }
  return errc_message(std::errc::permission_denied);
}

// The names in a directory, or an Err: full paths or file names.
[[nodiscard]] inline auto list_directory(const std::string_view builtin, const std::filesystem::path& path, const bool full_paths)
    -> Value {
  std::error_code ec;
  std::filesystem::directory_iterator entries(path, ec);
  if (ec) {
    return path_error(builtin, "list", path, ec.message());
  }
  TupleBuilder out(8U);
  for (const std::filesystem::directory_iterator end; entries != end; entries.increment(ec)) {
    if (ec) {
      return path_error(builtin, "list", path, ec.message());
    }
    out.push(make_string(full_paths ? entries->path().string() : entries->path().filename().string()));
  }
  if (ec) {
    return path_error(builtin, "list", path, ec.message());
  }
  return ResultOk(std::move(out).finish());
}

// The open file a handle refers to, for reading (`for_reading`) or writing: a closed handle, or one opened for the
// other, is a runtime error.
[[nodiscard]] inline auto handle_for(const Value& handle, const bool for_reading) -> FileHandleObject& {
  FileHandleObject& file = require_handle(handle);
  const bool readable = file.mode.find('r') != std::string::npos || file.mode.find_first_of("wa") == std::string::npos;
  const bool writable = file.mode.find_first_of("wa") != std::string::npos;
  if (for_reading ? !readable : !writable) {
    throw std::invalid_argument{
        std::format("the file '{}' was opened with mode '{}', which does not {}", file.path, file.mode, for_reading ? "read" : "write")};
  }
  return file;
}

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
  const auto path = detail::resolve_runtime_path(as_string(arg));
  std::ifstream in(path);
  if (!in) {
    return detail::path_error("Std.File.ReadText", "read", path, detail::open_failure_reason(path, false));
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ResultOk(make_string(ss.str()));
}

[[nodiscard]] inline auto FileWriteText(const Arguments& args) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(args[0]));
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return detail::path_error("Std.File.WriteText", "write", path, detail::open_failure_reason(path, true));
  }
  out << as_string(args[1]);
  out.close();
  if (!out) {
    return detail::path_error("Std.File.WriteText", "write", path, detail::errc_message(std::errc::io_error));
  }
  return ResultOk(make_string(path.string()));
}

[[nodiscard]] inline auto FileAppendText(const Arguments& args) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(args[0]));
  std::ofstream out(path, std::ios::app);
  if (!out) {
    return detail::path_error("Std.File.AppendText", "append to", path, detail::open_failure_reason(path, true));
  }
  out << as_string(args[1]);
  out.close();
  if (!out) {
    return detail::path_error("Std.File.AppendText", "append to", path, detail::errc_message(std::errc::io_error));
  }
  return ResultOk(make_string(path.string()));
}

[[nodiscard]] inline auto FileReadLines(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(arg));
  std::ifstream in(path);
  if (!in) {
    return detail::path_error("Std.File.ReadLines", "read", path, detail::open_failure_reason(path, false));
  }
  TupleBuilder out(4U);
  std::string line;
  while (std::getline(in, line)) {
    out.push(make_string(line));
  }
  return ResultOk(std::move(out).finish());
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
  if (ec) {
    return detail::path_error("Std.File.Delete", "delete", path, ec.message());
  }
  return ResultOk(make_bool(removed));
}

[[nodiscard]] inline auto FileSize(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(arg));
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return detail::path_error("Std.File.Size", "read the size of", path, ec.message());
  }
  return ResultOk(make_int(static_cast<Int>(size)));
}

[[nodiscard]] inline auto DirCreate(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(arg));
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return detail::path_error("Std.Dir.Create", "create", path, ec.message());
  }
  return ResultOk(make_string(path.string()));
}

[[nodiscard]] inline auto DirDelete(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(arg));
  std::error_code ec;
  const auto removed = std::filesystem::remove_all(path, ec);
  if (ec) {
    return detail::path_error("Std.Dir.Delete", "delete", path, ec.message());
  }
  return ResultOk(make_bool(removed > 0));
}

[[nodiscard]] inline auto DirList(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  return detail::list_directory("Std.Dir.List", detail::resolve_runtime_path(as_string(arg)), false);
}

[[nodiscard]] inline auto DirListFull(Value arg) -> Value {
  detail::ensure_runtime_filesystem_ready();
  return detail::list_directory("Std.Dir.ListFull", detail::resolve_runtime_path(as_string(arg)), true);
}

[[nodiscard]] inline auto OSEnv(Value arg) -> Value {
  const std::string key = as_string(arg);
#if defined(__EMSCRIPTEN__)
  if (const auto emulated = detail::web_env_value(key); emulated.has_value()) {
    return ResultOk(make_string(*emulated));
  }
#else
  if (const char* env_value = std::getenv(key.c_str())) {
    return ResultOk(make_string(env_value));
  }
#endif
  return ResultErr(make_string(std::format("Std.OS.Env: '{}' is not set", key)));
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
#else
#if defined(_WIN32)
  const int rc = _putenv_s(key.c_str(), value.c_str());
#else
  const int rc = setenv(key.c_str(), value.c_str(), 1) == 0 ? 0 : errno;
#endif
  if (rc != 0) {
    return ResultErr(make_string(std::format("Std.OS.SetEnv: cannot set '{}': {}", key, std::generic_category().message(rc))));
  }
#endif
  return ResultOk(make_string(value));
}

[[nodiscard]] inline auto OSUnsetEnv(Value arg) -> Value {
  const std::string key = as_string(arg);
#if defined(__EMSCRIPTEN__)
  return ResultOk(make_bool(detail::unset_web_env_value(key)));
#else
  const bool existed = std::getenv(key.c_str()) != nullptr;
#if defined(_WIN32)
  const int rc = _putenv_s(key.c_str(), "");
#else
  const int rc = unsetenv(key.c_str()) == 0 ? 0 : errno;
#endif
  if (rc != 0) {
    return ResultErr(make_string(std::format("Std.OS.UnsetEnv: cannot unset '{}': {}", key, std::generic_category().message(rc))));
  }
  return ResultOk(make_bool(existed));
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
    return ResultErr(make_string(std::format("Std.OS.MakeTempFile: no temporary directory: {}", ec.message())));
  }
#endif
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (const auto candidate = dir / ("fleaux_" + random_suffix() + ".tmp");
        !std::filesystem::exists(candidate, ec) && !ec) {
      if (std::ofstream out(candidate); out.good()) {
        return ResultOk(make_string(candidate.string()));
      }
    }
  }
  return detail::path_error("Std.OS.MakeTempFile", "create a file in", dir, detail::open_failure_reason(dir / "f", true));
}

[[nodiscard]] inline auto OSMakeTempDir(const Arguments& /*args*/) -> Value {
  std::error_code ec;
#if defined(__EMSCRIPTEN__)
  const auto dir = detail::web_temp_directory_path();
#else
  const auto dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return ResultErr(make_string(std::format("Std.OS.MakeTempDir: no temporary directory: {}", ec.message())));
  }
#endif
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (const auto candidate = dir / ("fleaux_" + random_suffix());
        std::filesystem::create_directory(candidate, ec) && !ec) {
      return ResultOk(make_string(candidate.string()));
    }
  }
  return detail::path_error("Std.OS.MakeTempDir", "create a directory in", dir,
                            ec ? ec.message() : detail::errc_message(std::errc::file_exists));
}

[[nodiscard]] inline auto OSExec(Value arg) -> Value {
  // arg = command -> Ok((exit_code, combined_output)), or an Err when the command cannot start
  const std::string command = as_string(arg);

#if defined(__EMSCRIPTEN__)
  return ResultErr(make_string("Std.OS.Exec: commands cannot run on web/WASM targets"));
#else
#if defined(_WIN32)
  const std::string wrapped = command + " 2>&1";
  FILE* pipe = _popen(wrapped.c_str(), "r");
#else
  const std::string wrapped = command + " 2>&1";
  FILE* pipe = popen(wrapped.c_str(), "r");
#endif
  if (pipe == nullptr) {
    return ResultErr(make_string(std::format("Std.OS.Exec: cannot start '{}': {}", command, std::generic_category().message(errno))));
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

  return ResultOk(make_tuple(make_int(exit_code), make_string(std::move(output))));
#endif
}

// File streaming builtins

[[nodiscard]] inline auto FileOpen(const Arguments& args) -> Value {
  // args = (path, mode) -> Ok(handle), or an Err when the file cannot be opened
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(args[0]));
  const std::string& mode = as_string(args[1]);
  if (auto handle = open_file_handle(path.string(), mode); handle.has_value()) {
    return ResultOk(std::move(*handle));
  }
  return detail::path_error("Std.File.Open", "open", path, detail::open_failure_reason(path, mode.find_first_of("wa") != std::string::npos),
                            std::format("with mode '{}'", mode));
}

[[nodiscard]] inline auto FileReadLine(Value arg) -> Value {
  // arg = handle -> Ok((handle, line, eof)), or an Err when the read fails
  const Value token = std::move(arg);
  auto& handle_entry = detail::handle_for(token, true);
  std::string line;
  bool eof = false;
  if (!std::getline(handle_entry.stream, line)) {
    if (!handle_entry.stream.eof()) {
      return detail::path_error("Std.File.ReadLine", "read", handle_entry.path, detail::errc_message(std::errc::io_error));
    }
    eof = true;
    line.clear();
  }
  return ResultOk(make_tuple(Value{token}, make_string(std::move(line)), make_bool(eof)));
}

[[nodiscard]] inline auto FileReadChunk(const Arguments& args) -> Value {
  // args = (handle, nbytes) -> Ok((handle, chunk, eof)), or an Err when the read fails
  auto& handle_entry = detail::handle_for(args[0], true);
  const std::size_t nbytes = count_index(args[1], "nbytes");
  std::string buf(nbytes, '\0');
  handle_entry.stream.read(buf.data(), static_cast<std::streamsize>(nbytes));
  if (handle_entry.stream.bad()) {
    return detail::path_error("Std.File.ReadChunk", "read", handle_entry.path, detail::errc_message(std::errc::io_error));
  }
  const std::streamsize bytes_read = handle_entry.stream.gcount();
  buf.resize(static_cast<std::size_t>(bytes_read));
  const bool eof = (bytes_read == 0 || handle_entry.stream.eof());
  return ResultOk(make_tuple(Value{args[0]}, make_string(std::move(buf)), make_bool(eof)));
}

[[nodiscard]] inline auto FileWriteChunk(const Arguments& args) -> Value {
  // args = (handle, data) -> Ok(handle), or an Err when the write fails
  auto& handle_entry = detail::handle_for(args[0], false);
  const std::string& data = as_string(args[1]);
  handle_entry.stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!handle_entry.stream) {
    return detail::path_error("Std.File.WriteChunk", "write", handle_entry.path, detail::errc_message(std::errc::io_error));
  }
  return ResultOk(args[0]);
}

[[nodiscard]] inline auto FileFlush(Value arg) -> Value {
  // arg = handle -> Ok(handle), or an Err when the flush fails
  const Value token = std::move(arg);
  auto& handle_entry = detail::handle_for(token, false);
  handle_entry.stream.flush();
  if (!handle_entry.stream) {
    return detail::path_error("Std.File.Flush", "write", handle_entry.path, detail::errc_message(std::errc::io_error));
  }
  return ResultOk(token);
}

[[nodiscard]] inline auto FileClose(Value arg) -> Value {
  // arg = handle -> Ok(true) when it was open, Ok(false) when it was already closed, or an Err when the close fails
  const Value token = std::move(arg);
  if (!token.is_file_handle() || token.file_handle().closed) {
    return ResultOk(make_bool(false));
  }
  FileHandleObject& handle = token.file_handle();
  // A read that reached the end of the file leaves the stream failed; only the close itself can fail it now.
  handle.stream.clear();
  handle.stream.close();
  handle.closed = true;
  if (!handle.stream) {
    return detail::path_error("Std.File.Close", "close", handle.path, detail::errc_message(std::errc::io_error));
  }
  return ResultOk(make_bool(true));
}

[[nodiscard]] inline auto FileWithOpen(const Arguments& args) -> Value {
  // args = (path, mode, func) -> Ok(func(handle)), or an Err when the file cannot be opened. The file is closed after
  // the function, whether it returns or fails.
  detail::ensure_runtime_filesystem_ready();
  const auto path = detail::resolve_runtime_path(as_string(args[0]));
  const std::string& mode = as_string(args[1]);
  const Value& fn = args[2];
  const auto opened = open_file_handle(path.string(), mode);
  if (!opened.has_value()) {
    return detail::path_error("Std.File.WithOpen", "open", path,
                              detail::open_failure_reason(path, mode.find_first_of("wa") != std::string::npos),
                              std::format("with mode '{}'", mode));
  }
  const Value& handle = *opened;
  const auto close = [&handle]() -> void {
    if (FileHandleObject& file = handle.file_handle(); !file.closed) {
      file.stream.close();
      file.closed = true;
    }
  };
  try {
    Value result = invoke_callable(fn, handle);
    close();
    return ResultOk(std::move(result));
  } catch (...) {
    close();
    throw;
  }
}

}  // namespace fleaux::runtime
