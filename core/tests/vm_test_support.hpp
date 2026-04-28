#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace fleaux::tests {

struct CurrentPathScope {
  explicit CurrentPathScope(const std::filesystem::path& path) : previous(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }

  ~CurrentPathScope() { std::filesystem::current_path(previous); }

  std::filesystem::path previous;
};

struct ScopedEnvVar {
  ScopedEnvVar(std::string env_key, const std::string& value) : key(std::move(env_key)) {
    if (const char* existing = std::getenv(key.c_str()); existing != nullptr) { previous = std::string(existing); }

#if defined(_WIN32)
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 1);
#endif
  }

  ~ScopedEnvVar() {
    if (previous.has_value()) {
#if defined(_WIN32)
      _putenv_s(key.c_str(), previous->c_str());
#else
      setenv(key.c_str(), previous->c_str(), 1);
#endif
      return;
    }

#if defined(_WIN32)
    _putenv_s(key.c_str(), "");
#else
    unsetenv(key.c_str());
#endif
  }

  std::string key;
  std::optional<std::string> previous;
};

inline auto write_text_file(const std::filesystem::path& path, const std::string_view source) -> void {
  std::ofstream out(path);
  REQUIRE(out.good());
  out.write(source.data(), static_cast<std::streamsize>(source.size()));
  REQUIRE(out.good());
}

enum class ImportContractOutcomeClass {
  kSuccess,
  kImportUnresolved,
  kImportCycle,
  kTypeMismatch,
  kUnresolvedSymbol,
  kOther,
};

struct ImportContractFixtureFile {
  std::string name;
  std::string source;
};

struct ImportContractCase {
  std::string name;
  std::string entry_file;
  std::vector<ImportContractFixtureFile> files;
  ImportContractOutcomeClass expected_outcome;
  std::vector<std::string> expected_fragments;
  bool execute_vm{false};
};

[[nodiscard]] inline auto classify_import_contract_message(const std::string& text) -> ImportContractOutcomeClass {
  if (text.find("import-unresolved:") != std::string::npos) { return ImportContractOutcomeClass::kImportUnresolved; }
  if (text.find("import-cycle:") != std::string::npos) { return ImportContractOutcomeClass::kImportCycle; }
  if (text.find("Type mismatch in call target arguments") != std::string::npos) {
    return ImportContractOutcomeClass::kTypeMismatch;
  }
  if (text.find("Unresolved symbol") != std::string::npos) { return ImportContractOutcomeClass::kUnresolvedSymbol; }
  return ImportContractOutcomeClass::kOther;
}

struct PoisonedSymbolicStdFallbacks {
  std::filesystem::path env_std_path;
};

inline auto write_poisoned_symbolic_std_fallbacks(const std::filesystem::path& temp_dir)
    -> PoisonedSymbolicStdFallbacks {
  std::filesystem::create_directories(temp_dir / "stdlib");

  const auto poisoned_env_std = temp_dir / "poisoned_env_std.fleaux";
  write_text_file(temp_dir / "stdlib" / "Std.fleaux", "this poisoned stdlib fallback must never be parsed\n");
  write_text_file(poisoned_env_std, "this poisoned FLEAUX_STD_PATH fallback must never be parsed\n");

  return PoisonedSymbolicStdFallbacks{
      .env_std_path = poisoned_env_std,
  };
}

}  // namespace fleaux::tests

