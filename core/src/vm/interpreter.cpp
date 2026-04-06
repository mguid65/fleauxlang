#include "fleaux/vm/interpreter.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux_runtime.hpp"

namespace fleaux::vm {
namespace {

using fleaux::frontend::diag::SourceSpan;
using fleaux::frontend::ir::IRCallTarget;
using fleaux::frontend::ir::IRExprPtr;
using fleaux::frontend::ir::IRFlowExpr;
using fleaux::frontend::ir::IRNameRef;
using fleaux::frontend::ir::IRProgram;
using fleaux::frontend::ir::IRTupleExpr;
using fleaux::runtime::RuntimeCallable;
using fleaux::runtime::Value;

std::string read_text_file(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in) {
    return {};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string let_key(const std::optional<std::string>& qualifier, const std::string& name) {
  if (!qualifier.has_value()) {
    return name;
  }
  return *qualifier + "." + name;
}

InterpretError make_error(const std::string& message,
                          const std::optional<std::string>& hint = std::nullopt,
                          const std::optional<SourceSpan>& span = std::nullopt) {
  return InterpretError{
      .message = message,
      .hint = hint,
      .span = span,
  };
}

#define FLEAUX_VM_BUILTINS(X)                \
  X("Std.Printf", Printf)                   \
  X("Std.Println", Println)                 \
  X("Std.GetArgs", GetArgs)                 \
  X("Std.ToString", ToString)               \
  X("Std.ToNum", ToNum)                     \
  X("Std.Input", Input)                     \
  X("Std.Exit", Exit)                       \
  X("Std.Select", Select)                   \
  X("Std.Apply", Apply)                     \
  X("Std.Branch", Branch)                   \
  X("Std.Loop", Loop)                       \
  X("Std.LoopN", LoopN)                     \
  X("Std.Wrap", Wrap)                       \
  X("Std.Unwrap", Unwrap)                   \
  X("Std.UnaryPlus", UnaryPlus)             \
  X("Std.UnaryMinus", UnaryMinus)           \
  X("Std.Add", Add)                         \
  X("Std.Subtract", Subtract)               \
  X("Std.Multiply", Multiply)               \
  X("Std.Divide", Divide)                   \
  X("Std.Mod", Mod)                         \
  X("Std.Pow", Pow)                         \
  X("Std.Sqrt", Sqrt)                       \
  X("Std.Sin", Sin)                         \
  X("Std.Cos", Cos)                         \
  X("Std.Tan", Tan)                         \
  X("Std.Equal", Equal)                     \
  X("Std.NotEqual", NotEqual)               \
  X("Std.LessThan", LessThan)               \
  X("Std.GreaterThan", GreaterThan)         \
  X("Std.GreaterOrEqual", GreaterOrEqual)   \
  X("Std.LessOrEqual", LessOrEqual)         \
  X("Std.Not", Not)                         \
  X("Std.And", And)                         \
  X("Std.Or", Or)                           \
  X("Std.ElementAt", ElementAt)             \
  X("Std.Length", Length)                   \
  X("Std.Take", Take)                       \
  X("Std.Drop", Drop)                       \
  X("Std.Slice", Slice)                     \
  X("Std.Math.Floor", MathFloor)            \
  X("Std.Math.Ceil", MathCeil)              \
  X("Std.Math.Abs", MathAbs)                \
  X("Std.Math.Log", MathLog)                \
  X("Std.Math.Clamp", MathClamp)            \
  X("Std.Math.Sqrt", Sqrt)                  \
  X("Std.Math.Sin", Sin)                    \
  X("Std.Math.Cos", Cos)                    \
  X("Std.Math.Tan", Tan)                    \
  X("Std.String.Upper", StringUpper)        \
  X("Std.String.Lower", StringLower)        \
  X("Std.String.Trim", StringTrim)          \
  X("Std.String.TrimStart", StringTrimStart)\
  X("Std.String.TrimEnd", StringTrimEnd)    \
  X("Std.String.Split", StringSplit)        \
  X("Std.String.Join", StringJoin)          \
  X("Std.String.Replace", StringReplace)    \
  X("Std.String.Contains", StringContains)  \
  X("Std.String.StartsWith", StringStartsWith) \
  X("Std.String.EndsWith", StringEndsWith)  \
  X("Std.String.Length", StringLength)      \
  X("Std.OS.Cwd", Cwd)                      \
  X("Std.OS.Env", OSEnv)                    \
  X("Std.OS.HasEnv", OSHasEnv)              \
  X("Std.OS.SetEnv", OSSetEnv)              \
  X("Std.OS.UnsetEnv", OSUnsetEnv)          \
  X("Std.OS.IsWindows", OSIsWindows)        \
  X("Std.OS.IsLinux", OSIsLinux)            \
  X("Std.OS.IsMacOS", OSIsMacOS)            \
  X("Std.OS.Home", OSHome)                  \
  X("Std.OS.TempDir", OSTempDir)            \
  X("Std.OS.MakeTempFile", OSMakeTempFile)  \
  X("Std.OS.MakeTempDir", OSMakeTempDir)    \
  X("Std.Path.Join", PathJoin)              \
  X("Std.Path.Normalize", PathNormalize)    \
  X("Std.Path.Basename", PathBasename)      \
  X("Std.Path.Dirname", PathDirname)        \
  X("Std.Path.Exists", PathExists)          \
  X("Std.Path.IsFile", PathIsFile)          \
  X("Std.Path.IsDir", PathIsDir)            \
  X("Std.Path.Absolute", PathAbsolute)      \
  X("Std.Path.Extension", PathExtension)    \
  X("Std.Path.Stem", PathStem)              \
  X("Std.Path.WithExtension", PathWithExtension) \
  X("Std.Path.WithBasename", PathWithBasename) \
  X("Std.File.ReadText", FileReadText)      \
  X("Std.File.WriteText", FileWriteText)    \
  X("Std.File.AppendText", FileAppendText)  \
  X("Std.File.ReadLines", FileReadLines)    \
  X("Std.File.Delete", FileDelete)          \
  X("Std.File.Size", FileSize)              \
  X("Std.File.Open", FileOpen)              \
  X("Std.File.ReadLine", FileReadLine)      \
  X("Std.File.ReadChunk", FileReadChunk)    \
  X("Std.File.WriteChunk", FileWriteChunk)  \
  X("Std.File.Flush", FileFlush)            \
  X("Std.File.Close", FileClose)            \
  X("Std.File.WithOpen", FileWithOpen)      \
  X("Std.Dir.Create", DirCreate)            \
  X("Std.Dir.Delete", DirDelete)            \
  X("Std.Dir.List", DirList)                \
  X("Std.Dir.ListFull", DirListFull)        \
  X("Std.Tuple.Append", TupleAppend)        \
  X("Std.Tuple.Prepend", TuplePrepend)      \
  X("Std.Tuple.Reverse", TupleReverse)      \
  X("Std.Tuple.Contains", TupleContains)    \
  X("Std.Tuple.Zip", TupleZip)              \
  X("Std.Tuple.Map", TupleMap)              \
  X("Std.Tuple.Filter", TupleFilter)        \
  X("Std.Tuple.Sort", TupleSort)            \
  X("Std.Tuple.Unique", TupleUnique)        \
  X("Std.Tuple.Min", TupleMin)              \
  X("Std.Tuple.Max", TupleMax)              \
  X("Std.Tuple.Reduce", TupleReduce)        \
  X("Std.Tuple.FindIndex", TupleFindIndex)  \
  X("Std.Tuple.Any", TupleAny)              \
  X("Std.Tuple.All", TupleAll)              \
  X("Std.Tuple.Range", TupleRange)          \
  X("Std.Dict.Create", DictCreate)          \
  X("Std.Dict.Set", DictSet)                \
  X("Std.Dict.Get", DictGet)                \
  X("Std.Dict.GetDefault", DictGetDefault)  \
  X("Std.Dict.Contains", DictContains)      \
  X("Std.Dict.Delete", DictDelete)          \
  X("Std.Dict.Keys", DictKeys)              \
  X("Std.Dict.Values", DictValues)          \
  X("Std.Dict.Entries", DictEntries)        \
  X("Std.Dict.Clear", DictClear)            \
  X("Std.Dict.Length", DictLength)

RuntimeCallable make_constant_callable(const double value) {
  return [value](Value) -> Value { return fleaux::runtime::make_float(value); };
}

const std::unordered_map<std::string, RuntimeCallable>& builtin_callables() {
  static const std::unordered_map<std::string, RuntimeCallable> map = [] {
    std::unordered_map<std::string, RuntimeCallable> out;
#define FLEAUX_INSERT_BUILTIN(name_literal, node_type)                                  \
  out.emplace(name_literal, [](Value arg) -> Value {                                    \
    return fleaux::runtime::node_type{}(std::move(arg));                                \
  });
    FLEAUX_VM_BUILTINS(FLEAUX_INSERT_BUILTIN)
#undef FLEAUX_INSERT_BUILTIN

    out.emplace("Std.Pi", make_constant_callable(3.141592653589793238462643383279502884));
    out.emplace("Std.TwoPi", make_constant_callable(6.283185307179586476925286766559005768));
    out.emplace("Std.Tau", make_constant_callable(6.283185307179586476925286766559005768));
    out.emplace("Std.HalfPi", make_constant_callable(1.570796326794896619231321691639751442));
    out.emplace("Std.E", make_constant_callable(2.718281828459045235360287471352662497));
    out.emplace("Std.Phi", make_constant_callable(1.618033988749894848204586834365638117));
    out.emplace("Std.RootTwo", make_constant_callable(1.414213562373095048801688724209698078));
    out.emplace("Std.RootThree", make_constant_callable(1.732050807568877293527446341505872366));
    out.emplace("Std.LnTwo", make_constant_callable(0.693147180559945309417232121458176568));
    out.emplace("Std.Log2E", make_constant_callable(1.442695040888963407359924681001892137));
    return out;
  }();
  return map;
}

RuntimeCallable resolve_std_callable_or_throw(const std::string& key,
                                              const std::optional<SourceSpan>& span) {
  const auto& map = builtin_callables();
  const auto it = map.find(key);
  if (it == map.end()) {
    throw std::runtime_error("Unsupported VM builtin target: '" + key + "'.");
  }
  (void)span;
  return it->second;
}

std::filesystem::path resolve_import_source(const std::filesystem::path& current_source,
                                            const std::string& module_name) {
  if (module_name == "StdBuiltins") {
    return {};
  }

  if (const auto local = current_source.parent_path() / (module_name + ".fleaux"); std::filesystem::exists(local)) {
    return std::filesystem::weakly_canonical(local);
  }

  if (module_name == "Std") {
    if (const auto workspace_std = current_source.parent_path().parent_path() / "Std.fleaux"; std::filesystem::exists(workspace_std)) {
      return std::filesystem::weakly_canonical(workspace_std);
    }
  }

  return {};
}

tl::expected<IRProgram, InterpretError> parse_and_lower(const std::filesystem::path& source_file) {
  const auto source_text = read_text_file(source_file);
  if (source_text.empty()) {
    return tl::unexpected(make_error(
        "Failed to read source file.",
        "Check the file path and ensure it is not empty."));
  }

  const frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_file.string());
  if (!parsed) {
    return tl::unexpected(make_error(parsed.error().message, parsed.error().hint, parsed.error().span));
  }

  const frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  if (!lowered) {
    return tl::unexpected(make_error(lowered.error().message, lowered.error().hint, lowered.error().span));
  }

  return lowered.value();
}

tl::expected<IRProgram, InterpretError> collect_program(
    const std::filesystem::path& source_file,
    std::unordered_map<std::string, IRProgram>& cache,
    std::unordered_set<std::string>& in_progress) {
  const std::string key = std::filesystem::weakly_canonical(source_file).string();
  if (cache.contains(key)) {
    return cache[key];
  }

  if (in_progress.contains(key)) {
    return tl::unexpected(make_error(
        "Cyclic import detected while executing in VM mode.",
        "Break the cycle by moving shared definitions into a third module."));
  }

  in_progress.insert(key);
  auto current_result = parse_and_lower(source_file);
  if (!current_result) {
    in_progress.erase(key);
    return tl::unexpected(current_result.error());
  }

  IRProgram merged = current_result.value();
  std::unordered_set<std::string> seen_symbols;
  for (const auto& let : merged.lets) {
    seen_symbols.insert(let_key(let.qualifier, let.name));
  }

  std::vector<frontend::ir::IRExprStatement> imported_exprs;
  std::vector<frontend::ir::IRLet> imported_lets;

  for (const auto&[module_name, span] : current_result->imports) {
    const auto import_source = resolve_import_source(source_file, module_name);
    if (import_source.empty()) {
      continue;
    }

    auto imported_result = collect_program(import_source, cache, in_progress);
    if (!imported_result) {
      in_progress.erase(key);
      return tl::unexpected(imported_result.error());
    }

    imported_exprs.insert(imported_exprs.end(), imported_result->expressions.begin(),
                          imported_result->expressions.end());

    for (const auto& imported_let : imported_result->lets) {
      if (const std::string sym = let_key(imported_let.qualifier, imported_let.name); seen_symbols.insert(sym).second) {
        imported_lets.push_back(imported_let);
      }
    }
  }

  merged.lets.insert(merged.lets.begin(), imported_lets.begin(), imported_lets.end());
  merged.expressions.insert(merged.expressions.begin(), imported_exprs.begin(), imported_exprs.end());

  cache[key] = merged;
  in_progress.erase(key);
  return merged;
}

struct EvalState {
  IRProgram program;
  std::unordered_map<std::string, RuntimeCallable> functions_qualified;
  std::unordered_map<std::string, RuntimeCallable> functions_unqualified;

