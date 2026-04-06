// Internal header: shared stdlib-builtin dispatch map for the bytecode runtime.
// Include this after fleaux_runtime.hpp is already in scope.
//
// Defines vm_builtin_callables(), an inline function returning a
// map<string, RuntimeCallable> covering all builtins exposed by the
// FLEAUX_VM_BUILTINS X-macro.
#pragma once

#include <string>
#include <unordered_map>

#include "fleaux_runtime.hpp"

// ── X-macro list of (name_literal, struct_name) ──────────────────────────────
// Keep in sync with interpreter.cpp's FLEAUX_VM_BUILTINS.

#define FLEAUX_VM_BUILTINS(X)                    \
  X("Std.Printf", Printf)                        \
  X("Std.Println", Println)                      \
  X("Std.GetArgs", GetArgs)                      \
  X("Std.ToString", ToString)                    \
  X("Std.ToNum", ToNum)                          \
  X("Std.Input", Input)                          \
  X("Std.Exit", Exit)                            \
  X("Std.Select", Select)                        \
  X("Std.Apply", Apply)                          \
  X("Std.Branch", Branch)                        \
  X("Std.Loop", Loop)                            \
  X("Std.LoopN", LoopN)                          \
  X("Std.Wrap", Wrap)                            \
  X("Std.Unwrap", Unwrap)                        \
  X("Std.UnaryPlus", UnaryPlus)                  \
  X("Std.UnaryMinus", UnaryMinus)                \
  X("Std.Add", Add)                              \
  X("Std.Subtract", Subtract)                    \
  X("Std.Multiply", Multiply)                    \
  X("Std.Divide", Divide)                        \
  X("Std.Mod", Mod)                              \
  X("Std.Pow", Pow)                              \
  X("Std.Sqrt", Sqrt)                            \
  X("Std.Sin", Sin)                              \
  X("Std.Cos", Cos)                              \
  X("Std.Tan", Tan)                              \
  X("Std.Equal", Equal)                          \
  X("Std.NotEqual", NotEqual)                    \
  X("Std.LessThan", LessThan)                    \
  X("Std.GreaterThan", GreaterThan)              \
  X("Std.GreaterOrEqual", GreaterOrEqual)        \
  X("Std.LessOrEqual", LessOrEqual)              \
  X("Std.Not", Not)                              \
  X("Std.And", And)                              \
  X("Std.Or", Or)                                \
  X("Std.ElementAt", ElementAt)                  \
  X("Std.Length", Length)                        \
  X("Std.Take", Take)                            \
  X("Std.Drop", Drop)                            \
  X("Std.Slice", Slice)                          \
  X("Std.Math.Floor", MathFloor)                 \
  X("Std.Math.Ceil", MathCeil)                   \
  X("Std.Math.Abs", MathAbs)                     \
  X("Std.Math.Log", MathLog)                     \
  X("Std.Math.Clamp", MathClamp)                 \
  X("Std.Math.Sqrt", Sqrt)                       \
  X("Std.Math.Sin", Sin)                         \
  X("Std.Math.Cos", Cos)                         \
  X("Std.Math.Tan", Tan)                         \
  X("Std.String.Upper", StringUpper)             \
  X("Std.String.Lower", StringLower)             \
  X("Std.String.Trim", StringTrim)               \
  X("Std.String.TrimStart", StringTrimStart)     \
  X("Std.String.TrimEnd", StringTrimEnd)         \
  X("Std.String.Split", StringSplit)             \
  X("Std.String.Join", StringJoin)               \
  X("Std.String.Replace", StringReplace)         \
  X("Std.String.Contains", StringContains)       \
  X("Std.String.StartsWith", StringStartsWith)   \
  X("Std.String.EndsWith", StringEndsWith)       \
  X("Std.String.Length", StringLength)           \
  X("Std.OS.Cwd", Cwd)                           \
  X("Std.OS.Env", OSEnv)                         \
  X("Std.OS.HasEnv", OSHasEnv)                   \
  X("Std.OS.SetEnv", OSSetEnv)                   \
  X("Std.OS.UnsetEnv", OSUnsetEnv)               \
  X("Std.OS.IsWindows", OSIsWindows)             \
  X("Std.OS.IsLinux", OSIsLinux)                 \
  X("Std.OS.IsMacOS", OSIsMacOS)                 \
  X("Std.OS.Home", OSHome)                       \
  X("Std.OS.TempDir", OSTempDir)                 \
  X("Std.OS.MakeTempFile", OSMakeTempFile)       \
  X("Std.OS.MakeTempDir", OSMakeTempDir)         \
  X("Std.Path.Join", PathJoin)                   \
  X("Std.Path.Normalize", PathNormalize)         \
  X("Std.Path.Basename", PathBasename)           \
  X("Std.Path.Dirname", PathDirname)             \
  X("Std.Path.Exists", PathExists)               \
  X("Std.Path.IsFile", PathIsFile)               \
  X("Std.Path.IsDir", PathIsDir)                 \
  X("Std.Path.Absolute", PathAbsolute)           \
  X("Std.Path.Extension", PathExtension)         \
  X("Std.Path.Stem", PathStem)                   \
  X("Std.Path.WithExtension", PathWithExtension) \
  X("Std.Path.WithBasename", PathWithBasename)   \
  X("Std.File.ReadText", FileReadText)           \
  X("Std.File.WriteText", FileWriteText)         \
  X("Std.File.AppendText", FileAppendText)       \
  X("Std.File.ReadLines", FileReadLines)         \
  X("Std.File.Delete", FileDelete)               \
  X("Std.File.Size", FileSize)                   \
  X("Std.File.Open", FileOpen)                   \
  X("Std.File.ReadLine", FileReadLine)           \
  X("Std.File.ReadChunk", FileReadChunk)         \
  X("Std.File.WriteChunk", FileWriteChunk)       \
  X("Std.File.Flush", FileFlush)                 \
  X("Std.File.Close", FileClose)                 \
  X("Std.File.WithOpen", FileWithOpen)           \
  X("Std.Dir.Create", DirCreate)                 \
  X("Std.Dir.Delete", DirDelete)                 \
  X("Std.Dir.List", DirList)                     \
  X("Std.Dir.ListFull", DirListFull)             \
  X("Std.Tuple.Append", TupleAppend)             \
  X("Std.Tuple.Prepend", TuplePrepend)           \
  X("Std.Tuple.Reverse", TupleReverse)           \
  X("Std.Tuple.Contains", TupleContains)         \
  X("Std.Tuple.Zip", TupleZip)                   \
  X("Std.Tuple.Map", TupleMap)                   \
  X("Std.Tuple.Filter", TupleFilter)             \
  X("Std.Tuple.Sort", TupleSort)                 \
  X("Std.Tuple.Unique", TupleUnique)             \
  X("Std.Tuple.Min", TupleMin)                   \
  X("Std.Tuple.Max", TupleMax)                   \
  X("Std.Tuple.Reduce", TupleReduce)             \
  X("Std.Tuple.FindIndex", TupleFindIndex)       \
  X("Std.Tuple.Any", TupleAny)                   \
  X("Std.Tuple.All", TupleAll)                   \
  X("Std.Tuple.Range", TupleRange)               \
  X("Std.Dict.Create", DictCreate)               \
  X("Std.Dict.Set", DictSet)                     \
  X("Std.Dict.Get", DictGet)                     \
  X("Std.Dict.GetDefault", DictGetDefault)       \
  X("Std.Dict.Contains", DictContains)           \
  X("Std.Dict.Delete", DictDelete)               \
  X("Std.Dict.Keys", DictKeys)                   \
  X("Std.Dict.Values", DictValues)               \
  X("Std.Dict.Entries", DictEntries)             \
  X("Std.Dict.Clear", DictClear)                 \
  X("Std.Dict.Length", DictLength)

