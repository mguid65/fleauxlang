#pragma once
// Path, File, Dir, OS, and file-streaming builtins.
// Part of the split fleaux_runtime; included by fleaux/runtime/fleaux_runtime.hpp.
#include "value.hpp"
namespace fleaux::runtime {
// ── Path/File/Dir/OS ──────────────────────────────────────────────────────────

struct Cwd {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "Cwd");
        return make_string(std::filesystem::current_path().string());
    }
};

struct PathJoin {
    // arg = [seg0, seg1, ...]  — at least 2 segments required.
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() < 2) {
            throw std::invalid_argument{"PathJoin expects at least 2 arguments"};
        }
        std::filesystem::path result = to_string(*args.TryGet(0));
        for (std::size_t i = 1; i < args.Size(); ++i) {
            result /= to_string(*args.TryGet(i));
        }
        return make_string(result.string());
    }
};

struct PathNormalize {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).lexically_normal().string());
    }
};

struct PathBasename {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).filename().string());
    }
};

struct PathDirname {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).parent_path().string());
    }
};

struct PathExists {
    Value operator()(Value arg) const {
        return make_bool(std::filesystem::exists(std::filesystem::path(as_path_string_unary(std::move(arg)))));
    }
};

struct PathIsFile {
    Value operator()(Value arg) const {
        return make_bool(std::filesystem::is_regular_file(std::filesystem::path(as_path_string_unary(std::move(arg)))));
    }
};

struct PathIsDir {
    Value operator()(Value arg) const {
        return make_bool(std::filesystem::is_directory(std::filesystem::path(as_path_string_unary(std::move(arg)))));
    }
};

struct PathAbsolute {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::absolute(std::filesystem::path(as_path_string_unary(std::move(arg)))).string());
    }
};

struct PathExtension {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).extension().string());
    }
};

struct PathStem {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).stem().string());
    }
};

struct PathWithExtension {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "PathWithExtension");
        std::filesystem::path p = to_string(*args.TryGet(0));
        std::string ext = to_string(*args.TryGet(1));
        if (!ext.empty() && ext[0] != '.') {
            ext.insert(ext.begin(), '.');
        }
        p.replace_extension(ext);
        return make_string(p.string());
    }
};

struct PathWithBasename {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "PathWithBasename");
        std::filesystem::path p = to_string(*args.TryGet(0));
        p.replace_filename(to_string(*args.TryGet(1)));
        return make_string(p.string());
    }
};

struct FileReadText {
    Value operator()(Value arg) const {
        std::ifstream in(as_path_string_unary(std::move(arg)));
        if (!in) {
            throw std::runtime_error{"FileReadText failed"};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return make_string(ss.str());
    }
};

struct FileWriteText {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "FileWriteText");
        const std::string path = to_string(*args.TryGet(0));
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            throw std::runtime_error{"FileWriteText failed"};
        }
        out << to_string(*args.TryGet(1));
        return make_string(path);
    }
};

struct FileAppendText {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "FileAppendText");
        const std::string path = to_string(*args.TryGet(0));
        std::ofstream out(path, std::ios::app);
        if (!out) {
            throw std::runtime_error{"FileAppendText failed"};
        }
        out << to_string(*args.TryGet(1));
        return make_string(path);
    }
};

struct FileReadLines {
    Value operator()(Value arg) const {
        std::ifstream in(as_path_string_unary(std::move(arg)));
        if (!in) {
            throw std::runtime_error{"FileReadLines failed"};
        }
        Array out;
        std::string line;
        while (std::getline(in, line)) {
            out.PushBack(make_string(line));
        }
        return Value{std::move(out)};
    }
};

struct FileDelete {
    Value operator()(Value arg) const {
        std::error_code ec;
        const bool removed = std::filesystem::remove(std::filesystem::path(as_path_string_unary(std::move(arg))), ec);
        throw_if_filesystem_error(ec, "FileDelete");
        return make_bool(removed);
    }
};

struct FileSize {
    Value operator()(Value arg) const {
        return make_int(static_cast<Int>(std::filesystem::file_size(std::filesystem::path(as_path_string_unary(std::move(arg))))));
    }
};

