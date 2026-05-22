#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/embed/native_bindings.hpp"
#include "fleaux/embed/vm_host.hpp"
#include "fleaux/runtime/runtime_support.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::nanoseconds;
using fleaux::bytecode::ConstValue;
using fleaux::bytecode::ExportedSymbol;
using fleaux::bytecode::ExportKind;
using fleaux::bytecode::FunctionDef;
using fleaux::bytecode::Instruction;
using fleaux::bytecode::Module;
using fleaux::bytecode::Opcode;
using fleaux::runtime::Array;
using fleaux::runtime::Int;
using fleaux::runtime::RegisteredCallable;
using fleaux::runtime::RuntimeCallable;
using fleaux::runtime::RuntimeExecutionState;
using fleaux::runtime::Value;

struct NullBuffer final : std::streambuf {
  auto overflow(const int ch) -> int override { return ch; }
};

struct BenchmarkConfig {
  std::size_t call_iterations{200000};
  std::size_t tuple_size{20000};
  std::size_t samples{7};
  std::size_t warmup{2};
};

struct BenchmarkResult {
  std::string name;
  std::size_t operation_count{0};
  Duration best{};
  Duration median{};
  Duration worst{};
};

volatile std::uint64_t g_sink = 0;

void consume_value(const Value& value) {
  if (value.HasBool()) {
    g_sink += fleaux::runtime::as_bool(value) ? 1U : 0U;
    return;
  }
  if (value.HasNumber()) {
    g_sink += static_cast<std::uint64_t>(fleaux::runtime::as_int_value(value));
    return;
  }
  if (value.HasArray()) {
    g_sink += fleaux::runtime::as_array(value).Size();
    return;
  }
  g_sink += 1U;
}

auto make_int_tuple(const std::size_t size) -> Value {
  Array values;
  values.Reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    values.PushBack(fleaux::runtime::make_int(static_cast<Int>(index)));
  }
  return Value{std::move(values)};
}

auto make_vm_loop_module() -> Module {
  Module module;
  module.constants.push_back(ConstValue{std::int64_t{0}});
  module.constants.push_back(ConstValue{std::int64_t{1}});

  FunctionDef run_loop;
  run_loop.name = "RunLoop#0";
  run_loop.arity = 1;
  run_loop.instructions = {
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kMakeUserFuncRef, .operand = 1},
      Instruction{.opcode = Opcode::kMakeUserFuncRef, .operand = 2},
      Instruction{.opcode = Opcode::kLoopCall, .operand = 0},
      Instruction{.opcode = Opcode::kReturn, .operand = 0},
  };

  FunctionDef continue_fn;
  continue_fn.name = "Continue#0";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 0},
      Instruction{.opcode = Opcode::kCmpGt, .operand = 0},
      Instruction{.opcode = Opcode::kReturn, .operand = 0},
  };

  FunctionDef step_fn;
  step_fn.name = "Step#0";
  step_fn.arity = 1;
  step_fn.instructions = {
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 1},
      Instruction{.opcode = Opcode::kSub, .operand = 0},
      Instruction{.opcode = Opcode::kReturn, .operand = 0},
  };

  module.functions.push_back(std::move(run_loop));
  module.functions.push_back(std::move(continue_fn));
  module.functions.push_back(std::move(step_fn));
  module.exports.push_back(ExportedSymbol{
      .name = "RunLoop",
      .link_name = "RunLoop#0",
      .kind = ExportKind::kFunction,
      .index = 0,
      .builtin_name = "",
  });
  return module;
}

