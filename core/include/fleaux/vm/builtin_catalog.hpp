#pragma once

#include <array>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

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
  TupleRange,
  ArrayGetAt,
  ArraySetAt,
  ArrayInsertAt,
  ArrayRemoveAt,
  ArraySlice,
  ArrayConcat,
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
  ResultTag,
  ResultPayload,
  ResultIsOk,
  ResultIsErr,
  ResultUnwrap,
  ResultUnwrapErr,
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
  Slice,
  ToString,
  ToNum,
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
  StringSlice,
  StringFind,
  StringFormat,
  StringRegexIsMatch,
  StringRegexFind,
  StringRegexReplace,
  StringRegexSplit,
  Half,
  Third,
  TwoThirds,
  Sixth,
  ThreeQuarters,
  RootTwo,
  RootThree,
  HalfRootTwo,
  LnTwo,
  LnLnTwo,
  RootLnFour,
  OneDivRootTwo,
  Pi,
  HalfPi,
  ThirdPi,
  SixthPi,
  TwoPi,
  Tau,
  TwoThirdsPi,
  ThreeQuartersPi,
  FourThirdsPi,
  OneDivTwoPi,
  OneDivRootTwoPi,
  RootPi,
  RootHalfPi,
  RootTwoPi,
  LogRootTwoPi,
  OneDivRootPi,
  RootOneDivPi,
  PiMinusThree,
  FourMinusPi,
  PiPowE,
  PiSqr,
  PiSqrDivSix,
  PiCubed,
  CbrtPi,
  OneDivCbrtPi,
  Log2E,
  E,
  ExpMinusHalf,
  ExpMinusOne,
  EPowPi,
  RootE,
  Log10E,
  OneDivLog10E,
  LnTen,
  Degree,
  Radian,
  SinOne,
  CosOne,
  SinhOne,
  CoshOne,
  Phi,
  LnPhi,
  OneDivLnPhi,
  Euler,
  OneDivEuler,
  EulerSqr,
  ZetaTwo,
  ZetaThree,
  Catalan,
  Glaisher,
  Khinchin,
  ExtremeValueSkewness,
  RayleighSkewness,
  RayleighKurtosis,
  RayleighKurtosisExcess,
  TwoDivPi,
  RootTwoDivPi,
  QuarterPi,
  InvPi,
  TwoDivRootPi,
  kCount,
};

enum class BuiltinArityKind : std::uint8_t {
  kUnchecked,
  kExact,
  kOneOf,
  kVariadicMinimum,
};

struct BuiltinArityContract {
  BuiltinArityKind kind{BuiltinArityKind::kUnchecked};
  std::uint8_t primary{0U};
  std::array<std::uint8_t, 4> allowed{};
  std::uint8_t allowed_count{0U};
};

[[nodiscard]] constexpr auto builtin_arity_exact(const std::uint8_t arity) -> BuiltinArityContract {
  return BuiltinArityContract{.kind = BuiltinArityKind::kExact, .primary = arity};
}

template <std::size_t N>
[[nodiscard]] constexpr auto builtin_arity_one_of(const std::array<std::uint8_t, N>& arities) -> BuiltinArityContract {
  static_assert(N > 0U);
  static_assert(N <= 4U);

  BuiltinArityContract contract{.kind = BuiltinArityKind::kOneOf, .allowed_count = static_cast<std::uint8_t>(N)};
  for (std::size_t index = 0; index < N; ++index) {
    contract.allowed[index] = arities[index];
  }
  return contract;
}

[[nodiscard]] constexpr auto builtin_arity_variadic_minimum(const std::uint8_t minimum_arity) -> BuiltinArityContract {
  return BuiltinArityContract{.kind = BuiltinArityKind::kVariadicMinimum, .primary = minimum_arity};
}

struct BuiltinSpec {
  BuiltinId id;
  std::string_view name;
  std::string_view symbol_key{};

  BuiltinArityContract arity{};
};