struct DirCreate {
    Value operator()(Value arg) const {
        const std::string path = as_path_string_unary(std::move(arg));
        std::filesystem::create_directories(path);
        return make_string(path);
    }
};

struct DirDelete {
    Value operator()(Value arg) const {
        std::error_code ec;
        const auto removed = std::filesystem::remove_all(std::filesystem::path(as_path_string_unary(std::move(arg))), ec);
        throw_if_filesystem_error(ec, "DirDelete");
        return make_bool(removed > 0);
    }
};

struct DirList {
    Value operator()(Value arg) const {
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(as_path_string_unary(std::move(arg))))) {
            names.push_back(entry.path().filename().string());
        }
        Array out;
        out.Reserve(names.size());
        for (const auto& n : names) {
            out.PushBack(make_string(n));
        }
        return Value{std::move(out)};
    }
};

struct DirListFull {
    Value operator()(Value arg) const {
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(as_path_string_unary(std::move(arg))))) {
            names.push_back(entry.path().string());
        }
        Array out;
        out.Reserve(names.size());
        for (const auto& n : names) {
            out.PushBack(make_string(n));
        }
        return Value{std::move(out)};
    }
};

struct OSEnv {
    Value operator()(Value arg) const {
        const std::string key = as_path_string_unary(std::move(arg));
        if (const char* val = std::getenv(key.c_str())) {
            return make_string(val);
        }
        return make_null();
    }
};

struct OSHasEnv {
    Value operator()(Value arg) const {
        const std::string key = as_path_string_unary(std::move(arg));
        return make_bool(std::getenv(key.c_str()) != nullptr);
    }
};

struct OSSetEnv {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "OSSetEnv");
        const std::string key = to_string(*args.TryGet(0));
        const std::string value = to_string(*args.TryGet(1));
#if defined(_WIN32)
        const int rc = _putenv_s(key.c_str(), value.c_str());
        if (rc != 0) {
            throw std::runtime_error{"OSSetEnv failed"};
        }
#else
        if (setenv(key.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error{"OSSetEnv failed"};
        }
#endif
        return make_string(value);
    }
};

struct OSUnsetEnv {
    Value operator()(Value arg) const {
        const std::string key = as_path_string_unary(std::move(arg));
        const bool existed = std::getenv(key.c_str()) != nullptr;
#if defined(_WIN32)
        const int rc = _putenv_s(key.c_str(), "");
        if (rc != 0) {
            throw std::runtime_error{"OSUnsetEnv failed"};
        }
#else
        if (unsetenv(key.c_str()) != 0) {
            throw std::runtime_error{"OSUnsetEnv failed"};
        }
#endif
        return make_bool(existed);
    }
};

struct OSIsWindows {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSIsWindows");
#if defined(_WIN32)
        return make_bool(true);
#else
        return make_bool(false);
#endif
    }
};

struct OSIsLinux {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSIsLinux");
#if defined(__linux__)
        return make_bool(true);
#else
        return make_bool(false);
#endif
    }
};

struct OSIsMacOS {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSIsMacOS");
#if defined(__APPLE__)
        return make_bool(true);
#else
        return make_bool(false);
#endif
    }
};

struct OSHome {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSHome");
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
            return make_string(home);
        }
        if (const char* userprofile = std::getenv("USERPROFILE");
            userprofile != nullptr && userprofile[0] != '\0') {
            return make_string(userprofile);
        }
        const char* homedrive = std::getenv("HOMEDRIVE");
        const char* homepath = std::getenv("HOMEPATH");
        if (homedrive != nullptr && homepath != nullptr && homedrive[0] != '\0' && homepath[0] != '\0') {
            return make_string(std::string(homedrive) + std::string(homepath));
        }

        std::error_code ec;
        const auto cwd = std::filesystem::current_path(ec);
        if (!ec) {
            return make_string(cwd.string());
        }
        return make_string(".");
    }
};

struct OSTempDir {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSTempDir");
        return make_string(std::filesystem::temp_directory_path().string());
    }
};