auto make_vm_user_call_module() -> Module {
  Module module;
  module.constants.push_back(ConstValue{std::int64_t{0}});
  module.constants.push_back(ConstValue{std::int64_t{1}});

  FunctionDef bench_unary;
  bench_unary.name = "BenchUnary#0";
  bench_unary.arity = 1;
  bench_unary.instructions = {
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 0},
      Instruction{.opcode = Opcode::kCmpGt, .operand = 0},
      Instruction{.opcode = Opcode::kJumpIfNot, .operand = 12},
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kCallUserFunc, .operand = 2},
      Instruction{.opcode = Opcode::kPop, .operand = 0},
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 1},
      Instruction{.opcode = Opcode::kSub, .operand = 0},
      Instruction{.opcode = Opcode::kStoreLocal, .operand = 0},
      Instruction{.opcode = Opcode::kJump, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 0},
      Instruction{.opcode = Opcode::kReturn, .operand = 0},
  };

  FunctionDef bench_binary;
  bench_binary.name = "BenchBinary#0";
  bench_binary.arity = 1;
  bench_binary.instructions = {
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 0},
      Instruction{.opcode = Opcode::kCmpGt, .operand = 0},
      Instruction{.opcode = Opcode::kJumpIfNot, .operand = 13},
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 1},
      Instruction{.opcode = Opcode::kCallUserFuncBinary, .operand = 3},
      Instruction{.opcode = Opcode::kPop, .operand = 0},
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 1},
      Instruction{.opcode = Opcode::kSub, .operand = 0},
      Instruction{.opcode = Opcode::kStoreLocal, .operand = 0},
      Instruction{.opcode = Opcode::kJump, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 0},
      Instruction{.opcode = Opcode::kReturn, .operand = 0},
  };

  FunctionDef add_one;
  add_one.name = "AddOne#0";
  add_one.arity = 1;
  add_one.instructions = {
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kPushConst, .operand = 1},
      Instruction{.opcode = Opcode::kAdd, .operand = 0},
      Instruction{.opcode = Opcode::kReturn, .operand = 0},
  };

  FunctionDef add_pair;
  add_pair.name = "AddPair#0";
  add_pair.arity = 2;
  add_pair.instructions = {
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 0},
      Instruction{.opcode = Opcode::kLoadLocal, .operand = 1},
      Instruction{.opcode = Opcode::kAdd, .operand = 0},
      Instruction{.opcode = Opcode::kReturn, .operand = 0},
  };

  module.functions.push_back(std::move(bench_unary));
  module.functions.push_back(std::move(bench_binary));
  module.functions.push_back(std::move(add_one));
  module.functions.push_back(std::move(add_pair));
  module.exports.push_back(ExportedSymbol{
      .name = "BenchUnary",
      .link_name = "BenchUnary#0",
      .kind = ExportKind::kFunction,
      .index = 0,
      .builtin_name = "",
  });
  module.exports.push_back(ExportedSymbol{
      .name = "BenchBinary",
      .link_name = "BenchBinary#0",
      .kind = ExportKind::kFunction,
      .index = 1,
      .builtin_name = "",
  });
  return module;
}

template <typename Fn>
auto run_benchmark(const std::string& name, const std::size_t operation_count, const BenchmarkConfig& config, Fn&& fn)
    -> BenchmarkResult {
  for (std::size_t warmup_index = 0; warmup_index < config.warmup; ++warmup_index) {
    fn();
  }

  std::vector<Duration> samples;
  samples.reserve(config.samples);
  for (std::size_t sample_index = 0; sample_index < config.samples; ++sample_index) {
    const auto started = Clock::now();
    fn();
    samples.push_back(std::chrono::duration_cast<Duration>(Clock::now() - started));
  }

  std::ranges::sort(samples);
  return BenchmarkResult{
      .name = name,
      .operation_count = operation_count,
      .best = samples.front(),
      .median = samples[samples.size() / 2U],
      .worst = samples.back(),
  };
}