  Value eval_expr(const IRExprPtr& expr, const std::unordered_map<std::string, Value>& locals) const {
    if (!expr) {
      return fleaux::runtime::make_null();
    }

    if (std::holds_alternative<IRFlowExpr>(expr->node)) {
      const auto& flow = std::get<IRFlowExpr>(expr->node);
      const Value lhs = eval_expr(flow.lhs, locals);
      const auto target = resolve_call_target(flow.rhs, locals);
      return target(lhs);
    }

    if (std::holds_alternative<IRTupleExpr>(expr->node)) {
      const auto&[items, span] = std::get<IRTupleExpr>(expr->node);
      fleaux::runtime::Array arr;
      arr.Reserve(items.size());
      for (const auto& item : items) {
        if (item && std::holds_alternative<IRNameRef>(item->node)) {
          if (const auto& name_ref = std::get<IRNameRef>(item->node); name_ref.qualifier.has_value() || !locals.contains(name_ref.name)) {
            arr.PushBack(fleaux::runtime::make_callable_ref(resolve_name_callable(name_ref, locals)));
            continue;
          }
        }
        arr.PushBack(eval_expr(item, locals));
      }
      return Value{std::move(arr)};
    }

    if (std::holds_alternative<frontend::ir::IRConstant>(expr->node)) {
      const auto&[val, span] = std::get<frontend::ir::IRConstant>(expr->node);
      if (std::holds_alternative<std::monostate>(val)) {
        return fleaux::runtime::make_null();
      }
      if (std::holds_alternative<bool>(val)) {
        return fleaux::runtime::make_bool(std::get<bool>(val));
      }
      if (std::holds_alternative<std::int64_t>(val)) {
        return fleaux::runtime::make_int(std::get<std::int64_t>(val));
      }
      if (std::holds_alternative<double>(val)) {
        return fleaux::runtime::make_float(std::get<double>(val));
      }
      return fleaux::runtime::make_string(std::get<std::string>(val));
    }

    const auto&[qualifier, name, span] = std::get<IRNameRef>(expr->node);
    if (!qualifier.has_value()) {
      if (const auto local_it = locals.find(name); local_it != locals.end()) {
        return local_it->second;
      }

      if (const auto fn_it = functions_unqualified.find(name); fn_it != functions_unqualified.end()) {
        return fleaux::runtime::make_callable_ref(fn_it->second);
      }

      throw std::runtime_error("Unresolved name in VM interpreter: '" + name + "'.");
    }

    const auto full_name = *qualifier + "." + name;
    if (qualifier.value() == "Std" || qualifier->rfind("Std.", 0) == 0) {
      return resolve_std_callable_or_throw(full_name, span)(fleaux::runtime::make_tuple());
    }

    if (const auto fn_it = functions_qualified.find(full_name); fn_it != functions_qualified.end()) {
      return fleaux::runtime::make_callable_ref(fn_it->second);
    }

    throw std::runtime_error("Unresolved qualified name in VM interpreter: '" + full_name + "'.");
  }