struct OSMakeTempFile {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSMakeTempFile");
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec);
        if (ec) { return make_null(); }
        for (int i = 0; i < 100; ++i) {
            const auto candidate = dir / ("fleaux_" + random_suffix() + ".tmp");
            if (!std::filesystem::exists(candidate, ec) && !ec) {
                std::ofstream out(candidate);
                if (out.good()) {
                    return make_string(candidate.string());
                }
            }
        }
        return make_null();
    }
};

struct OSMakeTempDir {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSMakeTempDir");
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec);
        if (ec) { return make_null(); }
        for (int i = 0; i < 100; ++i) {
            const auto candidate = dir / ("fleaux_" + random_suffix());
            if (std::filesystem::create_directory(candidate, ec) && !ec) {
                return make_string(candidate.string());
            }
        }
        return make_null();
    }
};

// ── File streaming builtins ───────────────────────────────────────────────────

struct FileOpen {
    // arg = (path, mode)  OR  (path,) defaults mode to "r"
    Value operator()(Value arg) const {
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
        const UInt slot = handle_registry().open(path, mode);
        const UInt gen  = handle_registry().entries[slot].generation;
        return make_handle_token(slot, gen);
    }
};

struct FileReadLine {
    // arg = handle_token   →  (handle_token, line, eof_bool)
    Value operator()(Value arg) const {
        const Value token = unwrap_singleton_arg(std::move(arg));
        auto& e = require_handle(token, "FileReadLine");
        if (e.mode.find('r') == std::string::npos) {
            throw std::runtime_error{"FileReadLine: read failed"};
        }
        std::string line;
        bool eof = false;
        if (!std::getline(e.stream, line)) {
            if (e.stream.eof()) {
                eof = true;
                line.clear();
            } else {
                throw std::runtime_error{"FileReadLine: read failed"};
            }
        }
        auto [slot, gen] = handle_id_from_value(token).value();
        return make_tuple(
            make_handle_token(slot, gen),
            make_string(std::move(line)),
            make_bool(eof)
        );
    }
};

struct FileReadChunk {
    // arg = (handle_token, nbytes)  →  (handle_token, chunk_string, eof_bool)
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "FileReadChunk");
        auto& e = require_handle(*args.TryGet(0), "FileReadChunk");
        const std::size_t nbytes = as_index(*args.TryGet(1));
        std::string buf(nbytes, '\0');
        e.stream.read(buf.data(), static_cast<std::streamsize>(nbytes));
        const std::streamsize n = e.stream.gcount();
        buf.resize(static_cast<std::size_t>(n));
        const bool eof = (n == 0 || e.stream.eof());
        auto [slot, gen] = handle_id_from_value(*args.TryGet(0)).value();
        return make_tuple(
            make_handle_token(slot, gen),
            make_string(std::move(buf)),
            make_bool(eof)
        );
    }
};

struct FileWriteChunk {
    // arg = (handle_token, data_string)  →  handle_token
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "FileWriteChunk");
        auto& e = require_handle(*args.TryGet(0), "FileWriteChunk");
        const std::string& data = as_string(*args.TryGet(1));
        e.stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!e.stream) throw std::runtime_error{"FileWriteChunk: write failed"};
        auto [slot, gen] = handle_id_from_value(*args.TryGet(0)).value();
        return make_handle_token(slot, gen);
    }
};

struct FileFlush {
    // arg = handle_token  →  handle_token
    Value operator()(Value arg) const {
        const Value token = unwrap_singleton_arg(std::move(arg));
        auto& e = require_handle(token, "FileFlush");
        e.stream.flush();
        if (!e.stream) throw std::runtime_error{"FileFlush: flush failed"};
        auto [slot, gen] = handle_id_from_value(token).value();
        return make_handle_token(slot, gen);
    }
};

struct FileClose {
    // arg = handle_token  →  Bool (true if was open, false if already closed)
    Value operator()(Value arg) const {
        const Value token = unwrap_singleton_arg(std::move(arg));
        const auto id = handle_id_from_value(token);
        if (!id) return make_bool(false);
        return make_bool(handle_registry().close(id->slot, id->gen));
    }
};

struct FileWithOpen {
    // arg = (path, mode, func_ref)  →  result of func_ref(handle_token)
    // Guarantees close even if func throws.
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 3, "FileWithOpen");
        const std::string path = as_string(*args.TryGet(0));
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
