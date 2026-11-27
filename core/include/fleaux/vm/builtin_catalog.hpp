#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "fleaux/common/fnv1a.hpp"

namespace fleaux::vm {

enum class BuiltinId : std::uint16_t {
  UnaryPlus,
  UnaryMinus,
  Add,
  Subtract,
  Multiply,
  Divide,
  Mod,
  Pow,
  BitAnd,
  BitOr,
  BitXor,
  BitNot,
  BitShiftLeft,
  BitShiftRight,
  Equal,
  NotEqual,
  LessThan,
  GreaterThan,
  GreaterOrEqual,
  LessOrEqual,
  Not,
  And,
  Or,
  Select,
  Match,
  Apply,
  Branch,
  Loop,
  LoopN,
  Printf,
  Println,
  GetArgs,
  Type,
  InputVoid,
  InputString,
  Help,
  ExitVoid,
  ExitInt64,
  Cwd,
  OSEnv,
  OSHasEnv,
  OSSetEnv,
  OSUnsetEnv,
  OSIsWindows,
  OSIsLinux,
  OSIsMacOS,
  OSHome,
  OSTempDir,
  OSExec,
  OSMakeTempFile,
  OSMakeTempDir,
  PathJoin,
  PathNormalize,
  PathBasename,
  PathDirname,
  PathExists,
  PathIsFile,
  PathIsDir,
  PathAbsolute,
  PathExtension,
  PathStem,
  PathWithExtension,
  PathWithBasename,
  FileReadText,
  FileWriteText,
  FileAppendText,
  FileReadLines,
  FileDelete,
  FileSize,
  FileOpen,
  FileReadLine,
  FileReadChunk,
  FileWriteChunk,
  FileFlush,
  FileClose,
  FileWithOpen,
  DirCreate,
  DirDelete,
  DirList,
  DirListFull,
  TupleAppend,
  TuplePrepend,
  TupleReverse,
  TupleContains,
  TupleZip,
  TupleMap,
  TupleFilter,
  TupleSort,
  TupleUnique,
  TupleMin,
  TupleMax,
  TupleReduce,
  TupleFindIndex,
  TupleAny,
  TupleAll,
  TupleRangeInt64,
  TupleRangeInt64Int64,
  TupleRangeInt64Int64Int64,
  TupleSetAt,
  TupleInsertAt,
  TupleRemoveAt,
  TupleConcat,
  ArraySetAt2D,
  ArrayFill,
  ArrayTranspose2D,
  ArraySlice2D,
  ArrayReshape,
  ArrayRank,
  ArrayShape,
  ArrayFlatten,
  ArrayGetAtND,
  ArraySetAtND,
  ArrayReshapeND,
  DictCreateVoid,
  DictCreateDict,
  DictSet,
  DictGet,
  DictGetDefault,
  DictContains,
  DictDelete,
  DictMerge,
  DictKeys,
  DictValues,
  DictEntries,
  DictClear,
  DictLength,
  Cast,
  TryCast,
  ToInt64,
  ToUInt64,
  ToFloat64,
  RandomCreate,
  RandomNextUInt64,
  RandomNextInt64,
  RandomNextFloat64,
  RandomNextBool,
  RandomSplit,
  MathFloor,
  MathCeil,
  MathAbs,
  MathLog,
  MathClamp,
  Sqrt,
  Sin,
  Cos,
  Tan,
  ResultOk,
  ResultErr,
  ResultPayload,
  ResultIsOk,
  ResultIsErr,
  ResultUnwrap,
  ResultUnwrapErr,
  ResultMap,
  ResultMapErr,
  ResultAndThen,
  ResultOrElse,
  ResultUnwrapOr,
  Panic,
  Assert,
  AssertEqual,
  MathExp,
  MathLog2,
  MathLog10,
  MathRound,
  MathTrunc,
  MathAsin,
  MathAcos,
  MathAtan,
  MathAtan2,
  Min,
  Max,
  TupleEnumerate,
  TupleSortBy,
  TupleGroupBy,
  TuplePartition,
  TupleFlatMap,
  TupleScan,
  TupleTakeWhile,
  TupleDropWhile,
  DictMap,
  DictFilter,
  DictUpdate,
  StringRepeat,
  StringReverse,
  StringChars,
  TimeNow,
  TimeMonotonic,
  TimeSleep,
  Try,
  ParallelMap,
  ParallelWithOptions,
  ParallelForEach,
  ParallelReduce,
  TaskSpawn,
  TaskAwait,
  TaskAwaitAll,
  TaskCancel,
  TaskWithTimeout,
  Wrap,
  Unwrap,
  First,
  Second,
  ElementAt,
  Length,
  Take,
  Drop,
  SliceTupleInt64,
  SliceTupleInt64Int64,
  SliceTupleInt64Int64Int64,
  ToString,
  StringParseInt64,
  StringParseUInt64,
  StringParseFloat64,
  StringUpper,
  StringLower,
  StringTrim,
  StringTrimStart,
  StringTrimEnd,
  StringSplit,
  StringJoin,
  StringReplace,
  StringContains,
  StringStartsWith,
  StringEndsWith,
  StringLength,
  StringCharAt,
  StringSliceStringInt64,
  StringSliceStringInt64Int64,
  StringFindStringString,
  StringFindStringStringInt64,
  StringFormat,
  StringRegexIsMatch,
  StringRegexFind,
  StringRegexReplace,
  StringRegexSplit,
  WrappingAdd,
  WrappingSubtract,
  WrappingMultiply,
  SaturatingAdd,
  SaturatingSubtract,
  SaturatingMultiply,
  kCount,
};

enum class BuiltinArityKind : std::uint8_t {
  kExact,
  kVariadicMinimum,
};

enum class BuiltinCallShape : std::uint8_t {
  kTupleExpanded = 0,
  kDirectValue = 1,
};

struct BuiltinArityContract {
  BuiltinArityKind kind{BuiltinArityKind::kExact};
  std::uint8_t primary{0U};
};

[[nodiscard]] constexpr auto builtin_arity_exact(const std::uint8_t arity) -> BuiltinArityContract {
  return BuiltinArityContract{.kind = BuiltinArityKind::kExact, .primary = arity};
}

[[nodiscard]] constexpr auto builtin_arity_variadic_minimum(const std::uint8_t minimum_arity) -> BuiltinArityContract {
  return BuiltinArityContract{.kind = BuiltinArityKind::kVariadicMinimum, .primary = minimum_arity};
}

// The kind of value a builtin's value slot takes (BuiltinSlot). The numbers are written in bytecode.
enum class BuiltinValueSlotKind : std::uint8_t {
  kDictStringAny = 0,
  kExactUInt64 = 1,
  kExactInt64 = 2,
  kTaskHandle = 3,
  kRandomGenerator = 4,
  kFileHandle = 5,
};

[[nodiscard]] constexpr auto builtin_value_slot_kind_name(const BuiltinValueSlotKind kind) -> std::string_view {
  switch (kind) {
    case BuiltinValueSlotKind::kDictStringAny:
      return "Dict<String, Any>";
    case BuiltinValueSlotKind::kExactUInt64:
      return "exact UInt64";
    case BuiltinValueSlotKind::kExactInt64:
      return "exact Int64";
    case BuiltinValueSlotKind::kTaskHandle:
      return "TaskHandle";
    case BuiltinValueSlotKind::kRandomGenerator:
      return "RandomGenerator";
    case BuiltinValueSlotKind::kFileHandle:
      return "FileHandle";
  }
  return "unknown";
}

// An argument of a builtin that the compiler records at each call and the verifier checks when it loads bytecode:
// either a function of `callable_arity` parameters, or a value of `value_kind`. A builtin's slots follow its
// parameter types in stdlib/Std.fleaux, which a test checks.
struct BuiltinSlot {
  std::uint8_t index{0};
  std::optional<BuiltinValueSlotKind> value_kind{std::nullopt};
  std::uint8_t callable_arity{0};
};

[[nodiscard]] constexpr auto callable_slot(const std::uint8_t index, const std::uint8_t arity) -> BuiltinSlot {
  return BuiltinSlot{.index = index, .value_kind = std::nullopt, .callable_arity = arity};
}

[[nodiscard]] constexpr auto value_slot(const std::uint8_t index, const BuiltinValueSlotKind kind) -> BuiltinSlot {
  return BuiltinSlot{.index = index, .value_kind = kind, .callable_arity = 0};
}

// A builtin's slots, in order of argument position.
class BuiltinSlots {
public:
  static constexpr std::size_t kCapacity = 4;