  RuntimeCallable resolve_name_callable(const IRNameRef& name,
                                        const std::unordered_map<std::string, Value>& locals) const {
    if (name.qualifier.has_value()) {
      const std::string full_name = *name.qualifier + "." + name.name;
      if (*name.qualifier == "Std" || name.qualifier->rfind("Std.", 0) == 0) {
        return resolve_std_callable_or_throw(full_name, name.span);
      }

      if (const auto fn_it = functions_qualified.find(full_name); fn_it != functions_qualified.end()) {
        return fn_it->second;
      }

      throw std::runtime_error("Unknown callable target in VM interpreter: '" + full_name + "'.");
    }

    if (const auto local_it = locals.find(name.name); local_it != locals.end()) {
      const Value callable_ref = local_it->second;
      return [callable_ref](Value arg) -> Value {
        return fleaux::runtime::invoke_callable_ref(callable_ref, std::move(arg));
      };
    }

    if (const auto fn_it = functions_unqualified.find(name.name); fn_it != functions_unqualified.end()) {
      return fn_it->second;
    }

    throw std::runtime_error("Unknown unqualified callable in VM interpreter: '" + name.name + "'.");
  }

  RuntimeCallable resolve_call_target(const IRCallTarget& target,
                                      const std::unordered_map<std::string, Value>& locals) const {
    if (std::holds_alternative<frontend::ir::IROperatorRef>(target)) {
      const auto& op = std::get<frontend::ir::IROperatorRef>(target).op;
      if (op == "+") {
        return resolve_std_callable_or_throw("Std.Add", std::nullopt);
      }
      if (op == "-") {
        return resolve_std_callable_or_throw("Std.Subtract", std::nullopt);
      }
      if (op == "*") {
        return resolve_std_callable_or_throw("Std.Multiply", std::nullopt);
      }
      if (op == "/") {
        return resolve_std_callable_or_throw("Std.Divide", std::nullopt);
      }
      if (op == "%") {
        return resolve_std_callable_or_throw("Std.Mod", std::nullopt);
      }
      if (op == "^") {
        return resolve_std_callable_or_throw("Std.Pow", std::nullopt);
      }
      if (op == "==") {
        return resolve_std_callable_or_throw("Std.Equal", std::nullopt);
      }
      if (op == "!=") {
        return resolve_std_callable_or_throw("Std.NotEqual", std::nullopt);
      }
      if (op == "<") {
        return resolve_std_callable_or_throw("Std.LessThan", std::nullopt);
      }
      if (op == ">") {
        return resolve_std_callable_or_throw("Std.GreaterThan", std::nullopt);
      }
      if (op == ">=") {
        return resolve_std_callable_or_throw("Std.GreaterOrEqual", std::nullopt);
      }
      if (op == "<=") {
        return resolve_std_callable_or_throw("Std.LessOrEqual", std::nullopt);
      }
      if (op == "!") {
        return resolve_std_callable_or_throw("Std.Not", std::nullopt);
      }
      if (op == "&&") {
        return resolve_std_callable_or_throw("Std.And", std::nullopt);
      }
      if (op == "||") {
        return resolve_std_callable_or_throw("Std.Or", std::nullopt);
      }
      throw std::runtime_error("Unsupported operator in VM interpreter: '" + op + "'.");
    }

    return resolve_name_callable(std::get<IRNameRef>(target), locals);
  }

