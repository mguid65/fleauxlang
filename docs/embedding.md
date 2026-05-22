# Embedding Fleaux from C++

This guide covers the host-facing runtime surface in `core/include/fleaux/embed/`.
It is intentionally short and practical:

1. run Fleaux source or files from a host process
2. register in-process native bindings
3. load dynamic binding plugins
4. link a downstream CMake project against the installed embedding package

## Current embedding contract

The current embedding layer is centered on `fleaux::embed::VmHost` and `fleaux::embed::NativeBindingRegistry`.
Important behavior to know up front:

- `VmHost` is **move-only**
- `VmHost` execution and dispatch entrypoints are intentionally **non-const**
    - `run_source(...)`, `run_file(...)`, `call_native(...)`, `call_fleaux(...)`, and `call(...)` mutate host-owned execution state
    - native binding callbacks receive mutable access to the active host through `BindingContext::host`
- `VmHostConfig.stdout_sink` receives Fleaux program/runtime text output that would otherwise go to standard output
- `VmHostConfig.stderr_sink` optionally receives rendered `HostError` diagnostics from failing public `VmHost` entrypoints
    - the same `HostError` is still returned to the caller
    - this is currently a host-diagnostic channel, not a second VM text-output stream
- `VmHostConfig.process_args` is applied per `run_*` / `call_fleaux(...)` execution
    - the configured trailing arguments are presented through `Std.GetArgs`
    - they are now scoped to the active runtime invocation instead of permanently mutating ambient runtime args between host calls
- `VmHostConfig.stdin_stream` optionally routes `Std.Input` through a host-provided input stream
    - when null, runtime input falls back to the process stdin stream
    - when set, the configured stream is scoped to the active execution/call rather than replacing ambient runtime input globally
- `VmHost::call(...)` is a **combined dispatcher**
    - it calls a loaded Fleaux export if only the Fleaux module exposes the symbol
    - it calls a native binding if only the binding registry exposes the symbol
    - it returns an error if both surfaces expose the same symbol name
- `BindingSignature` is currently **descriptive metadata only**
    - the host does **not** enforce its declared arity before native dispatch
    - native bindings receive the raw `VmValue` argument and may validate it themselves

## Installed CMake package

After installing `core/`, downstream consumers can use:

```cmake
find_package(fleaux CONFIG REQUIRED)
add_executable(my_host main.cpp)
target_link_libraries(my_host PRIVATE fleaux::embed)
```

The installed package currently exports these main targets:

- `fleaux::embed`
- `fleaux::vm`
- `fleaux::bytecode`
- `fleaux::frontend_lowering`
- `fleaux::frontend_parser`
- `fleaux::frontend_diagnostics`
- `fleaux::common`

Most embedders should only need `fleaux::embed`.

Downstream CMake must also be able to find `PCRE2` because the exported package resolves it as a dependency at package-load time.

If the downstream project uses a single-config generator such as Ninja or Unix Makefiles, also set `CMAKE_BUILD_TYPE` during configure so the current `PCRE2` package config can resolve the matching configuration.

## Minimal host example

```c++
#include <iostream>
#include <string_view>

#include "fleaux/embed/vm_host.hpp"
#include "fleaux/runtime/value.hpp"

int main() {
  std::string captured_stdout;
  std::string captured_stderr;
  
  fleaux::embed::VmHostConfig config;
  config.stdout_sink = [&captured_stdout](std::string_view text) {
    captured_stdout.append(text);
  };
  config.stderr_sink = [&captured_stderr](std::string_view text) {
    captured_stderr.append(text);
  };
  fleaux::embed::VmHost host(config);
  
  const auto run_result = host.run_source("memory/host_demo.fleaux", R"(
import Std;
let App.Double(x: Int64): Int64 = (x, x) -> Std.Add;
("Hello from embedded Fleaux") -> Std.Println;
)");

  if (!run_result) {
    std::cerr << run_result.error().message << '\n';
    return 1;
  }
  
  const auto called = host.call("App.Double", fleaux::runtime::make_int(21));
  if (!called) {
    std::cerr << called.error().message << '\n';
    return 1;
  }
  
  std::cout << "captured stdout: " << captured_stdout;
  std::cout << "captured stderr: " << captured_stderr;
  std::cout << "App.Double(21) = " << fleaux::runtime::as_int_value(*called) << '\n';
  
  return 0;
}
```

### How `stderr_sink` works

`stderr_sink` is intentionally narrower than `stdout_sink`:

- `stdout_sink` receives text produced by the running Fleaux program, such as `Std.Println`
- `stderr_sink` receives rendered diagnostics when a public `VmHost` method returns a `HostError`

If `stderr_sink` is not configured, the error is still returned as a `HostError`, but `VmHost` does not mirror it to the process stderr stream on its own.

That means these failures are routed to `stderr_sink`:

- parse/lowering/analysis failures from `run_source(...)`
- bytecode/runtime failures from `run_source(...)`, `run_file(...)`, or `call_fleaux(...)`
- binding/dispatch failures from `call_native(...)`, `call(...)`, or `load_binding_module(...)`

The diagnostic text uses the same structured formatting helpers as the CLI layer, so parse/analysis errors include source locations and hints when that information is available.

At the moment, the VM runtime does **not** expose a separate program-level stderr stream. So `stderr_sink` should be read as an embedder-facing diagnostic channel, not as a capture stream for a Fleaux `Std.Err`-style output surface.

Example failure capture:

```c++
std::string captured_stderr;

fleaux::embed::VmHostConfig config;
config.stderr_sink = [&](std::string_view text) {
  captured_stderr.append(text);
};

fleaux::embed::VmHost host(config);
const auto result = host.call_native("Host.Missing", fleaux::runtime::make_null());

if (!result) {
  // Structured error for programmatic handling.
  const auto& error = result.error();
  // Mirrored human-readable diagnostic text for logging/UI.
  std::cout << captured_stderr;
}
```

## In-process native binding example

Use `NativeBindingRegistry` when you want the host process to expose C++ callables directly.

```c++
#include <iostream>
#include <string_view>

#include "fleaux/embed/native_bindings.hpp"
#include "fleaux/embed/vm_host.hpp"
#include "fleaux/runtime/value.hpp"

int main() {
  fleaux::embed::NativeBindingRegistry registry;
  auto registered = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "Host.Echo",
      .signature = fleaux::embed::BindingSignature{
          .arity = fleaux::embed::ArityRange{.min = 1, .max = 1},
          .description = "Returns the raw incoming VmValue",
      },
      .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
          -> fleaux::embed::NativeInvokeResult {
        return args;
      },
  });
  
  if (!registered) {
    std::cerr << registered.error().message << '\n';
    return 1;
  }
  
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);
  
  const auto result = host.call_native("Host.Echo", fleaux::runtime::make_int(42));
  if (!result) {
    std::cerr << result.error().message << '\n';
    return 1;
  }
  std::cout << fleaux::runtime::as_int_value(*result) << '\n';
  return 0;
}
```

### Notes on native binding arguments

Even though the example above declares an arity range, `VmHost::call_native(...)` still forwards the raw `VmValue`
directly to the callable.
That means these are the binding author's responsibility:

- deciding whether an argument should be a tuple, scalar, array, or null
- validating shape/arity inside the callable
- returning a `HostError` if the input is not acceptable

For in-process hosts that have a true binary native operation, `NativeBindingRegistry` also supports an optional binary
specialization alongside the normal unary raw-`VmValue` callable. When present, `VmHost::call_native_binary(...)` uses
that binary specialization directly. If no binary specialization was registered, `call_native_binary(...)` falls back to
calling the unary binding with the usual 2-tuple argument shape.

Native bindings also receive a non-owning `BindingContext::host` reference for the duration of the callback. That reference refers to the active `VmHost` instance and is intentionally mutable, so bindings may make additional host calls while they execute.

`VmHostConfig::binding_registry` is likewise a nullable non-owning observer. If you provide one, it must outlive any `VmHost` calls that dispatch native bindings or load binding modules.

If you inspect registry metadata directly with `NativeBindingRegistry::find_callable(...)`, it returns an optional borrowed reference to registry-owned storage. That borrowed reference is invalidated by any later registration, unregistration, or clear operation on that registry.

## Dynamic plugin example

Dynamic binding modules use the tiny plugin ABI from `fleaux/embed/binding_plugin.hpp` and the helper macros/functions
from `fleaux/embed/fleaux_binding.hpp`.
Example plugin:

```c++
#include "fleaux/embed/fleaux_binding.hpp"
#include "fleaux/runtime/value.hpp"

namespace {

auto register_bindings(const fleaux::embed::BindingPluginHostApi* host_api,
                       const char** error_message) -> fleaux::embed::BindingPluginStatus {
  if (!fleaux::embed::require_abi(host_api, error_message)) {
    return fleaux::embed::BindingPluginStatus::kAbiMismatch;
  }
  
  if (!fleaux::embed::require_host_api(host_api, error_message)) {
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }
  
  const auto registered = fleaux::embed::register_native(host_api, fleaux::embed::NativeBinding{
      .symbol = "Host.PluginEcho",
      .signature = fleaux::embed::BindingSignature{
          .arity = fleaux::embed::ArityRange{.min = 1, .max = 1},
          .description = "Echo binding exported from a plugin",
      },
      .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
          -> fleaux::embed::NativeInvokeResult {
        return args;
      },
  });
  
  if (!registered) {
    if (error_message != nullptr) {
      *error_message = "failed to register Host.PluginEcho";
    }
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }
  
  return fleaux::embed::BindingPluginStatus::kOk;
}
}  // namespace

FLEAUX_BINDING_ENTRYPOINT(register_bindings);
```

Example host-side loading:

```c++
#include <filesystem>
#include <iostream>

#include "fleaux/embed/native_bindings.hpp"
#include "fleaux/embed/vm_host.hpp"
#include "fleaux/runtime/value.hpp"

int main() {
  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);
  
  const auto loaded = host.load_binding_module(std::filesystem::path{"my_plugin.dll"});
  if (!loaded) {
    std::cerr << loaded.error().message << '\n';
    return 1;
  }
  
  const auto result = host.call_native("Host.PluginEcho", fleaux::runtime::make_int(7));
  if (!result) {
    std::cerr << result.error().message << '\n';
    return 1;
  }
  
  std::cout << fleaux::runtime::as_int_value(*result) << '\n';
  return 0;
}
```

## Installing Fleaux for embedding

A minimal native install looks like this:

```bash
cd core
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
cmake --install build-release --prefix /tmp/fleaux-install
```

The installed development package includes:

- public headers under `include/`
- the exported `fleaux` CMake package under `lib/cmake/fleaux/`
- the native embedding/static libraries
- bundled `tl` and `data_tree` headers used by the public embedding surface

## Current limitations

The embedding surface is usable, but it is still intentionally narrow:

- `VmHost` currently owns only the most recent loaded Fleaux module snapshot
- `call(...)` can report ambiguity if the same symbol exists in both Fleaux and native surfaces
- `BindingSignature` metadata is not yet enforced by the host runtime
- plugin ABI growth is versioned conservatively through a single registration entrypoint