void print_results(const std::vector<BenchmarkResult>& results) {
  std::cout << std::left << std::setw(28) << "benchmark" << std::right << std::setw(14) << "ops" << std::setw(14)
            << "best ns/op" << std::setw(16) << "median ns/op" << std::setw(14) << "worst ns/op" << '\n';
  std::cout << std::string(86, '=') << '\n';

  for (const auto& result : results) {
    const auto to_ns_per_op = [&](const Duration duration) -> double {
      return static_cast<double>(duration.count()) /
             static_cast<double>(std::max<std::size_t>(1, result.operation_count));
    };

    std::cout << std::left << std::setw(28) << result.name << std::right << std::setw(14) << result.operation_count
              << std::setw(14) << std::fixed << std::setprecision(1) << to_ns_per_op(result.best) << std::setw(16)
              << to_ns_per_op(result.median) << std::setw(14) << to_ns_per_op(result.worst) << '\n';
  }

  std::cout << '\n' << "sink=" << g_sink << '\n';
}

auto parse_positive_size(const std::string_view text, const char* flag_name) -> std::size_t {
  if (text.empty()) {
    throw std::invalid_argument(std::string(flag_name) + " requires a value");
  }

  std::size_t value = 0;
  const std::string owned{text};
  try {
    value = std::stoull(owned);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(flag_name) + " must be a non-negative integer");
  }
  if (value == 0U) {
    throw std::invalid_argument(std::string(flag_name) + " must be greater than 0");
  }
  return value;
}