namespace fleaux::vm {

// Returns the complete stdlib builtin dispatch map.
// The map is constructed once (lazily) per program.
[[nodiscard]] inline const std::unordered_map<std::string, fleaux::runtime::RuntimeCallable>&
vm_builtin_callables() {
  using namespace fleaux::runtime;

  static const std::unordered_map<std::string, RuntimeCallable> map = [] {
    std::unordered_map<std::string, RuntimeCallable> out;

#define FLEAUX_INSERT_BUILTIN(name_literal, node_type)                 \
    out.emplace(name_literal, [](Value arg) -> Value {                 \
      return node_type{}(std::move(arg));                              \
    });
    FLEAUX_VM_BUILTINS(FLEAUX_INSERT_BUILTIN)
#undef FLEAUX_INSERT_BUILTIN

    // Numeric constants (zero-arg: ignore the argument, return the constant).
    auto constant = [](const double v) {
      return [v](Value) -> Value { return make_float(v); };
    };
    out.emplace("Std.Pi",        constant(3.141592653589793238462643383279502884));
    out.emplace("Std.TwoPi",     constant(6.283185307179586476925286766559005768));
    out.emplace("Std.Tau",       constant(6.283185307179586476925286766559005768));
    out.emplace("Std.HalfPi",    constant(1.570796326794896619231321691639751442));
    out.emplace("Std.E",         constant(2.718281828459045235360287471352662497));
    out.emplace("Std.Phi",       constant(1.618033988749894848204586834365638117));
    out.emplace("Std.RootTwo",   constant(1.414213562373095048801688724209698078));
    out.emplace("Std.RootThree", constant(1.732050807568877293527446341505872366));
    out.emplace("Std.LnTwo",     constant(0.693147180559945309417232121458176568));
    out.emplace("Std.Log2E",     constant(1.442695040888963407359924681001892137));

    return out;
  }();
  return map;
}

}  // namespace fleaux::vm