  Value invoke_let(const frontend::ir::IRLet& let, Value arg) const {
    if (let.is_builtin) {
      return resolve_std_callable_or_throw(let_key(let.qualifier, let.name), let.span)(std::move(arg));
    }

    std::unordered_map<std::string, Value> locals;
    for (const auto&[fst, snd] : functions_unqualified) {
      locals.emplace(fst, fleaux::runtime::make_callable_ref(snd));
    }

    if (let.params.size() == 1U) {
      locals[let.params[0].name] = fleaux::runtime::unwrap_singleton_arg(std::move(arg));
    } else if (let.params.size() > 1U) {
      for (std::size_t idx = 0; idx < let.params.size(); ++idx) {
        locals[let.params[idx].name] = fleaux::runtime::array_at(arg, idx);
      }
    }

    return eval_expr(let.body, locals);
  }
};

}  // namespace

InterpretResult Interpreter::run_file(const std::filesystem::path& source_file,
                                      const std::vector<std::string>& process_args) const {
  if (!std::filesystem::exists(source_file)) {
    return tl::unexpected(make_error("Input file does not exist.", std::nullopt));
  }

  std::unordered_map<std::string, IRProgram> cache;
  std::unordered_set<std::string> in_progress;
  auto merged_result = collect_program(source_file, cache, in_progress);
  if (!merged_result) {
    return tl::unexpected(merged_result.error());
  }

  std::vector<std::string> args_storage;
  args_storage.reserve(process_args.size() + 1U);
  args_storage.push_back(source_file.string());
  args_storage.insert(args_storage.end(), process_args.begin(), process_args.end());
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args_storage.size());
  for (auto& arg : args_storage) {
    argv_ptrs.push_back(arg.data());
  }
  fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());

  auto state = std::make_shared<EvalState>();
  state->program = std::move(merged_result.value());

  for (const auto& let : state->program.lets) {
    const std::string full_name = let_key(let.qualifier, let.name);
    const auto let_ptr = &let;
    const RuntimeCallable callable = [state, let_ptr](Value arg) -> Value {
      return state->invoke_let(*let_ptr, std::move(arg));
    };
    state->functions_qualified[full_name] = callable;
    state->functions_unqualified[let.name] = callable;
  }

  try {
    for (const auto&[expr, span] : state->program.expressions) {
      (void)state->eval_expr(expr, {});
    }
  } catch (const std::exception& ex) {
    return tl::unexpected(make_error(ex.what(), "Unsupported VM coverage in current interpreter slice."));
  }

  return {};
}

}  // namespace fleaux::vm

