#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/frontend/box.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/runtime/value.hpp"
#include "fleaux/vm/runtime.hpp"

namespace fleaux::embed {

class NativeBindingRegistry;
class DynamicLoader;

enum class HostErrorCategory {
  kParse,
  kAnalysis,
  kBytecode,
  kRuntime,
  kIo,
  kBinding,
  kInternal,
};

struct HostError {
  HostErrorCategory category{HostErrorCategory::kInternal};
  std::string message{};
  std::optional<std::string> hint{std::nullopt};
  std::optional<frontend::diag::SourceSpan> span{std::nullopt};
};

template <typename T>
using HostResult = tl::expected<T, HostError>;

using VmValue = runtime::Value;
using VmResult = HostResult<VmValue>;
using HostStatus = HostResult<void>;
using OutputSink = std::function<void(std::string_view)>;

// VmHost is single-threaded by default. If a host shares one instance across
// threads, external synchronization is required for all mutating operations
// and for symbol invocation. The active Fleaux module snapshot is owned by the
// VmHost instance and is cleared by reset().
struct VmHostConfig {
  std::vector<std::string> process_args{};
  std::vector<std::filesystem::path> import_roots{};
  vm::RuntimeCompileOptions compile_options{};
  std::optional<OutputSink> stdout_sink{std::nullopt};
  std::optional<OutputSink> stderr_sink{std::nullopt};
  NativeBindingRegistry* binding_registry{nullptr};
  bool preload_std{true};
};

class VmHost {
public:
  // Loading a new module or replacing the dynamic loader may invalidate prior
  // plugin-owned callables after unregister-before-unload has run.
  explicit VmHost(const VmHostConfig& config = {});
  VmHost(const VmHost& other);
  VmHost(VmHost&& other) noexcept;
  auto operator=(const VmHost& other) -> VmHost&;
  auto operator=(VmHost&& other) noexcept -> VmHost&;
  ~VmHost();

  [[nodiscard]] auto run_file(const std::filesystem::path& path) const -> VmResult;
  [[nodiscard]] auto run_source(std::string_view module_name, std::string_view source_text) const -> VmResult;
  [[nodiscard]] auto call_native(std::string_view qualified_symbol, const VmValue& args) const -> VmResult;
  [[nodiscard]] auto call_fleaux(std::string_view qualified_symbol, const VmValue& args) const -> VmResult;
  [[nodiscard]] auto call(std::string_view qualified_symbol, const VmValue& args) const -> VmResult;
  [[nodiscard]] auto load_binding_module(const std::filesystem::path& module_path) -> HostStatus;

  void set_binding_registry(NativeBindingRegistry* registry);
  [[nodiscard]] auto binding_registry() const -> NativeBindingRegistry*;
  void set_dynamic_loader(std::unique_ptr<DynamicLoader> loader);

  void add_import_root(const std::filesystem::path& root);
  void clear_import_roots();
  void reset();

private:
  struct Impl;
  mutable frontend::Box<Impl> impl_;
};

}  // namespace fleaux::embed