void parse_args(int argc, char** argv, BenchmarkConfig& config) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    const auto require_next = [&](const char* flag_name) -> std::string_view {
      if (index + 1 >= argc) {
        throw std::invalid_argument(std::string(flag_name) + " requires a value");
      }
      ++index;
      return argv[index];
    };

    if (arg == "--call-iters") {
      config.call_iterations = parse_positive_size(require_next("--call-iters"), "--call-iters");
    } else if (arg == "--tuple-size") {
      config.tuple_size = parse_positive_size(require_next("--tuple-size"), "--tuple-size");
    } else if (arg == "--samples") {
      config.samples = parse_positive_size(require_next("--samples"), "--samples");
    } else if (arg == "--warmup") {
      config.warmup = parse_positive_size(require_next("--warmup"), "--warmup");
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: fleaux_runtime_microbench [--call-iters N] [--tuple-size N] [--samples N] [--warmup N]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(arg));
    }
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  try {
    BenchmarkConfig config;
    parse_args(argc, argv, config);

    NullBuffer null_buffer;
    std::ostream null_output(&null_buffer);
    std::istringstream null_input;

    RuntimeExecutionState execution_state;
    fleaux::runtime::ActiveRuntimeExecutionStateScope execution_state_scope(execution_state);
    fleaux::runtime::CallableRegistryScope callable_scope;
    fleaux::runtime::ValueRegistryScope value_scope;
    fleaux::runtime::HandleRegistryScope handle_scope;
    fleaux::runtime::TaskRegistryScope task_scope;
    fleaux::runtime::RuntimeOutputStreamScope output_scope(null_output);
    fleaux::runtime::RuntimeInputStreamScope input_scope(null_input);
    fleaux::runtime::RuntimeProcessArgsScope process_args_scope({"<runtime-microbench>"});

    std::vector<BenchmarkResult> results;
    results.reserve(12);

    const Value increment_ref = fleaux::runtime::make_callable_ref(
        [](Value arg) -> Value { return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) + 1); });

    results.push_back(run_benchmark("invoke_callable_ref", config.call_iterations, config, [&]() -> void {
      Int total = 0;
      for (std::size_t index = 0; index < config.call_iterations; ++index) {
        const Value out =
            fleaux::runtime::invoke_callable_ref(increment_ref, fleaux::runtime::make_int(static_cast<Int>(index)));
        total += fleaux::runtime::as_int_value(out);
      }
      g_sink += static_cast<std::uint64_t>(total);
    }));

    const RuntimeCallable resolved_increment = fleaux::runtime::resolve_callable_ref(increment_ref);
    results.push_back(run_benchmark("resolved_callable_direct", config.call_iterations, config, [&]() -> void {
      Int total = 0;
      for (std::size_t index = 0; index < config.call_iterations; ++index) {
        const Value out = resolved_increment(fleaux::runtime::make_int(static_cast<Int>(index)));
        total += fleaux::runtime::as_int_value(out);
      }
      g_sink += static_cast<std::uint64_t>(total);
    }));

    const Value values = make_int_tuple(config.tuple_size);
    const Value add_one = fleaux::runtime::make_callable_ref(
        [](Value arg) -> Value { return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) + 1); });
    results.push_back(run_benchmark("TupleMap", config.tuple_size, config, [&]() -> void {
      const Value mapped = fleaux::runtime::TupleMap(fleaux::runtime::make_tuple(values, add_one));
      consume_value(mapped);
    }));

    const Value sum_pair = fleaux::runtime::make_callable_ref([](Value arg) -> Value {
      const auto& pair = fleaux::runtime::as_array(arg);
      return fleaux::runtime::make_int(fleaux::runtime::as_int_value(*pair.TryGet(0)) +
                                       fleaux::runtime::as_int_value(*pair.TryGet(1)));
    });
    results.push_back(run_benchmark("TupleReduce", config.tuple_size, config, [&]() -> void {
      const Value reduced =
          fleaux::runtime::TupleReduce(fleaux::runtime::make_tuple(values, fleaux::runtime::make_int(0), sum_pair));
      consume_value(reduced);
    }));

    const Value builtin_style_add = fleaux::runtime::make_callable_ref(RegisteredCallable{
        .unary = fleaux::runtime::Add,
        .binary = [](Value lhs, Value rhs) -> Value { return fleaux::runtime::AddBinary(lhs, rhs); },
    });
    results.push_back(run_benchmark("TupleReduce builtin-style Add", config.tuple_size, config, [&]() -> void {
      const Value reduced = fleaux::runtime::TupleReduce(
          fleaux::runtime::make_tuple(values, fleaux::runtime::make_int(0), builtin_style_add));
      consume_value(reduced);
    }));

    const Value continue_func = fleaux::runtime::make_callable_ref(
        [](Value arg) -> Value { return fleaux::runtime::make_bool(fleaux::runtime::as_int_value(arg) > 0); });
    const Value step_func = fleaux::runtime::make_callable_ref(
        [](Value arg) -> Value { return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) - 1); });
    results.push_back(run_benchmark("Runtime::Loop", config.call_iterations, config, [&]() -> void {
      const Value loop_result = fleaux::runtime::Loop(fleaux::runtime::make_tuple(
          fleaux::runtime::make_int(static_cast<Int>(config.call_iterations)), continue_func, step_func));
      consume_value(loop_result);
    }));

    const Module vm_loop_module = make_vm_loop_module();
    const Module vm_user_call_module = make_vm_user_call_module();
    const fleaux::vm::Runtime runtime;

    fleaux::embed::NativeBindingRegistry binding_registry;
    auto unary_native_registration = binding_registry.register_callable(fleaux::embed::NativeBinding{
        .symbol = "Host.SumUnary",
        .signature = fleaux::embed::BindingSignature{},
        .callable = [](const fleaux::embed::BindingContext&,
                       const fleaux::embed::VmValue& args) -> fleaux::embed::NativeInvokeResult {
          const auto& pair = fleaux::runtime::as_array(args);
          return fleaux::runtime::make_int(fleaux::runtime::as_int_value(*pair.TryGet(0)) +
                                           fleaux::runtime::as_int_value(*pair.TryGet(1)));
        },
    });
    if (!unary_native_registration) {
      throw std::runtime_error(unary_native_registration.error().message);
    }

    auto binary_native_registration = binding_registry.register_callable(
        fleaux::embed::NativeBinding{
            .symbol = "Host.SumBinary",
            .signature = fleaux::embed::BindingSignature{},
            .callable = [](const fleaux::embed::BindingContext&,
                           const fleaux::embed::VmValue& args) -> fleaux::embed::NativeInvokeResult {
              const auto& pair = fleaux::runtime::as_array(args);
              return fleaux::runtime::make_int(fleaux::runtime::as_int_value(*pair.TryGet(0)) +
                                               fleaux::runtime::as_int_value(*pair.TryGet(1)));
            },
        },
        [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& lhs,
           const fleaux::embed::VmValue& rhs) -> fleaux::embed::NativeInvokeResult {
          return fleaux::runtime::make_int(fleaux::runtime::as_int_value(lhs) + fleaux::runtime::as_int_value(rhs));
        });
    if (!binary_native_registration) {
      throw std::runtime_error(binary_native_registration.error().message);
    }

    fleaux::embed::VmHostConfig host_config;
    host_config.binding_registry = &binding_registry;
    fleaux::embed::VmHost host(host_config);
    std::vector<Value> host_native_lhs_inputs;
    std::vector<Value> host_native_tuple_args;
    host_native_lhs_inputs.reserve(config.call_iterations);
    host_native_tuple_args.reserve(config.call_iterations);
    const Value host_native_one = fleaux::runtime::make_int(1);
    for (std::size_t index = 0; index < config.call_iterations; ++index) {
      Value lhs = fleaux::runtime::make_int(static_cast<Int>(index));
      host_native_lhs_inputs.push_back(lhs);
      host_native_tuple_args.push_back(fleaux::runtime::make_tuple(lhs, host_native_one));
    }
    results.push_back(run_benchmark("VM CallUserFunc unary", config.call_iterations, config, [&]() -> void {
      auto result = runtime.invoke_symbol(vm_user_call_module, "BenchUnary",
                                          fleaux::runtime::make_int(static_cast<Int>(config.call_iterations)));
      if (!result) {
        throw std::runtime_error(result.error().message);
      }
      consume_value(*result);
    }));

    results.push_back(run_benchmark("VmHost call_native tuple", config.call_iterations, config, [&]() -> void {
      Int total = 0;
      for (std::size_t index = 0; index < config.call_iterations; ++index) {
        auto result = host.call_native("Host.SumUnary", host_native_tuple_args[index]);
        if (!result) {
          throw std::runtime_error(result.error().message);
        }
        total += fleaux::runtime::as_int_value(*result);
      }
      g_sink += static_cast<std::uint64_t>(total);
    }));

    results.push_back(run_benchmark("VmHost native binary fallback", config.call_iterations, config, [&]() -> void {
      Int total = 0;
      for (std::size_t index = 0; index < config.call_iterations; ++index) {
        auto result = host.call_native_binary("Host.SumUnary", host_native_lhs_inputs[index], host_native_one);
        if (!result) {
          throw std::runtime_error(result.error().message);
        }
        total += fleaux::runtime::as_int_value(*result);
      }
      g_sink += static_cast<std::uint64_t>(total);
    }));

    results.push_back(run_benchmark("VmHost native binary direct", config.call_iterations, config, [&]() -> void {
      Int total = 0;
      for (std::size_t index = 0; index < config.call_iterations; ++index) {
        auto result = host.call_native_binary("Host.SumBinary", host_native_lhs_inputs[index], host_native_one);
        if (!result) {
          throw std::runtime_error(result.error().message);
        }
        total += fleaux::runtime::as_int_value(*result);
      }
      g_sink += static_cast<std::uint64_t>(total);
    }));

    results.push_back(run_benchmark("VM CallUserFunc binary", config.call_iterations, config, [&]() -> void {
      auto result = runtime.invoke_symbol(vm_user_call_module, "BenchBinary",
                                          fleaux::runtime::make_int(static_cast<Int>(config.call_iterations)));
      if (!result) {
        throw std::runtime_error(result.error().message);
      }
      consume_value(*result);
    }));

    results.push_back(run_benchmark("VM LoopCall", config.call_iterations, config, [&]() -> void {
      auto result = runtime.invoke_symbol(vm_loop_module, "RunLoop",
                                          fleaux::runtime::make_int(static_cast<Int>(config.call_iterations)));
      if (!result) {
        throw std::runtime_error(result.error().message);
      }
      consume_value(*result);
    }));

    print_results(results);
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "runtime microbenchmark failed: " << ex.what() << '\n';
    return 1;
  }
}