  constexpr BuiltinSlots() = default;

  // More slots than kCapacity do not compile: the throw is not a constant expression.
  constexpr BuiltinSlots(const std::initializer_list<BuiltinSlot> slots) {
    for (const BuiltinSlot& slot : slots) {
      if (count_ == kCapacity) {
        throw std::length_error("a builtin has more slots than BuiltinSlots::kCapacity");
      }
      items_[count_++] = slot;
    }
  }

  [[nodiscard]] constexpr auto view() const -> std::span<const BuiltinSlot> { return {items_.data(), count_}; }

private:
  std::array<BuiltinSlot, kCapacity> items_{};
  std::size_t count_{0};
};

struct BuiltinSpec {
  BuiltinId id;
  std::string_view name;
  std::string_view symbol_key{};

  BuiltinArityContract arity{};
  BuiltinSlots slots{};
};

inline constexpr auto kBuiltinSpecs = std::to_array<BuiltinSpec>({
    BuiltinSpec{.id = BuiltinId::UnaryPlus, .name = "Std.UnaryPlus", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::UnaryMinus, .name = "Std.UnaryMinus", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Add, .name = "Std.Add", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Subtract, .name = "Std.Subtract", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Multiply, .name = "Std.Multiply", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Divide, .name = "Std.Divide", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Mod, .name = "Std.Mod", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Pow, .name = "Std.Pow", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::BitAnd,
                .name = "Std.Bit.And",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64), value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::BitOr,
                .name = "Std.Bit.Or",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64), value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::BitXor,
                .name = "Std.Bit.Xor",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64), value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::BitNot,
                .name = "Std.Bit.Not",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::BitShiftLeft,
                .name = "Std.Bit.ShiftLeft",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64), value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::BitShiftRight,
                .name = "Std.Bit.ShiftRight",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64), value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::Equal, .name = "Std.Equal", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::NotEqual, .name = "Std.NotEqual", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::LessThan, .name = "Std.LessThan", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::GreaterThan, .name = "Std.GreaterThan", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::GreaterOrEqual, .name = "Std.GreaterOrEqual", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::LessOrEqual, .name = "Std.LessOrEqual", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Not, .name = "Std.Not", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::And, .name = "Std.And", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Or, .name = "Std.Or", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Select, .name = "Std.Select", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::Match, .name = "Std.Match", .arity = builtin_arity_variadic_minimum(2U)},
    BuiltinSpec{.id = BuiltinId::Apply, .name = "Std.Apply", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::Branch,
                .name = "Std.Branch",
                .arity = builtin_arity_exact(4U),
                .slots = {callable_slot(2U, 1U), callable_slot(3U, 1U)}},
    BuiltinSpec{.id = BuiltinId::Loop,
                .name = "Std.Loop",
                .arity = builtin_arity_exact(3U),
                .slots = {callable_slot(1U, 1U), callable_slot(2U, 1U)}},
    BuiltinSpec{.id = BuiltinId::LoopN,
                .name = "Std.LoopN",
                .arity = builtin_arity_exact(4U),
                .slots = {callable_slot(1U, 1U), callable_slot(2U, 1U), value_slot(3U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::Printf, .name = "Std.Printf", .arity = builtin_arity_variadic_minimum(1U)},
    BuiltinSpec{.id = BuiltinId::Println, .name = "Std.Println", .arity = builtin_arity_variadic_minimum(0U)},
    BuiltinSpec{.id = BuiltinId::GetArgs, .name = "Std.GetArgs", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::Type, .name = "Std.Type", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::InputVoid, .name = "Std.Input", .symbol_key = "Std.Input#0", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::InputString, .name = "Std.Input", .symbol_key = "Std.Input#1", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Help, .name = "Std.Help", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ExitVoid, .name = "Std.Exit", .symbol_key = "Std.Exit#0", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::ExitInt64,
                .name = "Std.Exit",
                .symbol_key = "Std.Exit#1",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::Cwd, .name = "Std.OS.Cwd", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSEnv, .name = "Std.OS.Env", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::OSHasEnv, .name = "Std.OS.HasEnv", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::OSSetEnv, .name = "Std.OS.SetEnv", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::OSUnsetEnv, .name = "Std.OS.UnsetEnv", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::OSIsWindows, .name = "Std.OS.IsWindows", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSIsLinux, .name = "Std.OS.IsLinux", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSIsMacOS, .name = "Std.OS.IsMacOS", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSHome, .name = "Std.OS.Home", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSTempDir, .name = "Std.OS.TempDir", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSExec, .name = "Std.OS.Exec", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::OSMakeTempFile, .name = "Std.OS.MakeTempFile", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSMakeTempDir, .name = "Std.OS.MakeTempDir", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::PathJoin, .name = "Std.Path.Join", .arity = builtin_arity_variadic_minimum(2U)},
    BuiltinSpec{.id = BuiltinId::PathNormalize, .name = "Std.Path.Normalize", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::PathBasename, .name = "Std.Path.Basename", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::PathDirname, .name = "Std.Path.Dirname", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::PathExists, .name = "Std.Path.Exists", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::PathIsFile, .name = "Std.Path.IsFile", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::PathIsDir, .name = "Std.Path.IsDir", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::PathAbsolute, .name = "Std.Path.Absolute", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::PathExtension, .name = "Std.Path.Extension", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::PathStem, .name = "Std.Path.Stem", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::PathWithExtension, .name = "Std.Path.WithExtension", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::PathWithBasename, .name = "Std.Path.WithBasename", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::FileReadText, .name = "Std.File.ReadText", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::FileWriteText, .name = "Std.File.WriteText", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::FileAppendText, .name = "Std.File.AppendText", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::FileReadLines, .name = "Std.File.ReadLines", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::FileDelete, .name = "Std.File.Delete", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::FileSize, .name = "Std.File.Size", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::FileOpen, .name = "Std.File.Open", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::FileReadLine,
                .name = "Std.File.ReadLine",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kFileHandle)}},
    BuiltinSpec{.id = BuiltinId::FileReadChunk,
                .name = "Std.File.ReadChunk",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kFileHandle), value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::FileWriteChunk,
                .name = "Std.File.WriteChunk",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kFileHandle)}},
    BuiltinSpec{.id = BuiltinId::FileFlush,
                .name = "Std.File.Flush",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kFileHandle)}},
    BuiltinSpec{.id = BuiltinId::FileClose,
                .name = "Std.File.Close",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kFileHandle)}},
    BuiltinSpec{
        .id = BuiltinId::FileWithOpen, .name = "Std.File.WithOpen", .arity = builtin_arity_exact(3U), .slots = {callable_slot(2U, 1U)}},
    BuiltinSpec{.id = BuiltinId::DirCreate, .name = "Std.Dir.Create", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::DirDelete, .name = "Std.Dir.Delete", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::DirList, .name = "Std.Dir.List", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::DirListFull, .name = "Std.Dir.ListFull", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::TupleAppend, .name = "Std.Tuple.Append", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TuplePrepend, .name = "Std.Tuple.Prepend", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleReverse, .name = "Std.Tuple.Reverse", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::TupleContains, .name = "Std.Tuple.Contains", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleZip, .name = "Std.Tuple.Zip", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleMap, .name = "Std.Tuple.Map", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::TupleFilter, .name = "Std.Tuple.Filter", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::TupleSort, .name = "Std.Tuple.Sort", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::TupleUnique, .name = "Std.Tuple.Unique", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::TupleMin, .name = "Std.Tuple.Min", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::TupleMax, .name = "Std.Tuple.Max", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{
        .id = BuiltinId::TupleReduce, .name = "Std.Tuple.Reduce", .arity = builtin_arity_exact(3U), .slots = {callable_slot(2U, 2U)}},
    BuiltinSpec{
        .id = BuiltinId::TupleFindIndex, .name = "Std.Tuple.FindIndex", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::TupleAny, .name = "Std.Tuple.Any", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::TupleAll, .name = "Std.Tuple.All", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::TupleRangeInt64,
                .name = "Std.Tuple.Range",
                .symbol_key = "Std.Tuple.Range#0",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::TupleRangeInt64Int64,
                .name = "Std.Tuple.Range",
                .symbol_key = "Std.Tuple.Range#1",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64), value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::TupleRangeInt64Int64Int64,
                .name = "Std.Tuple.Range",
                .symbol_key = "Std.Tuple.Range#2",
                .arity = builtin_arity_exact(3U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64), value_slot(1U, BuiltinValueSlotKind::kExactInt64),
                          value_slot(2U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::TupleSetAt,
                .name = "Std.Tuple.SetAt",
                .arity = builtin_arity_exact(3U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::TupleInsertAt,
                .name = "Std.Tuple.InsertAt",
                .arity = builtin_arity_exact(3U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::TupleRemoveAt,
                .name = "Std.Tuple.RemoveAt",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::TupleConcat, .name = "Std.Tuple.Concat", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::ArraySetAt2D,
                .name = "Std.Array.SetAt2D",
                .arity = builtin_arity_exact(4U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64), value_slot(2U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::ArrayFill,
                .name = "Std.Array.Fill",
                .arity = builtin_arity_exact(4U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64), value_slot(2U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::ArrayTranspose2D, .name = "Std.Array.Transpose2D", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ArraySlice2D,
                .name = "Std.Array.Slice2D",
                .arity = builtin_arity_exact(5U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64), value_slot(2U, BuiltinValueSlotKind::kExactInt64),
                          value_slot(3U, BuiltinValueSlotKind::kExactInt64), value_slot(4U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::ArrayReshape,
                .name = "Std.Array.Reshape",
                .arity = builtin_arity_exact(3U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64), value_slot(2U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::ArrayRank, .name = "Std.Array.Rank", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ArrayShape, .name = "Std.Array.Shape", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ArrayFlatten, .name = "Std.Array.Flatten", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ArrayGetAtND, .name = "Std.Array.GetAtND", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::ArraySetAtND, .name = "Std.Array.SetAtND", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::ArrayReshapeND, .name = "Std.Array.ReshapeND", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{
        .id = BuiltinId::DictCreateVoid, .name = "Std.Dict.Create", .symbol_key = "Std.Dict.Create#0", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{
        .id = BuiltinId::DictCreateDict, .name = "Std.Dict.Create", .symbol_key = "Std.Dict.Create#1", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::DictSet, .name = "Std.Dict.Set", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::DictGet, .name = "Std.Dict.Get", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::DictGetDefault, .name = "Std.Dict.GetDefault", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::DictContains, .name = "Std.Dict.Contains", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::DictDelete, .name = "Std.Dict.Delete", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::DictMerge, .name = "Std.Dict.Merge", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::DictKeys, .name = "Std.Dict.Keys", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::DictValues, .name = "Std.Dict.Values", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::DictEntries, .name = "Std.Dict.Entries", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::DictClear, .name = "Std.Dict.Clear", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::DictLength, .name = "Std.Dict.Length", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Cast, .name = "Std.Cast", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::TryCast, .name = "Std.TryCast", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ToInt64, .name = "Std.ToInt64", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ToUInt64, .name = "Std.ToUInt64", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ToFloat64, .name = "Std.ToFloat64", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::RandomCreate,
                .name = "Std.Random.Create",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactUInt64)}},
    BuiltinSpec{.id = BuiltinId::RandomNextUInt64,
                .name = "Std.Random.NextUInt64",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kRandomGenerator)}},
    BuiltinSpec{.id = BuiltinId::RandomNextInt64,
                .name = "Std.Random.NextInt64",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kRandomGenerator)}},
    BuiltinSpec{.id = BuiltinId::RandomNextFloat64,
                .name = "Std.Random.NextFloat64",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kRandomGenerator)}},
    BuiltinSpec{.id = BuiltinId::RandomNextBool,
                .name = "Std.Random.NextBool",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kRandomGenerator)}},
    BuiltinSpec{.id = BuiltinId::RandomSplit,
                .name = "Std.Random.Split",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kRandomGenerator)}},
    BuiltinSpec{.id = BuiltinId::MathFloor, .name = "Std.Math.Floor", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathCeil, .name = "Std.Math.Ceil", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathAbs, .name = "Std.Math.Abs", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathLog, .name = "Std.Math.Log", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathClamp, .name = "Std.Math.Clamp", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::Sqrt, .name = "Std.Math.Sqrt", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Sin, .name = "Std.Math.Sin", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Cos, .name = "Std.Math.Cos", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Tan, .name = "Std.Math.Tan", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ResultOk, .name = "Std.Result.Ok", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ResultErr, .name = "Std.Result.Err", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ResultPayload, .name = "Std.Result.Payload", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ResultIsOk, .name = "Std.Result.IsOk", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ResultIsErr, .name = "Std.Result.IsErr", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ResultUnwrap, .name = "Std.Result.Unwrap", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ResultUnwrapErr, .name = "Std.Result.UnwrapErr", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ResultMap, .name = "Std.Result.Map", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::ResultMapErr, .name = "Std.Result.MapErr", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::ResultAndThen, .name = "Std.Result.AndThen", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::ResultOrElse, .name = "Std.Result.OrElse", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::ResultUnwrapOr, .name = "Std.Result.UnwrapOr", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Panic, .name = "Std.Panic", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Assert, .name = "Std.Assert", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::AssertEqual, .name = "Std.AssertEqual", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::MathExp, .name = "Std.Math.Exp", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathLog2, .name = "Std.Math.Log2", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathLog10, .name = "Std.Math.Log10", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathRound, .name = "Std.Math.Round", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathTrunc, .name = "Std.Math.Trunc", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathAsin, .name = "Std.Math.Asin", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathAcos, .name = "Std.Math.Acos", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathAtan, .name = "Std.Math.Atan", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::MathAtan2, .name = "Std.Math.Atan2", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Min, .name = "Std.Min", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Max, .name = "Std.Max", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleEnumerate, .name = "Std.Tuple.Enumerate", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{
        .id = BuiltinId::TupleSortBy, .name = "Std.Tuple.SortBy", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::TupleGroupBy, .name = "Std.Tuple.GroupBy", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::TuplePartition, .name = "Std.Tuple.Partition", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::TupleFlatMap, .name = "Std.Tuple.FlatMap", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::TupleScan, .name = "Std.Tuple.Scan", .arity = builtin_arity_exact(3U), .slots = {callable_slot(2U, 2U)}},
    BuiltinSpec{
        .id = BuiltinId::TupleTakeWhile, .name = "Std.Tuple.TakeWhile", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::TupleDropWhile, .name = "Std.Tuple.DropWhile", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::DictMap, .name = "Std.Dict.Map", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::DictFilter, .name = "Std.Dict.Filter", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 2U)}},
    BuiltinSpec{.id = BuiltinId::DictUpdate, .name = "Std.Dict.Update", .arity = builtin_arity_exact(4U), .slots = {callable_slot(3U, 1U)}},
    BuiltinSpec{.id = BuiltinId::StringRepeat,
                .name = "Std.String.Repeat",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::StringReverse, .name = "Std.String.Reverse", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringChars, .name = "Std.String.Chars", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::TimeNow, .name = "Std.Time.Now", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::TimeMonotonic, .name = "Std.Time.Monotonic", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::TimeSleep,
                .name = "Std.Time.Sleep",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::Try, .name = "Std.Try", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::ParallelMap, .name = "Std.Parallel.Map", .arity = builtin_arity_exact(2U), .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{.id = BuiltinId::ParallelWithOptions,
                .name = "Std.Parallel.WithOptions",
                .arity = builtin_arity_exact(3U),
                .slots = {callable_slot(1U, 1U), value_slot(2U, BuiltinValueSlotKind::kDictStringAny)}},
    BuiltinSpec{.id = BuiltinId::ParallelForEach,
                .name = "Std.Parallel.ForEach",
                .arity = builtin_arity_exact(2U),
                .slots = {callable_slot(1U, 1U)}},
    BuiltinSpec{
        .id = BuiltinId::ParallelReduce, .name = "Std.Parallel.Reduce", .arity = builtin_arity_exact(3U), .slots = {callable_slot(2U, 2U)}},
    BuiltinSpec{.id = BuiltinId::TaskSpawn, .name = "Std.Task.Spawn", .arity = builtin_arity_exact(2U), .slots = {callable_slot(0U, 1U)}},
    BuiltinSpec{.id = BuiltinId::TaskAwait,
                .name = "Std.Task.Await",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kTaskHandle)}},
    BuiltinSpec{.id = BuiltinId::TaskAwaitAll, .name = "Std.Task.AwaitAll", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::TaskCancel,
                .name = "Std.Task.Cancel",
                .arity = builtin_arity_exact(1U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kTaskHandle)}},
    BuiltinSpec{.id = BuiltinId::TaskWithTimeout,
                .name = "Std.Task.WithTimeout",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(0U, BuiltinValueSlotKind::kTaskHandle), value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::Wrap, .name = "Std.Wrap", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Unwrap, .name = "Std.Unwrap", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::First, .name = "Std.First", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Second, .name = "Std.Second", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ElementAt,
                .name = "Std.ElementAt",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::Length, .name = "Std.Length", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Take,
                .name = "Std.Take",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::Drop,
                .name = "Std.Drop",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::SliceTupleInt64,
                .name = "Std.Slice",
                .symbol_key = "Std.Slice#0",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::SliceTupleInt64Int64,
                .name = "Std.Slice",
                .symbol_key = "Std.Slice#1",
                .arity = builtin_arity_exact(3U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64), value_slot(2U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::SliceTupleInt64Int64Int64,
                .name = "Std.Slice",
                .symbol_key = "Std.Slice#2",
                .arity = builtin_arity_exact(4U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64), value_slot(2U, BuiltinValueSlotKind::kExactInt64),
                          value_slot(3U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::ToString, .name = "Std.ToString", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringParseInt64, .name = "Std.String.ParseInt64", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringParseUInt64, .name = "Std.String.ParseUInt64", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringParseFloat64, .name = "Std.String.ParseFloat64", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringUpper, .name = "Std.String.Upper", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringLower, .name = "Std.String.Lower", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringTrim, .name = "Std.String.Trim", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringTrimStart, .name = "Std.String.TrimStart", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringTrimEnd, .name = "Std.String.TrimEnd", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringSplit, .name = "Std.String.Split", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringJoin, .name = "Std.String.Join", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringReplace, .name = "Std.String.Replace", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::StringContains, .name = "Std.String.Contains", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringStartsWith, .name = "Std.String.StartsWith", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringEndsWith, .name = "Std.String.EndsWith", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringLength, .name = "Std.String.Length", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringCharAt,
                .name = "Std.String.CharAt",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::StringSliceStringInt64,
                .name = "Std.String.Slice",
                .symbol_key = "Std.String.Slice#0",
                .arity = builtin_arity_exact(2U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::StringSliceStringInt64Int64,
                .name = "Std.String.Slice",
                .symbol_key = "Std.String.Slice#1",
                .arity = builtin_arity_exact(3U),
                .slots = {value_slot(1U, BuiltinValueSlotKind::kExactInt64), value_slot(2U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::StringFindStringString,
                .name = "Std.String.Find",
                .symbol_key = "Std.String.Find#0",
                .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringFindStringStringInt64,
                .name = "Std.String.Find",
                .symbol_key = "Std.String.Find#1",
                .arity = builtin_arity_exact(3U),
                .slots = {value_slot(2U, BuiltinValueSlotKind::kExactInt64)}},
    BuiltinSpec{.id = BuiltinId::StringFormat, .name = "Std.String.Format", .arity = builtin_arity_variadic_minimum(1U)},
    BuiltinSpec{.id = BuiltinId::StringRegexIsMatch, .name = "Std.String.Regex.IsMatch", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringRegexFind, .name = "Std.String.Regex.Find", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringRegexReplace, .name = "Std.String.Regex.Replace", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::StringRegexSplit, .name = "Std.String.Regex.Split", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::WrappingAdd, .name = "Std.WrappingAdd", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::WrappingSubtract, .name = "Std.WrappingSubtract", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::WrappingMultiply, .name = "Std.WrappingMultiply", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::SaturatingAdd, .name = "Std.SaturatingAdd", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::SaturatingSubtract, .name = "Std.SaturatingSubtract", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::SaturatingMultiply, .name = "Std.SaturatingMultiply", .arity = builtin_arity_exact(2U)},
});

[[nodiscard]] inline auto builtin_name_lookup_map() -> const std::unordered_map<std::string_view, BuiltinId>& {
  static const auto map = []() -> std::unordered_map<std::string_view, BuiltinId> {
    std::unordered_map<std::string_view, BuiltinId> out;
    out.reserve(kBuiltinSpecs.size());
    for (const auto& spec : kBuiltinSpecs) {
      out.emplace(spec.name, spec.id);
    }
    return out;
  }();
  return map;
}

[[nodiscard]] inline auto builtin_symbol_key_lookup_map() -> const std::unordered_map<std::string_view, BuiltinId>& {
  static const auto map = []() -> std::unordered_map<std::string_view, BuiltinId> {
    std::unordered_map<std::string_view, BuiltinId> out;
    out.reserve(kBuiltinSpecs.size());
    for (const auto& spec : kBuiltinSpecs) {
      const auto effective_symbol_key = spec.symbol_key.empty() ? spec.name : spec.symbol_key;
      out.emplace(effective_symbol_key, spec.id);
    }
    return out;
  }();
  return map;
}

[[nodiscard]] constexpr auto builtin_count() -> std::size_t { return static_cast<std::size_t>(BuiltinId::kCount); }

// The spec tables are indexed by BuiltinId, so their order must match the enum exactly.
[[nodiscard]] consteval auto builtin_spec_tables_match_enum_order() -> bool {
  if (kBuiltinSpecs.size() != builtin_count()) {
    return false;
  }
  for (std::size_t index = 0; index < kBuiltinSpecs.size(); ++index) {
    if (static_cast<std::size_t>(kBuiltinSpecs[index].id) != index) {
      return false;
    }
  }
  return true;
}

static_assert(builtin_spec_tables_match_enum_order(), "kBuiltinSpecs must list every BuiltinId in enum order.");

inline constexpr std::uint64_t kBuiltinOperandIdMask = 0xFFFF'FFFFULL;
inline constexpr std::uint64_t kBuiltinOperandDirectValueBit = 1ULL << 32U;
// kCallBuiltinArgs keeps its argument count in the 16 bits above the direct-value bit.
inline constexpr unsigned kBuiltinOperandArgumentCountShift = 33U;
inline constexpr std::uint64_t kBuiltinOperandArgumentCountMask = 0xFFFFULL;

[[nodiscard]] constexpr auto builtin_operand(const BuiltinId id,
                                             const BuiltinCallShape shape = BuiltinCallShape::kTupleExpanded)
    -> std::int64_t {
  const auto raw_id = static_cast<std::uint64_t>(static_cast<std::uint32_t>(id));
  const auto raw_shape = shape == BuiltinCallShape::kDirectValue ? kBuiltinOperandDirectValueBit : 0ULL;
  return static_cast<std::int64_t>(raw_id | raw_shape);
}

[[nodiscard]] constexpr auto builtin_id_from_operand(const std::int64_t operand) -> std::optional<BuiltinId> {
  if (operand < 0) {
    return std::nullopt;
  }
  const auto raw_operand = static_cast<std::uint64_t>(operand);
  const auto raw_id = raw_operand & kBuiltinOperandIdMask;
  if (std::cmp_greater_equal(raw_id, builtin_count())) {
    return std::nullopt;
  }
  return static_cast<BuiltinId>(raw_id);
}

[[nodiscard]] constexpr auto builtin_call_shape_from_operand(const std::int64_t operand)
    -> std::optional<BuiltinCallShape> {
  if (!builtin_id_from_operand(operand).has_value()) {
    return std::nullopt;
  }
  const auto raw_operand = static_cast<std::uint64_t>(operand);
  return (raw_operand & kBuiltinOperandDirectValueBit) != 0ULL ? BuiltinCallShape::kDirectValue
                                                               : BuiltinCallShape::kTupleExpanded;
}

[[nodiscard]] inline auto builtin_id_from_name(const std::string_view name) -> std::optional<BuiltinId> {
  if (const auto it = builtin_name_lookup_map().find(name); it != builtin_name_lookup_map().end()) {
    return it->second;
  }
  return std::nullopt;
}

[[nodiscard]] inline auto builtin_id_from_symbol_key(const std::string_view symbol_key) -> std::optional<BuiltinId> {
  if (const auto it = builtin_symbol_key_lookup_map().find(symbol_key); it != builtin_symbol_key_lookup_map().end()) {
    return it->second;
  }
  if (const auto hash_pos = symbol_key.rfind('#'); hash_pos != std::string_view::npos) {
    const auto base_symbol_key = symbol_key.substr(0, hash_pos);
    if (const auto base_id = builtin_id_from_symbol_key(base_symbol_key); base_id.has_value()) {
      return base_id;
    }
  }
  return builtin_id_from_name(symbol_key);
}

[[nodiscard]] constexpr auto builtin_name(const BuiltinId id) -> std::string_view {
  return kBuiltinSpecs[static_cast<std::size_t>(id)].name;
}

[[nodiscard]] constexpr auto builtin_arity_contract(const BuiltinId id) -> BuiltinArityContract {
  return kBuiltinSpecs[static_cast<std::size_t>(id)].arity;
}

[[nodiscard]] constexpr auto builtin_callable_call_shape(const BuiltinId id) -> BuiltinCallShape {
  if (const auto [kind, primary] = builtin_arity_contract(id); kind == BuiltinArityKind::kExact && primary == 1U) {
    return BuiltinCallShape::kDirectValue;
  }
  return BuiltinCallShape::kTupleExpanded;
}

// Whether the builtin takes runtime::Arguments: every builtin but those of exactly one parameter, which take their
// argument as a value, and Std.Println and Std.Printf, which return their argument tuple and so take it whole.
[[nodiscard]] constexpr auto builtin_takes_arguments(const BuiltinId id) -> bool {
  if (id == BuiltinId::Println || id == BuiltinId::Printf) {
    return false;
  }
  return builtin_callable_call_shape(id) == BuiltinCallShape::kTupleExpanded;
}

// The operand of a kCallBuiltinArgs: the builtin and the number of argument values on the stack.
[[nodiscard]] constexpr auto builtin_operand_with_arguments(const BuiltinId id, const std::uint16_t argument_count)
    -> std::int64_t {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(builtin_operand(id)) |
                                   (static_cast<std::uint64_t>(argument_count) << kBuiltinOperandArgumentCountShift));
}

[[nodiscard]] constexpr auto builtin_argument_count_from_operand(const std::int64_t operand) -> std::uint16_t {
  return static_cast<std::uint16_t>((static_cast<std::uint64_t>(operand) >> kBuiltinOperandArgumentCountShift) &
                                    kBuiltinOperandArgumentCountMask);
}

[[nodiscard]] constexpr auto builtin_accepts_arity(const BuiltinId id, const std::size_t arity) -> bool {
  switch (const auto [kind, primary] = builtin_arity_contract(id); kind) {
    case BuiltinArityKind::kExact:
      return arity == primary;
    case BuiltinArityKind::kVariadicMinimum:
      return arity >= primary;
  }
  return false;
}

[[nodiscard]] constexpr auto all_builtin_specs() -> std::span<const BuiltinSpec> { return kBuiltinSpecs; }

[[nodiscard]] constexpr auto builtin_slots(const BuiltinId id) -> std::span<const BuiltinSlot> {
  return kBuiltinSpecs[static_cast<std::size_t>(id)].slots.view();
}

[[nodiscard]] constexpr auto builtin_has_callable_slots(const BuiltinId id) -> bool {
  return std::ranges::any_of(builtin_slots(id), [](const BuiltinSlot& slot) -> bool { return !slot.value_kind; });
}

// The number of parameters of the function the builtin takes as argument `index`, or nothing when that argument is not
// a function slot.
[[nodiscard]] constexpr auto builtin_callable_slot_arity(const BuiltinId id, const std::uint32_t index) -> std::optional<std::uint32_t> {
  for (const BuiltinSlot& slot : builtin_slots(id)) {
    if (slot.index == index && !slot.value_kind.has_value()) {
      return slot.callable_arity;
    }
  }
  return std::nullopt;
}

// The kind of value the builtin takes as argument `index`, or nothing when that argument is not a value slot.
[[nodiscard]] constexpr auto builtin_value_slot_kind(const BuiltinId id, const std::uint32_t index) -> std::optional<BuiltinValueSlotKind> {
  for (const BuiltinSlot& slot : builtin_slots(id)) {
    if (slot.index == index && slot.value_kind.has_value()) {
      return slot.value_kind;
    }
  }
  return std::nullopt;
}

// A hash of what bytecode depends on in the builtin table: each builtin's position, name, symbol key, arity, and
// slots. Bytecode records the hash it was compiled with, and the loader refuses a module compiled against another
// table, so a change to the builtins invalidates old bytecode without a change of BYTECODE_VERSION.
[[nodiscard]] consteval auto compute_builtin_table_hash() -> std::uint64_t {
  std::uint64_t hash = common::kFnv1aOffsetBasis;
  const auto mix = [&hash](const std::uint64_t value) -> void {
    std::array<std::uint8_t, sizeof(value)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
    hash = common::fnv1a(std::span<const std::uint8_t>{bytes}, hash);
  };
  for (const BuiltinSpec& spec : kBuiltinSpecs) {
    mix(static_cast<std::uint64_t>(spec.id));
    hash = common::fnv1a(spec.name, hash);
    mix(spec.name.size());
    hash = common::fnv1a(spec.symbol_key, hash);
    mix(spec.symbol_key.size());
    mix(static_cast<std::uint64_t>(spec.arity.kind));
    mix(spec.arity.primary);
    for (const BuiltinSlot& slot : spec.slots.view()) {
      mix(slot.index);
      mix(slot.value_kind.has_value() ? static_cast<std::uint64_t>(*slot.value_kind) + 1U : 0U);
      mix(slot.callable_arity);
    }
    mix(spec.slots.view().size());
  }
  return hash;
}

inline constexpr std::uint64_t kBuiltinTableHash = compute_builtin_table_hash();

}  // namespace fleaux::vm