struct ConstantBuiltinSpec {
  BuiltinId id;
  std::string_view name;
  double value;
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
    BuiltinSpec{.id = BuiltinId::BitAnd, .name = "Std.Bit.And", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::BitOr, .name = "Std.Bit.Or", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::BitXor, .name = "Std.Bit.Xor", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::BitNot, .name = "Std.Bit.Not", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::BitShiftLeft, .name = "Std.Bit.ShiftLeft", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::BitShiftRight, .name = "Std.Bit.ShiftRight", .arity = builtin_arity_exact(2U)},
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
    BuiltinSpec{.id = BuiltinId::Match, .name = "Std.Match"},
    BuiltinSpec{.id = BuiltinId::Apply, .name = "Std.Apply", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Branch, .name = "Std.Branch", .arity = builtin_arity_exact(4U)},
    BuiltinSpec{.id = BuiltinId::Loop, .name = "Std.Loop", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::LoopN, .name = "Std.LoopN", .arity = builtin_arity_exact(4U)},
    BuiltinSpec{.id = BuiltinId::Printf, .name = "Std.Printf", .arity = builtin_arity_variadic_minimum(1U)},
    BuiltinSpec{.id = BuiltinId::Println, .name = "Std.Println"},
    BuiltinSpec{.id = BuiltinId::GetArgs, .name = "Std.GetArgs", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::Type, .name = "Std.Type"},
    BuiltinSpec{
        .id = BuiltinId::InputVoid, .name = "Std.Input", .symbol_key = "Std.Input#0", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::InputString,
                .name = "Std.Input",
                .symbol_key = "Std.Input#1",
                .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Help, .name = "Std.Help"},
    BuiltinSpec{
        .id = BuiltinId::ExitVoid, .name = "Std.Exit", .symbol_key = "Std.Exit#0", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{
        .id = BuiltinId::ExitInt64, .name = "Std.Exit", .symbol_key = "Std.Exit#1", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::Cwd, .name = "Std.OS.Cwd", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSEnv, .name = "Std.OS.Env"},
    BuiltinSpec{.id = BuiltinId::OSHasEnv, .name = "Std.OS.HasEnv"},
    BuiltinSpec{.id = BuiltinId::OSSetEnv, .name = "Std.OS.SetEnv", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::OSUnsetEnv, .name = "Std.OS.UnsetEnv"},
    BuiltinSpec{.id = BuiltinId::OSIsWindows, .name = "Std.OS.IsWindows", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSIsLinux, .name = "Std.OS.IsLinux", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSIsMacOS, .name = "Std.OS.IsMacOS", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSHome, .name = "Std.OS.Home", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSTempDir, .name = "Std.OS.TempDir", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSExec, .name = "Std.OS.Exec"},
    BuiltinSpec{.id = BuiltinId::OSMakeTempFile, .name = "Std.OS.MakeTempFile", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::OSMakeTempDir, .name = "Std.OS.MakeTempDir", .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::PathJoin, .name = "Std.Path.Join", .arity = builtin_arity_variadic_minimum(2U)},
    BuiltinSpec{.id = BuiltinId::PathNormalize, .name = "Std.Path.Normalize"},
    BuiltinSpec{.id = BuiltinId::PathBasename, .name = "Std.Path.Basename"},
    BuiltinSpec{.id = BuiltinId::PathDirname, .name = "Std.Path.Dirname"},
    BuiltinSpec{.id = BuiltinId::PathExists, .name = "Std.Path.Exists"},
    BuiltinSpec{.id = BuiltinId::PathIsFile, .name = "Std.Path.IsFile"},
    BuiltinSpec{.id = BuiltinId::PathIsDir, .name = "Std.Path.IsDir"},
    BuiltinSpec{.id = BuiltinId::PathAbsolute, .name = "Std.Path.Absolute"},
    BuiltinSpec{.id = BuiltinId::PathExtension, .name = "Std.Path.Extension"},
    BuiltinSpec{.id = BuiltinId::PathStem, .name = "Std.Path.Stem"},
    BuiltinSpec{.id = BuiltinId::PathWithExtension, .name = "Std.Path.WithExtension", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::PathWithBasename, .name = "Std.Path.WithBasename", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::FileReadText, .name = "Std.File.ReadText"},
    BuiltinSpec{.id = BuiltinId::FileWriteText, .name = "Std.File.WriteText", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::FileAppendText, .name = "Std.File.AppendText", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::FileReadLines, .name = "Std.File.ReadLines"},
    BuiltinSpec{.id = BuiltinId::FileDelete, .name = "Std.File.Delete"},
    BuiltinSpec{.id = BuiltinId::FileSize, .name = "Std.File.Size"},
    BuiltinSpec{.id = BuiltinId::FileOpen, .name = "Std.File.Open"},
    BuiltinSpec{.id = BuiltinId::FileReadLine, .name = "Std.File.ReadLine"},
    BuiltinSpec{.id = BuiltinId::FileReadChunk, .name = "Std.File.ReadChunk", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::FileWriteChunk, .name = "Std.File.WriteChunk", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::FileFlush, .name = "Std.File.Flush"},
    BuiltinSpec{.id = BuiltinId::FileClose, .name = "Std.File.Close"},
    BuiltinSpec{.id = BuiltinId::FileWithOpen, .name = "Std.File.WithOpen", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::DirCreate, .name = "Std.Dir.Create"},
    BuiltinSpec{.id = BuiltinId::DirDelete, .name = "Std.Dir.Delete"},
    BuiltinSpec{.id = BuiltinId::DirList, .name = "Std.Dir.List"},
    BuiltinSpec{.id = BuiltinId::DirListFull, .name = "Std.Dir.ListFull"},
    BuiltinSpec{.id = BuiltinId::TupleAppend, .name = "Std.Tuple.Append", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TuplePrepend, .name = "Std.Tuple.Prepend", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleReverse, .name = "Std.Tuple.Reverse"},
    BuiltinSpec{.id = BuiltinId::TupleContains, .name = "Std.Tuple.Contains", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleZip, .name = "Std.Tuple.Zip", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleMap, .name = "Std.Tuple.Map", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleFilter, .name = "Std.Tuple.Filter", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleSort, .name = "Std.Tuple.Sort"},
    BuiltinSpec{.id = BuiltinId::TupleUnique, .name = "Std.Tuple.Unique"},
    BuiltinSpec{.id = BuiltinId::TupleMin, .name = "Std.Tuple.Min"},
    BuiltinSpec{.id = BuiltinId::TupleMax, .name = "Std.Tuple.Max"},
    BuiltinSpec{.id = BuiltinId::TupleReduce, .name = "Std.Tuple.Reduce", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::TupleFindIndex, .name = "Std.Tuple.FindIndex", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleAny, .name = "Std.Tuple.Any", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleAll, .name = "Std.Tuple.All", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TupleRange,
                .name = "Std.Tuple.Range",
                .arity = builtin_arity_one_of(std::to_array<std::uint8_t>({1U, 2U, 3U}))},
    BuiltinSpec{.id = BuiltinId::ArrayGetAt, .name = "Std.Array.GetAt", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::ArraySetAt, .name = "Std.Array.SetAt", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::ArrayInsertAt, .name = "Std.Array.InsertAt", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::ArrayRemoveAt, .name = "Std.Array.RemoveAt", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::ArraySlice, .name = "Std.Array.Slice", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::ArrayConcat, .name = "Std.Array.Concat", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::ArraySetAt2D, .name = "Std.Array.SetAt2D", .arity = builtin_arity_exact(4U)},
    BuiltinSpec{.id = BuiltinId::ArrayFill, .name = "Std.Array.Fill", .arity = builtin_arity_exact(4U)},
    BuiltinSpec{.id = BuiltinId::ArrayTranspose2D, .name = "Std.Array.Transpose2D"},
    BuiltinSpec{.id = BuiltinId::ArraySlice2D, .name = "Std.Array.Slice2D", .arity = builtin_arity_exact(5U)},
    BuiltinSpec{.id = BuiltinId::ArrayReshape, .name = "Std.Array.Reshape", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::ArrayRank, .name = "Std.Array.Rank"},
    BuiltinSpec{.id = BuiltinId::ArrayShape, .name = "Std.Array.Shape"},
    BuiltinSpec{.id = BuiltinId::ArrayFlatten, .name = "Std.Array.Flatten"},
    BuiltinSpec{.id = BuiltinId::ArrayGetAtND, .name = "Std.Array.GetAtND", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::ArraySetAtND, .name = "Std.Array.SetAtND", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::ArrayReshapeND, .name = "Std.Array.ReshapeND", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::DictCreateVoid,
                .name = "Std.Dict.Create",
                .symbol_key = "Std.Dict.Create#0",
                .arity = builtin_arity_exact(0U)},
    BuiltinSpec{.id = BuiltinId::DictCreateDict,
                .name = "Std.Dict.Create",
                .symbol_key = "Std.Dict.Create#1",
                .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::DictSet, .name = "Std.Dict.Set", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::DictGet, .name = "Std.Dict.Get", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::DictGetDefault, .name = "Std.Dict.GetDefault", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::DictContains, .name = "Std.Dict.Contains", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::DictDelete, .name = "Std.Dict.Delete", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::DictMerge, .name = "Std.Dict.Merge", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::DictKeys, .name = "Std.Dict.Keys"},
    BuiltinSpec{.id = BuiltinId::DictValues, .name = "Std.Dict.Values"},
    BuiltinSpec{.id = BuiltinId::DictEntries, .name = "Std.Dict.Entries"},
    BuiltinSpec{.id = BuiltinId::DictClear, .name = "Std.Dict.Clear"},
    BuiltinSpec{.id = BuiltinId::DictLength, .name = "Std.Dict.Length"},
    BuiltinSpec{.id = BuiltinId::Cast, .name = "Std.Cast"},
    BuiltinSpec{.id = BuiltinId::ToInt64, .name = "Std.ToInt64", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ToUInt64, .name = "Std.ToUInt64", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::ToFloat64, .name = "Std.ToFloat64", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::RandomCreate, .name = "Std.Random.Create"},
    BuiltinSpec{.id = BuiltinId::RandomNextUInt64, .name = "Std.Random.NextUInt64"},
    BuiltinSpec{.id = BuiltinId::RandomNextInt64, .name = "Std.Random.NextInt64"},
    BuiltinSpec{.id = BuiltinId::RandomNextFloat64, .name = "Std.Random.NextFloat64"},
    BuiltinSpec{.id = BuiltinId::RandomNextBool, .name = "Std.Random.NextBool"},
    BuiltinSpec{.id = BuiltinId::RandomSplit, .name = "Std.Random.Split"},
    BuiltinSpec{.id = BuiltinId::MathFloor, .name = "Std.Math.Floor"},
    BuiltinSpec{.id = BuiltinId::MathCeil, .name = "Std.Math.Ceil"},
    BuiltinSpec{.id = BuiltinId::MathAbs, .name = "Std.Math.Abs"},
    BuiltinSpec{.id = BuiltinId::MathLog, .name = "Std.Math.Log"},
    BuiltinSpec{.id = BuiltinId::MathClamp, .name = "Std.Math.Clamp", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::Sqrt, .name = "Std.Math.Sqrt"},
    BuiltinSpec{.id = BuiltinId::Sin, .name = "Std.Math.Sin"},
    BuiltinSpec{.id = BuiltinId::Cos, .name = "Std.Math.Cos"},
    BuiltinSpec{.id = BuiltinId::Tan, .name = "Std.Math.Tan"},
    BuiltinSpec{.id = BuiltinId::ResultOk, .name = "Std.Result.Ok"},
    BuiltinSpec{.id = BuiltinId::ResultErr, .name = "Std.Result.Err"},
    BuiltinSpec{.id = BuiltinId::ResultTag, .name = "Std.Result.Tag"},
    BuiltinSpec{.id = BuiltinId::ResultPayload, .name = "Std.Result.Payload"},
    BuiltinSpec{.id = BuiltinId::ResultIsOk, .name = "Std.Result.IsOk"},
    BuiltinSpec{.id = BuiltinId::ResultIsErr, .name = "Std.Result.IsErr"},
    BuiltinSpec{.id = BuiltinId::ResultUnwrap, .name = "Std.Result.Unwrap"},
    BuiltinSpec{.id = BuiltinId::ResultUnwrapErr, .name = "Std.Result.UnwrapErr"},
    BuiltinSpec{.id = BuiltinId::Try, .name = "Std.Try", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::ParallelMap, .name = "Std.Parallel.Map", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{
        .id = BuiltinId::ParallelWithOptions, .name = "Std.Parallel.WithOptions", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::ParallelForEach, .name = "Std.Parallel.ForEach", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::ParallelReduce, .name = "Std.Parallel.Reduce", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::TaskSpawn, .name = "Std.Task.Spawn", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::TaskAwait, .name = "Std.Task.Await"},
    BuiltinSpec{.id = BuiltinId::TaskAwaitAll, .name = "Std.Task.AwaitAll"},
    BuiltinSpec{.id = BuiltinId::TaskCancel, .name = "Std.Task.Cancel"},
    BuiltinSpec{.id = BuiltinId::TaskWithTimeout, .name = "Std.Task.WithTimeout", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Wrap, .name = "Std.Wrap"},
    BuiltinSpec{.id = BuiltinId::Unwrap, .name = "Std.Unwrap"},
    BuiltinSpec{.id = BuiltinId::First, .name = "Std.First"},
    BuiltinSpec{.id = BuiltinId::Second, .name = "Std.Second"},
    BuiltinSpec{.id = BuiltinId::ElementAt, .name = "Std.ElementAt", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Length, .name = "Std.Length"},
    BuiltinSpec{.id = BuiltinId::Take, .name = "Std.Take", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Drop, .name = "Std.Drop", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Slice, .name = "Std.Slice"},
    BuiltinSpec{.id = BuiltinId::ToString, .name = "Std.ToString"},
    BuiltinSpec{.id = BuiltinId::ToNum, .name = "Std.ToNum", .arity = builtin_arity_exact(1U)},
    BuiltinSpec{.id = BuiltinId::StringParseInt64, .name = "Std.String.ParseInt64"},
    BuiltinSpec{.id = BuiltinId::StringParseUInt64, .name = "Std.String.ParseUInt64"},
    BuiltinSpec{.id = BuiltinId::StringParseFloat64, .name = "Std.String.ParseFloat64"},
    BuiltinSpec{.id = BuiltinId::StringUpper, .name = "Std.String.Upper"},
    BuiltinSpec{.id = BuiltinId::StringLower, .name = "Std.String.Lower"},
    BuiltinSpec{.id = BuiltinId::StringTrim, .name = "Std.String.Trim"},
    BuiltinSpec{.id = BuiltinId::StringTrimStart, .name = "Std.String.TrimStart"},
    BuiltinSpec{.id = BuiltinId::StringTrimEnd, .name = "Std.String.TrimEnd"},
    BuiltinSpec{.id = BuiltinId::StringSplit, .name = "Std.String.Split", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringJoin, .name = "Std.String.Join", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringReplace, .name = "Std.String.Replace", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::StringContains, .name = "Std.String.Contains", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringStartsWith, .name = "Std.String.StartsWith", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringEndsWith, .name = "Std.String.EndsWith", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringLength, .name = "Std.String.Length"},
    BuiltinSpec{.id = BuiltinId::StringCharAt, .name = "Std.String.CharAt", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringSlice,
                .name = "Std.String.Slice",
                .arity = builtin_arity_one_of(std::to_array<std::uint8_t>({2U, 3U}))},
    BuiltinSpec{.id = BuiltinId::StringFind,
                .name = "Std.String.Find",
                .arity = builtin_arity_one_of(std::to_array<std::uint8_t>({2U, 3U}))},
    BuiltinSpec{.id = BuiltinId::StringFormat,
                .name = "Std.String.Format",
                .arity = builtin_arity_variadic_minimum(1U)},
    BuiltinSpec{
        .id = BuiltinId::StringRegexIsMatch, .name = "Std.String.Regex.IsMatch", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::StringRegexFind, .name = "Std.String.Regex.Find", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{
        .id = BuiltinId::StringRegexReplace, .name = "Std.String.Regex.Replace", .arity = builtin_arity_exact(3U)},
    BuiltinSpec{.id = BuiltinId::StringRegexSplit, .name = "Std.String.Regex.Split", .arity = builtin_arity_exact(2U)},
    BuiltinSpec{.id = BuiltinId::Half, .name = "Std.Half"},
    BuiltinSpec{.id = BuiltinId::Third, .name = "Std.Third"},
    BuiltinSpec{.id = BuiltinId::TwoThirds, .name = "Std.TwoThirds"},
    BuiltinSpec{.id = BuiltinId::Sixth, .name = "Std.Sixth"},
    BuiltinSpec{.id = BuiltinId::ThreeQuarters, .name = "Std.ThreeQuarters"},
    BuiltinSpec{.id = BuiltinId::RootTwo, .name = "Std.RootTwo"},
    BuiltinSpec{.id = BuiltinId::RootThree, .name = "Std.RootThree"},
    BuiltinSpec{.id = BuiltinId::HalfRootTwo, .name = "Std.HalfRootTwo"},
    BuiltinSpec{.id = BuiltinId::LnTwo, .name = "Std.LnTwo"},
    BuiltinSpec{.id = BuiltinId::LnLnTwo, .name = "Std.LnLnTwo"},
    BuiltinSpec{.id = BuiltinId::RootLnFour, .name = "Std.RootLnFour"},
    BuiltinSpec{.id = BuiltinId::OneDivRootTwo, .name = "Std.OneDivRootTwo"},
    BuiltinSpec{.id = BuiltinId::Pi, .name = "Std.Pi"},
    BuiltinSpec{.id = BuiltinId::HalfPi, .name = "Std.HalfPi"},
    BuiltinSpec{.id = BuiltinId::ThirdPi, .name = "Std.ThirdPi"},
    BuiltinSpec{.id = BuiltinId::SixthPi, .name = "Std.SixthPi"},
    BuiltinSpec{.id = BuiltinId::TwoPi, .name = "Std.TwoPi"},
    BuiltinSpec{.id = BuiltinId::Tau, .name = "Std.Tau"},
    BuiltinSpec{.id = BuiltinId::TwoThirdsPi, .name = "Std.TwoThirdsPi"},
    BuiltinSpec{.id = BuiltinId::ThreeQuartersPi, .name = "Std.ThreeQuartersPi"},
    BuiltinSpec{.id = BuiltinId::FourThirdsPi, .name = "Std.FourThirdsPi"},
    BuiltinSpec{.id = BuiltinId::OneDivTwoPi, .name = "Std.OneDivTwoPi"},
    BuiltinSpec{.id = BuiltinId::OneDivRootTwoPi, .name = "Std.OneDivRootTwoPi"},
    BuiltinSpec{.id = BuiltinId::RootPi, .name = "Std.RootPi"},
    BuiltinSpec{.id = BuiltinId::RootHalfPi, .name = "Std.RootHalfPi"},
    BuiltinSpec{.id = BuiltinId::RootTwoPi, .name = "Std.RootTwoPi"},
    BuiltinSpec{.id = BuiltinId::LogRootTwoPi, .name = "Std.LogRootTwoPi"},
    BuiltinSpec{.id = BuiltinId::OneDivRootPi, .name = "Std.OneDivRootPi"},
    BuiltinSpec{.id = BuiltinId::RootOneDivPi, .name = "Std.RootOneDivPi"},
    BuiltinSpec{.id = BuiltinId::PiMinusThree, .name = "Std.PiMinusThree"},
    BuiltinSpec{.id = BuiltinId::FourMinusPi, .name = "Std.FourMinusPi"},
    BuiltinSpec{.id = BuiltinId::PiPowE, .name = "Std.PiPowE"},
    BuiltinSpec{.id = BuiltinId::PiSqr, .name = "Std.PiSqr"},
    BuiltinSpec{.id = BuiltinId::PiSqrDivSix, .name = "Std.PiSqrDivSix"},
    BuiltinSpec{.id = BuiltinId::PiCubed, .name = "Std.PiCubed"},
    BuiltinSpec{.id = BuiltinId::CbrtPi, .name = "Std.CbrtPi"},
    BuiltinSpec{.id = BuiltinId::OneDivCbrtPi, .name = "Std.OneDivCbrtPi"},
    BuiltinSpec{.id = BuiltinId::Log2E, .name = "Std.Log2E"},
    BuiltinSpec{.id = BuiltinId::E, .name = "Std.E"},
    BuiltinSpec{.id = BuiltinId::ExpMinusHalf, .name = "Std.ExpMinusHalf"},
    BuiltinSpec{.id = BuiltinId::ExpMinusOne, .name = "Std.ExpMinusOne"},
    BuiltinSpec{.id = BuiltinId::EPowPi, .name = "Std.EPowPi"},
    BuiltinSpec{.id = BuiltinId::RootE, .name = "Std.RootE"},
    BuiltinSpec{.id = BuiltinId::Log10E, .name = "Std.Log10E"},
    BuiltinSpec{.id = BuiltinId::OneDivLog10E, .name = "Std.OneDivLog10E"},
    BuiltinSpec{.id = BuiltinId::LnTen, .name = "Std.LnTen"},
    BuiltinSpec{.id = BuiltinId::Degree, .name = "Std.Degree"},
    BuiltinSpec{.id = BuiltinId::Radian, .name = "Std.Radian"},
    BuiltinSpec{.id = BuiltinId::SinOne, .name = "Std.SinOne"},
    BuiltinSpec{.id = BuiltinId::CosOne, .name = "Std.CosOne"},
    BuiltinSpec{.id = BuiltinId::SinhOne, .name = "Std.SinhOne"},
    BuiltinSpec{.id = BuiltinId::CoshOne, .name = "Std.CoshOne"},
    BuiltinSpec{.id = BuiltinId::Phi, .name = "Std.Phi"},
    BuiltinSpec{.id = BuiltinId::LnPhi, .name = "Std.LnPhi"},
    BuiltinSpec{.id = BuiltinId::OneDivLnPhi, .name = "Std.OneDivLnPhi"},
    BuiltinSpec{.id = BuiltinId::Euler, .name = "Std.Euler"},
    BuiltinSpec{.id = BuiltinId::OneDivEuler, .name = "Std.OneDivEuler"},
    BuiltinSpec{.id = BuiltinId::EulerSqr, .name = "Std.EulerSqr"},
    BuiltinSpec{.id = BuiltinId::ZetaTwo, .name = "Std.ZetaTwo"},
    BuiltinSpec{.id = BuiltinId::ZetaThree, .name = "Std.ZetaThree"},
    BuiltinSpec{.id = BuiltinId::Catalan, .name = "Std.Catalan"},
    BuiltinSpec{.id = BuiltinId::Glaisher, .name = "Std.Glaisher"},
    BuiltinSpec{.id = BuiltinId::Khinchin, .name = "Std.Khinchin"},
    BuiltinSpec{.id = BuiltinId::ExtremeValueSkewness, .name = "Std.ExtremeValueSkewness"},
    BuiltinSpec{.id = BuiltinId::RayleighSkewness, .name = "Std.RayleighSkewness"},
    BuiltinSpec{.id = BuiltinId::RayleighKurtosis, .name = "Std.RayleighKurtosis"},
    BuiltinSpec{.id = BuiltinId::RayleighKurtosisExcess, .name = "Std.RayleighKurtosisExcess"},
    BuiltinSpec{.id = BuiltinId::TwoDivPi, .name = "Std.TwoDivPi"},
    BuiltinSpec{.id = BuiltinId::RootTwoDivPi, .name = "Std.RootTwoDivPi"},
    BuiltinSpec{.id = BuiltinId::QuarterPi, .name = "Std.QuarterPi"},
    BuiltinSpec{.id = BuiltinId::InvPi, .name = "Std.InvPi"},
    BuiltinSpec{.id = BuiltinId::TwoDivRootPi, .name = "Std.TwoDivRootPi"},
});

inline constexpr auto kConstantBuiltinSpecs = std::to_array<ConstantBuiltinSpec>({
    // clang-format off
    ConstantBuiltinSpec{.id = BuiltinId::Half, .name = "Std.Half", .value = 0.5000000000000000000000000000000000000e-01},
    ConstantBuiltinSpec{.id = BuiltinId::Third, .name = "Std.Third", .value = 3.333333333333333333333333333333333333e-01},
    ConstantBuiltinSpec{.id = BuiltinId::TwoThirds, .name = "Std.TwoThirds", .value = 6.666666666666666666666666666666666666e-01},
    ConstantBuiltinSpec{.id = BuiltinId::Sixth, .name = "Std.Sixth", .value = 1.666666666666666666666666666666666666e-01},
    ConstantBuiltinSpec{.id = BuiltinId::ThreeQuarters, .name = "Std.ThreeQuarters", .value = 7.500000000000000000000000000000000000e-01},
    ConstantBuiltinSpec{.id = BuiltinId::RootTwo, .name = "Std.RootTwo", .value = std::numbers::sqrt2},
    ConstantBuiltinSpec{.id = BuiltinId::RootThree, .name = "Std.RootThree", .value = std::numbers::sqrt3},
    ConstantBuiltinSpec{.id = BuiltinId::HalfRootTwo, .name = "Std.HalfRootTwo", .value = 7.071067811865475244008443621048490392e-01},
    ConstantBuiltinSpec{.id = BuiltinId::LnTwo, .name = "Std.LnTwo", .value = std::numbers::ln2},
    ConstantBuiltinSpec{.id = BuiltinId::LnLnTwo, .name = "Std.LnLnTwo", .value = -3.665129205816643270124391582326694694e-01},
    ConstantBuiltinSpec{.id = BuiltinId::RootLnFour, .name = "Std.RootLnFour", .value = 1.177410022515474691011569326459699637e+00},
    ConstantBuiltinSpec{.id = BuiltinId::OneDivRootTwo, .name = "Std.OneDivRootTwo", .value = 7.071067811865475244008443621048490392e-01},
    ConstantBuiltinSpec{.id = BuiltinId::Pi, .name = "Std.Pi", .value = std::numbers::pi},
    ConstantBuiltinSpec{.id = BuiltinId::HalfPi, .name = "Std.HalfPi", .value = 1.570796326794896619231321691639751442e+00},
    ConstantBuiltinSpec{.id = BuiltinId::ThirdPi, .name = "Std.ThirdPi", .value = 1.047197551196597746154214461093167628e+00},
    ConstantBuiltinSpec{.id = BuiltinId::SixthPi, .name = "Std.SixthPi", .value = 5.235987755982988730771072305465838140e-01},
    ConstantBuiltinSpec{.id = BuiltinId::TwoPi, .name = "Std.TwoPi", .value = 6.283185307179586476925286766559005768e+00},
    ConstantBuiltinSpec{.id = BuiltinId::Tau, .name = "Std.Tau", .value = 6.283185307179586476925286766559005768e+00},
    ConstantBuiltinSpec{.id = BuiltinId::TwoThirdsPi, .name = "Std.TwoThirdsPi", .value = 2.094395102393195492308428922186335256e+00},
    ConstantBuiltinSpec{.id = BuiltinId::ThreeQuartersPi, .name = "Std.ThreeQuartersPi", .value = 2.356194490192344928846982537459627163e+00},
    ConstantBuiltinSpec{.id = BuiltinId::FourThirdsPi, .name = "Std.FourThirdsPi", .value = 4.188790204786390984616857844372670512e+00},
    ConstantBuiltinSpec{.id = BuiltinId::OneDivTwoPi, .name = "Std.OneDivTwoPi", .value = 1.591549430918953357688837633725143620e-01},
    ConstantBuiltinSpec{.id = BuiltinId::OneDivRootTwoPi, .name = "Std.OneDivRootTwoPi", .value = 3.989422804014326779399460599343818684e-01},
    ConstantBuiltinSpec{.id = BuiltinId::RootPi, .name = "Std.RootPi", .value = 1.772453850905516027298167483341145182e+00},
    ConstantBuiltinSpec{.id = BuiltinId::RootHalfPi, .name = "Std.RootHalfPi", .value = 1.253314137315500251207882642405522626e+00},
    ConstantBuiltinSpec{.id = BuiltinId::RootTwoPi, .name = "Std.RootTwoPi", .value = 2.506628274631000502415765284811045253e+00},
    ConstantBuiltinSpec{.id = BuiltinId::LogRootTwoPi, .name = "Std.LogRootTwoPi", .value = 9.189385332046727417803297364056176398e-01},
    ConstantBuiltinSpec{.id = BuiltinId::OneDivRootPi, .name = "Std.OneDivRootPi", .value = std::numbers::inv_sqrtpi},
    ConstantBuiltinSpec{.id = BuiltinId::RootOneDivPi, .name = "Std.RootOneDivPi", .value = std::numbers::inv_sqrtpi},
    ConstantBuiltinSpec{.id = BuiltinId::PiMinusThree, .name = "Std.PiMinusThree", .value = 1.415926535897932384626433832795028841e-01},
    ConstantBuiltinSpec{.id = BuiltinId::FourMinusPi, .name = "Std.FourMinusPi", .value = 8.584073464102067615373566167204971158e-01},
    ConstantBuiltinSpec{.id = BuiltinId::PiPowE, .name = "Std.PiPowE", .value = 2.245915771836104547342715220454373502e+01},
    ConstantBuiltinSpec{.id = BuiltinId::PiSqr, .name = "Std.PiSqr", .value = 9.869604401089358618834490999876151135e+00},
    ConstantBuiltinSpec{.id = BuiltinId::PiSqrDivSix, .name = "Std.PiSqrDivSix", .value = 1.644934066848226436472415166646025189e+00},
    ConstantBuiltinSpec{.id = BuiltinId::PiCubed, .name = "Std.PiCubed", .value = 3.100627668029982017547631506710139520e+01},
    ConstantBuiltinSpec{.id = BuiltinId::CbrtPi, .name = "Std.CbrtPi", .value = 1.464591887561523263020142527263790391e+00},
    ConstantBuiltinSpec{.id = BuiltinId::OneDivCbrtPi, .name = "Std.OneDivCbrtPi", .value = 6.827840632552956814670208331581645981e-01},
    ConstantBuiltinSpec{.id = BuiltinId::Log2E, .name = "Std.Log2E", .value = std::numbers::log2e},
    ConstantBuiltinSpec{.id = BuiltinId::E, .name = "Std.E", .value = std::numbers::e},
    ConstantBuiltinSpec{.id = BuiltinId::ExpMinusHalf, .name = "Std.ExpMinusHalf", .value = 6.065306597126334236037995349911804534e-01},
    ConstantBuiltinSpec{.id = BuiltinId::ExpMinusOne, .name = "Std.ExpMinusOne", .value = 3.678794411714423215955237701614608674e-01},
    ConstantBuiltinSpec{.id = BuiltinId::EPowPi, .name = "Std.EPowPi", .value = 2.314069263277926900572908636794854738e+01},
    ConstantBuiltinSpec{.id = BuiltinId::RootE, .name = "Std.RootE", .value = 1.648721270700128146848650787814163571e+00},
    ConstantBuiltinSpec{.id = BuiltinId::Log10E, .name = "Std.Log10E", .value = std::numbers::log10e},
    ConstantBuiltinSpec{.id = BuiltinId::OneDivLog10E, .name = "Std.OneDivLog10E", .value = std::numbers::ln10},
    ConstantBuiltinSpec{.id = BuiltinId::LnTen, .name = "Std.LnTen", .value = std::numbers::ln10},
    ConstantBuiltinSpec{.id = BuiltinId::Degree, .name = "Std.Degree", .value = 1.745329251994329576923690768488612713e-02},
    ConstantBuiltinSpec{.id = BuiltinId::Radian, .name = "Std.Radian", .value = 5.729577951308232087679815481410517033e+01},
    ConstantBuiltinSpec{.id = BuiltinId::SinOne, .name = "Std.SinOne", .value = 8.414709848078965066525023216302989996e-01},
    ConstantBuiltinSpec{.id = BuiltinId::CosOne, .name = "Std.CosOne", .value = 5.403023058681397174009366074429766037e-01},
    ConstantBuiltinSpec{.id = BuiltinId::SinhOne, .name = "Std.SinhOne", .value = 1.175201193643801456882381850595600815e+00},
    ConstantBuiltinSpec{.id = BuiltinId::CoshOne, .name = "Std.CoshOne", .value = 1.543080634815243778477905620757061682e+00},
    ConstantBuiltinSpec{.id = BuiltinId::Phi, .name = "Std.Phi", .value = std::numbers::phi},
    ConstantBuiltinSpec{.id = BuiltinId::LnPhi, .name = "Std.LnPhi", .value = 4.812118250596034474977589134243684231e-01},
    ConstantBuiltinSpec{.id = BuiltinId::OneDivLnPhi, .name = "Std.OneDivLnPhi", .value = 2.078086921235027537601322606117795767e+00},
    ConstantBuiltinSpec{.id = BuiltinId::Euler, .name = "Std.Euler", .value = std::numbers::egamma},
    ConstantBuiltinSpec{.id = BuiltinId::OneDivEuler, .name = "Std.OneDivEuler", .value = std::numbers::sqrt3},
    ConstantBuiltinSpec{.id = BuiltinId::EulerSqr, .name = "Std.EulerSqr", .value = 3.331779238077186743183761363552442266e-01},
    ConstantBuiltinSpec{.id = BuiltinId::ZetaTwo, .name = "Std.ZetaTwo", .value = 1.644934066848226436472415166646025189e+00},
    ConstantBuiltinSpec{.id = BuiltinId::ZetaThree, .name = "Std.ZetaThree", .value = 1.202056903159594285399738161511449990e+00},
    ConstantBuiltinSpec{.id = BuiltinId::Catalan, .name = "Std.Catalan", .value = 9.159655941772190150546035149323841107e-01},
    ConstantBuiltinSpec{.id = BuiltinId::Glaisher, .name = "Std.Glaisher", .value = 1.282427129100622636875342568869791727e+00},
    ConstantBuiltinSpec{.id = BuiltinId::Khinchin, .name = "Std.Khinchin", .value = 2.685452001065306445309714835481795693e+00},
    ConstantBuiltinSpec{.id = BuiltinId::ExtremeValueSkewness, .name = "Std.ExtremeValueSkewness", .value = 1.139547099404648657492793019389846112e+00},
    ConstantBuiltinSpec{.id = BuiltinId::RayleighSkewness, .name = "Std.RayleighSkewness", .value = 6.311106578189371381918993515442277798e-01},
    ConstantBuiltinSpec{.id = BuiltinId::RayleighKurtosis, .name = "Std.RayleighKurtosis", .value = 3.245089300687638062848660410619754415e+00},
    ConstantBuiltinSpec{.id = BuiltinId::RayleighKurtosisExcess, .name = "Std.RayleighKurtosisExcess", .value = 2.450893006876380628486604106197544154e-01},
    ConstantBuiltinSpec{.id = BuiltinId::TwoDivPi, .name = "Std.TwoDivPi", .value = 6.366197723675813430755350534900574481e-01},
    ConstantBuiltinSpec{.id = BuiltinId::RootTwoDivPi, .name = "Std.RootTwoDivPi", .value = 7.978845608028653558798921198687637369e-01},
    ConstantBuiltinSpec{.id = BuiltinId::QuarterPi, .name = "Std.QuarterPi", .value = 0.785398163397448309615660845819875721049292},
    ConstantBuiltinSpec{.id = BuiltinId::InvPi, .name = "Std.InvPi", .value = std::numbers::inv_pi},
    ConstantBuiltinSpec{.id = BuiltinId::TwoDivRootPi, .name = "Std.TwoDivRootPi", .value = 1.12837916709551257389615890312154517168810125},
    // clang-format on
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

[[nodiscard]] constexpr auto callable_builtin_count() -> std::size_t {
  return static_cast<std::size_t>(BuiltinId::Half);
}

[[nodiscard]] constexpr auto builtin_operand(const BuiltinId id) -> std::int64_t {
  return static_cast<std::int64_t>(id);
}

[[nodiscard]] constexpr auto builtin_id_from_operand(const std::int64_t operand) -> std::optional<BuiltinId> {
  if (operand < 0 || std::cmp_greater_equal(operand, builtin_count())) {
    return std::nullopt;
  }
  return static_cast<BuiltinId>(operand);
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

[[nodiscard]] constexpr auto builtin_has_arity_contract(const BuiltinId id) -> bool {
  return builtin_arity_contract(id).kind != BuiltinArityKind::kUnchecked;
}

[[nodiscard]] constexpr auto builtin_accepts_arity(const BuiltinId id, const std::size_t arity) -> bool {
  switch (const auto [kind, primary, allowed, allowed_count] = builtin_arity_contract(id); kind) {
    case BuiltinArityKind::kUnchecked:
      return true;
    case BuiltinArityKind::kExact:
      return arity == primary;
    case BuiltinArityKind::kOneOf:
      for (std::size_t index = 0; index < allowed_count; ++index) {
        if (arity == allowed[index]) {
          return true;
        }
      }
      return false;
    case BuiltinArityKind::kVariadicMinimum:
      return arity >= primary;
  }
  return false;
}

[[nodiscard]] constexpr auto all_builtin_specs() -> std::span<const BuiltinSpec> { return kBuiltinSpecs; }

[[nodiscard]] constexpr auto all_callable_builtin_specs() -> std::span<const BuiltinSpec> {
  return {kBuiltinSpecs.data(), callable_builtin_count()};
}

[[nodiscard]] constexpr auto all_constant_builtin_specs() -> std::span<const ConstantBuiltinSpec> {
  return kConstantBuiltinSpecs;
}

}  // namespace fleaux::vm
