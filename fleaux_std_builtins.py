import itertools
import math
import os
import shutil
import tempfile
import typing
from pathlib import Path
from typing import TypeVar
from rich.traceback import install
import sys


install(show_locals=True)

T = TypeVar('T')


def decompose_call(func: typing.Callable, tuple_args: tuple):
    if not isinstance(tuple_args, tuple):
        tuple_args = (tuple_args,)
    return func(*tuple_args)


class GetArgs:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> tuple:
        # Called as () -> Std.GetArgs; tuple_args will be () (empty tuple).
        return tuple(sys.argv)


class Wrap:
    def __init__(self):
        pass

    def __ror__(self, arg: T) -> tuple[T]:
        return (arg, )


class Unwrap:
    def __init__(self):
        pass

    def __ror__(self, tuple_arg: tuple[T]) -> tuple[T]:
        return tuple_arg[0]


class ElementAt:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[tuple[T, ...], int]) -> T:
        def extract(seq: tuple[typing.Any], idx: int):
            return seq[idx]

        return decompose_call(extract, tuple_args)


class ToNum:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str]) -> float:
        def parse_number(data: str) -> float:
            num = float(data)
            if num.is_integer():
                return int(num)
            return num

        return decompose_call(parse_number, tuple_args)



class Input:
    """Read a single line from stdin and return it as String.

    Usage:
      () -> Std.Input
      ("prompt: ") -> Std.Input
    """

    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple | str) -> str:
        if tuple_args == ():
            return input()
        if isinstance(tuple_args, tuple):
            if len(tuple_args) == 1:
                return input(str(tuple_args[0]))
            raise TypeError(f"Input expects 0 or 1 argument, got {len(tuple_args)}")
        # scalar piped value counts as one prompt argument
        return input(str(tuple_args))


class Exit:
    """Terminate execution by raising SystemExit.

    Usage:
      () -> Std.Exit          # exit code 0
      (1) -> Std.Exit         # exit code 1
      1 -> Std.Exit           # scalar form
    """

    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple | int | float) -> typing.NoReturn:
        if tuple_args == ():
            raise SystemExit(0)
        if isinstance(tuple_args, tuple):
            if len(tuple_args) == 1:
                code = tuple_args[0]
                raise SystemExit(int(code))
            raise TypeError(f"Exit expects 0 or 1 argument, got {len(tuple_args)}")
        raise SystemExit(int(tuple_args))


class Take:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[tuple[T, ...], int]) -> tuple[T, ...] | None:
        def take(seq: tuple[T, ...], stop: int) -> tuple[T, ...] | None:
            return seq[:stop]

        return decompose_call(take, tuple_args)


class Drop:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[tuple[T, ...], int]) -> tuple[T, ...]:
        def drop(seq: tuple[T, ...], start: int) -> tuple[T, ...]:
            return seq[start:]

        return decompose_call(drop, tuple_args)


class Length:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> int:
        def length(seq):
            return len(seq)

        return decompose_call(length, tuple_args)


class Slice:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[tuple[T, ...], int] |
                                  tuple[tuple[T, ...], int, int] |
                                  tuple[tuple[T, ...], int, int, int]
                ) -> tuple[T, ...] | None:
        if len(tuple_args) == 2:
            def _slice_stop(iterable, stop):
                return iterable[:stop]

            return decompose_call(_slice_stop, tuple_args)

        if len(tuple_args) == 3:
            def _slice_start_stop(iterable, start, stop):
                return iterable[start: stop:]

            return decompose_call(_slice_start_stop, tuple_args)

        if len(tuple_args) == 4:
            def _slice_start_stop_step(iterable, start, stop, step):
                return iterable[start: stop: step]

            return decompose_call(_slice_start_stop_step, tuple_args)


class Pow:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float, float]) -> float:
        return decompose_call(math.pow, tuple_args)


class Subtract:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float, float]) -> float:
        def subtract(lhs, rhs):
            return lhs - rhs

        return decompose_call(subtract, tuple_args)


class Multiply:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float, float]) -> float:
        def multiply(lhs, rhs):
            return lhs * rhs

        return decompose_call(multiply, tuple_args)


class Divide:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float, float]) -> float:
        def divide(lhs, rhs):
            return lhs / rhs

        return decompose_call(divide, tuple_args)


class Add:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float, float]) -> float:
        def add(lhs, rhs):
            return rhs + lhs

        return decompose_call(add, tuple_args)


class Sqrt:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float]) -> float:
        return decompose_call(math.sqrt, tuple_args)


class Println:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[T, ...]) -> tuple[T, ...] | T:
        if not isinstance(tuple_args, tuple):
            print(tuple_args)
            return tuple_args
        decompose_call(print, tuple_args)
        if len(tuple_args) == 1:
            return tuple_args[0]
        return tuple_args


class Printf:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, typing.Any, ...]) -> tuple[str, typing.Any, ...]:
        def printf(fmt_str: str, *args_inner):
            print(fmt_str.format(args_inner))
            return fmt_str, args_inner

        return decompose_call(printf, tuple_args)


class Tan:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float]) -> float:
        return decompose_call(math.tan, tuple_args)


class Cos:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float]) -> float:
        return decompose_call(math.cos, tuple_args)


class Sin:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float]) -> float:
        return decompose_call(math.sin, tuple_args)


class Mod:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float, float]) -> float:
        def mod(lhs, rhs):
            return lhs % rhs

        return decompose_call(mod, tuple_args)


class GreaterThan:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any, typing.Any]) -> bool:
        def gt(lhs, rhs):
            return lhs > rhs

        return decompose_call(gt, tuple_args)


class LessThan:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any, typing.Any]) -> bool:
        def lt(lhs, rhs):
            return lhs < rhs

        return decompose_call(lt, tuple_args)


class GreaterOrEqual:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any, typing.Any]) -> bool:
        def ge(lhs, rhs):
            return lhs >= rhs

        return decompose_call(ge, tuple_args)


class LessOrEqual:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any, typing.Any]) -> bool:
        def le(lhs, rhs):
            return lhs <= rhs

        return decompose_call(le, tuple_args)


class Equal:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any, typing.Any]) -> bool:
        def eq(lhs, rhs):
            return lhs == rhs

        return decompose_call(eq, tuple_args)


class NotEqual:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any, typing.Any]) -> bool:
        def ne(lhs, rhs):
            return lhs != rhs

        return decompose_call(ne, tuple_args)


class Not:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any]) -> bool:
        def logical_not(val):
            return not val

        return decompose_call(logical_not, tuple_args)


class And:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any, typing.Any]) -> bool:
        def logical_and(lhs, rhs):
            return lhs and rhs

        return decompose_call(logical_and, tuple_args)


class Or:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any, typing.Any]) -> bool:
        def logical_or(lhs, rhs):
            return lhs or rhs

        return decompose_call(logical_or, tuple_args)


class Select:
    """(condition, true_val, false_val) -> Std.Select  =>  true_val if condition else false_val

    Both branches are evaluated eagerly before Select is called.
    Use Std.Branch when only one branch should execute.
    """

    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> typing.Any:
        if not isinstance(tuple_args, tuple) or len(tuple_args) != 3:
            raise TypeError(
                f"Select expects a 3-tuple (condition, true_val, false_val), got {tuple_args!r}"
            )
        condition, true_val, false_val = tuple_args
        return true_val if condition else false_val


def _apply_node(node_ref: typing.Any, value: typing.Any) -> typing.Any:
    """Pipe value into a function reference (class or already-instantiated node)."""
    instance = node_ref() if isinstance(node_ref, type) else node_ref
    return value | instance


class Apply:
    """(value, Func) -> Std.Apply  =>  value -> Func

    Applies a function reference held as a data value.
    Func may be any make_node class or builtin class/instance.
    """

    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> typing.Any:
        if not isinstance(tuple_args, tuple) or len(tuple_args) != 2:
            raise TypeError(
                f"Apply expects a 2-tuple (value, Func), got {tuple_args!r}"
            )
        value, func = tuple_args
        return _apply_node(func, value)


class Branch:
    """(condition, value, TrueFunc, FalseFunc) -> Std.Branch

    If condition is truthy, applies TrueFunc to value.
    If condition is falsy,  applies FalseFunc to value.

    Only the chosen function is called - unlike Select, the other branch
    does not execute at all.

    TrueFunc and FalseFunc are function references (make_node classes or
    builtin classes/instances).
    """

    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> typing.Any:
        if not isinstance(tuple_args, tuple) or len(tuple_args) != 4:
            raise TypeError(
                f"Branch expects a 4-tuple (condition, value, TrueFunc, FalseFunc), "
                f"got {tuple_args!r}"
            )
        condition, value, true_func, false_func = tuple_args
        return _apply_node(true_func if condition else false_func, value)


class Loop:
    """(state, continue_func, step_func) -> Std.Loop

    While continue_func(state) is truthy, computes state = step_func(state).
    Returns the final state when the condition becomes falsy.

    continue_func and step_func are function references (make_node classes or
    builtin classes/instances).
    """

    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> typing.Any:
        if not isinstance(tuple_args, tuple) or len(tuple_args) != 3:
            raise TypeError(
                f"Loop expects a 3-tuple (state, continue_func, step_func), got {tuple_args!r}"
            )

        state, continue_func, step_func = tuple_args
        while _apply_node(continue_func, state):
            state = _apply_node(step_func, state)
        return state


class LoopN:
    """(state, continue_func, step_func, max_iters) -> Std.LoopN

    Same behavior as Std.Loop, but raises RuntimeError if max_iters
    would be exceeded before termination.
    """

    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> typing.Any:
        if not isinstance(tuple_args, tuple) or len(tuple_args) != 4:
            raise TypeError(
                "LoopN expects a 4-tuple "
                "(state, continue_func, step_func, max_iters), "
                f"got {tuple_args!r}"
            )

        state, continue_func, step_func, max_iters = tuple_args
        if not isinstance(max_iters, int):
            raise TypeError(f"LoopN max_iters must be int, got {type(max_iters).__name__}")
        if max_iters < 0:
            raise ValueError("LoopN max_iters must be >= 0")

        steps = 0
        while _apply_node(continue_func, state):
            if steps >= max_iters:
                raise RuntimeError(
                    f"LoopN exceeded max_iters={max_iters} with state={state!r}"
                )
            state = _apply_node(step_func, state)
            steps += 1
        return state


class UnaryPlus:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any]) -> typing.Any:
        def unary_plus(val):
            return +val

        return decompose_call(unary_plus, tuple_args)


class UnaryMinus:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any]) -> typing.Any:
        def unary_minus(val):
            return -val

        return decompose_call(unary_minus, tuple_args)


class ToString:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[typing.Any]) -> str:
        def to_string(val):
            return str(val)

        return decompose_call(to_string, tuple_args)


class Cwd:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> str:
        if tuple_args != ():
            raise TypeError("Cwd expects no arguments")
        return os.getcwd()


class OSEnv:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str | None:
        def get_env(name: str) -> str | None:
            return os.environ.get(str(name))

        return decompose_call(get_env, tuple_args)


class OSHasEnv:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> bool:
        def has_env(name: str) -> bool:
            return str(name) in os.environ

        return decompose_call(has_env, tuple_args)


class OSSetEnv:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str]) -> str:
        def set_env(name: str, value: str) -> str:
            os.environ[str(name)] = str(value)
            return str(value)

        return decompose_call(set_env, tuple_args)


class OSUnsetEnv:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> bool:
        def unset_env(name: str) -> bool:
            key = str(name)
            existed = key in os.environ
            os.environ.pop(key, None)
            return existed

        return decompose_call(unset_env, tuple_args)


class OSIsWindows:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> bool:
        if tuple_args != ():
            raise TypeError("OS.IsWindows expects no arguments")
        return os.name == "nt"


class OSIsLinux:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> bool:
        if tuple_args != ():
            raise TypeError("OS.IsLinux expects no arguments")
        return sys.platform.startswith("linux")


class OSIsMacOS:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> bool:
        if tuple_args != ():
            raise TypeError("OS.IsMacOS expects no arguments")
        return sys.platform == "darwin"


class OSHome:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> str:
        if tuple_args != ():
            raise TypeError("OS.Home expects no arguments")
        try:
            return str(Path.home())
        except Exception:
            return "."


class OSTempDir:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> str:
        if tuple_args != ():
            raise TypeError("OS.TempDir expects no arguments")
        return tempfile.gettempdir()


class OSMakeTempFile:
    """Create a unique, empty temporary file and return its path.

    The file is guaranteed to exist and be writable at the returned path.
    Usage:  () -> Std.OS.MakeTempFile
    """

    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> str:
        if tuple_args != ():
            raise TypeError("OS.MakeTempFile expects no arguments")
        fd, path = tempfile.mkstemp()
        os.close(fd)
        return path


class OSMakeTempDir:
    """Create a unique temporary directory and return its path.

    The directory is guaranteed to exist and be writable at the returned path.
    Usage:  () -> Std.OS.MakeTempDir
    """

    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> str:
        if tuple_args != ():
            raise TypeError("OS.MakeTempDir expects no arguments")
        return tempfile.mkdtemp()


class StringUpper:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        return decompose_call(lambda s: str(s).upper(), tuple_args)


class StringLower:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        return decompose_call(lambda s: str(s).lower(), tuple_args)


class StringTrim:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        return decompose_call(lambda s: str(s).strip(), tuple_args)


class StringTrimStart:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        return decompose_call(lambda s: str(s).lstrip(), tuple_args)


class StringTrimEnd:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        return decompose_call(lambda s: str(s).rstrip(), tuple_args)


class StringSplit:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str]) -> tuple:
        def split(s: str, sep: str) -> tuple:
            return tuple(str(s).split(str(sep)))

        return decompose_call(split, tuple_args)


class StringJoin:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, tuple]) -> str:
        def join(sep: str, parts: tuple) -> str:
            return str(sep).join(str(p) for p in parts)

        return decompose_call(join, tuple_args)


class StringReplace:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str, str]) -> str:
        def replace(s: str, old: str, new: str) -> str:
            return str(s).replace(str(old), str(new))

        return decompose_call(replace, tuple_args)


class StringContains:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str]) -> bool:
        def contains(s: str, sub: str) -> bool:
            return str(sub) in str(s)

        return decompose_call(contains, tuple_args)


class StringStartsWith:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str]) -> bool:
        def starts_with(s: str, prefix: str) -> bool:
            return str(s).startswith(str(prefix))

        return decompose_call(starts_with, tuple_args)


class StringEndsWith:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str]) -> bool:
        def ends_with(s: str, suffix: str) -> bool:
            return str(s).endswith(str(suffix))

        return decompose_call(ends_with, tuple_args)


class StringLength:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> int:
        return decompose_call(lambda s: len(str(s)), tuple_args)


class MathFloor:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float] | float) -> int:
        def floor(x: float) -> int:
            return math.floor(x)

        return decompose_call(floor, tuple_args)


class MathCeil:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float] | float) -> int:
        def ceil(x: float) -> int:
            return math.ceil(x)

        return decompose_call(ceil, tuple_args)


class MathAbs:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float] | float) -> float:
        def absolute(x: float) -> float:
            return abs(x)

        return decompose_call(absolute, tuple_args)


class MathLog:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float] | float) -> float:
        def log(x: float) -> float:
            return math.log(x)

        return decompose_call(log, tuple_args)


class MathClamp:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[float, float, float]) -> float:
        def clamp(x: float, lo: float, hi: float) -> float:
            return max(lo, min(x, hi))

        return decompose_call(clamp, tuple_args)


class PathJoin:
    def __init__(self):
        pass

    def __ror__(self, tuple_args) -> str:
        if not isinstance(tuple_args, tuple):
            raise TypeError("PathJoin expects a tuple of at least 2 path segments")
        if len(tuple_args) < 2:
            raise ValueError("PathJoin expects at least 2 path segments")
        head, *tail = tuple_args
        return str(Path(str(head)).joinpath(*(str(s) for s in tail)))


class PathNormalize:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        def normalize(path: str) -> str:
            return os.path.normpath(str(path))

        return decompose_call(normalize, tuple_args)


class PathBasename:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        def basename(path: str) -> str:
            return os.path.basename(str(path))

        return decompose_call(basename, tuple_args)


class PathDirname:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        def dirname(path: str) -> str:
            return os.path.dirname(str(path))

        return decompose_call(dirname, tuple_args)


class PathExists:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> bool:
        def exists(path: str) -> bool:
            return os.path.exists(str(path))

        return decompose_call(exists, tuple_args)


class PathIsFile:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> bool:
        def is_file(path: str) -> bool:
            return os.path.isfile(str(path))

        return decompose_call(is_file, tuple_args)


class PathIsDir:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> bool:
        def is_dir(path: str) -> bool:
            return os.path.isdir(str(path))

        return decompose_call(is_dir, tuple_args)


class PathAbsolute:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        def absolute(path: str) -> str:
            return os.path.abspath(str(path))

        return decompose_call(absolute, tuple_args)


class FileReadText:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        def read_text(path: str) -> str:
            with open(str(path), "r", encoding="utf-8") as f:
                return f.read()

        return decompose_call(read_text, tuple_args)


class FileWriteText:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str]) -> str:
        def write_text(path: str, text: str) -> str:
            with open(str(path), "w", encoding="utf-8") as f:
                f.write(str(text))
            return str(path)

        return decompose_call(write_text, tuple_args)


class FileAppendText:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str]) -> str:
        def append_text(path: str, text: str) -> str:
            with open(str(path), "a", encoding="utf-8") as f:
                f.write(str(text))
            return str(path)

        return decompose_call(append_text, tuple_args)


class FileReadLines:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> tuple:
        def read_lines(path: str) -> tuple:
            with open(str(path), "r", encoding="utf-8") as f:
                return tuple(f.read().splitlines())

        return decompose_call(read_lines, tuple_args)


class FileDelete:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> bool:
        def delete(path: str) -> bool:
            try:
                os.remove(str(path))
                return True
            except FileNotFoundError:
                return False

        return decompose_call(delete, tuple_args)


class FileSize:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> int:
        def size(path: str) -> int:
            return os.path.getsize(str(path))

        return decompose_call(size, tuple_args)


class PathExtension:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        return decompose_call(lambda p: Path(str(p)).suffix, tuple_args)


class PathStem:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        return decompose_call(lambda p: Path(str(p)).stem, tuple_args)


class PathWithExtension:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str]) -> str:
        def with_ext(path: str, ext: str) -> str:
            ext = str(ext)
            if ext and not ext.startswith("."):
                ext = "." + ext
            return str(Path(str(path)).with_suffix(ext))

        return decompose_call(with_ext, tuple_args)


class PathWithBasename:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str, str]) -> str:
        def with_basename(path: str, name: str) -> str:
            return str(Path(str(path)).with_name(str(name)))

        return decompose_call(with_basename, tuple_args)


class DirCreate:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> str:
        def create(path: str) -> str:
            os.makedirs(str(path), exist_ok=True)
            return str(path)

        return decompose_call(create, tuple_args)


class DirDelete:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> bool:
        def delete(path: str) -> bool:
            try:
                shutil.rmtree(str(path))
                return True
            except FileNotFoundError:
                return False

        return decompose_call(delete, tuple_args)


class DirList:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> tuple:
        def list_dir(path: str) -> tuple:
            return tuple(os.listdir(str(path)))

        return decompose_call(list_dir, tuple_args)


class DirListFull:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[str] | str) -> tuple:
        def list_full(path: str) -> tuple:
            p = str(path)
            return tuple(os.path.join(p, name) for name in os.listdir(p))

        return decompose_call(list_full, tuple_args)


class TupleAppend:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> tuple:
        def append(t: tuple, item: typing.Any) -> tuple:
            return t + (item,)

        return decompose_call(append, tuple_args)


class TuplePrepend:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> tuple:
        def prepend(t: tuple, item: typing.Any) -> tuple:
            return (item,) + t

        return decompose_call(prepend, tuple_args)


class TupleReverse:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> tuple:
        return decompose_call(lambda t: t[::-1], tuple_args)


class TupleContains:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> bool:
        def contains(t: tuple, item: typing.Any) -> bool:
            return item in t

        return decompose_call(contains, tuple_args)


class TupleZip:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> tuple:
        def zip_tuples(a: tuple, b: tuple) -> tuple:
            return tuple(zip(a, b))

        return decompose_call(zip_tuples, tuple_args)


class TupleMap:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> tuple:
        def map_tuple(t: tuple, func: typing.Any) -> tuple:
            return tuple(_apply_node(func, item) for item in t)

        return decompose_call(map_tuple, tuple_args)


class TupleFilter:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple) -> tuple:
        def filter_tuple(t: tuple, pred: typing.Any) -> tuple:
            return tuple(item for item in t if _apply_node(pred, item))

        return decompose_call(filter_tuple, tuple_args)


def make_node(func):
    class Node:
        def __init__(self):
            self.func = func

        def __ror__(self, tuple_args: tuple) -> typing.Any:
            return decompose_call(self.func, tuple_args)

    return Node

# ── Streaming file handle builtins ────────────────────────────────────────────

import threading

_handle_lock = threading.Lock()

class _HandleEntry:
    def __init__(self, fp, path: str, mode: str, generation: int):
        self.fp = fp
        self.path = path
        self.mode = mode
        self.generation = generation
        self.closed = False

_handle_slots: list[_HandleEntry | None] = []
_HANDLE_TAG = "__fleaux_handle__"


def _make_handle_token(slot: int, gen: int) -> tuple:
    return (_HANDLE_TAG, slot, gen)


def _resolve_handle(token, op: str) -> _HandleEntry:
    # The transpiler wraps single expressions in a 1-tuple: (h) -> ReadLine
    # becomes (h,) | ReadLine().  Unwrap one level if needed.
    if isinstance(token, tuple) and len(token) == 1:
        token = token[0]
    if not isinstance(token, tuple) or len(token) != 3 or token[0] != _HANDLE_TAG:
        raise RuntimeError(f"{op}: not a valid handle token")
    _, slot, gen = token
    with _handle_lock:
        if slot >= len(_handle_slots):
            raise RuntimeError(f"{op}: handle is closed or invalid")
        entry = _handle_slots[slot]
        if entry is None or entry.closed or entry.generation != gen:
            raise RuntimeError(f"{op}: handle is closed or invalid")
        return entry


def _open_handle(path: str, mode: str) -> tuple:
    fp = open(str(path), mode, encoding=None if 'b' in mode else 'utf-8')
    with _handle_lock:
        for i, slot in enumerate(_handle_slots):
            if slot is None or slot.closed:
                gen = (slot.generation + 1) if slot is not None else 0
                entry = _HandleEntry(fp, path, mode, gen)
                _handle_slots[i] = entry
                return _make_handle_token(i, gen)
        slot = len(_handle_slots)
        entry = _HandleEntry(fp, path, mode, 0)
        _handle_slots.append(entry)
        return _make_handle_token(slot, 0)


def _close_handle(token) -> bool:
    if not isinstance(token, tuple) or len(token) != 3 or token[0] != _HANDLE_TAG:
        return False
    _, slot, gen = token
    with _handle_lock:
        if slot >= len(_handle_slots):
            return False
        entry = _handle_slots[slot]
        if entry is None or entry.closed or entry.generation != gen:
            return False
        entry.fp.close()
        entry.closed = True
        return True


class FileOpen:
    def __init__(self): pass
    def __ror__(self, tuple_args) -> tuple:
        if isinstance(tuple_args, tuple):
            if len(tuple_args) == 2:
                path, mode = tuple_args
            elif len(tuple_args) == 1:
                path, mode = tuple_args[0], 'r'
            else:
                raise RuntimeError("File.Open: expected (path,) or (path, mode)")
        else:
            path, mode = tuple_args, 'r'
        return _open_handle(str(path), mode)


class FileReadLine:
    def __init__(self): pass
    def __ror__(self, token) -> tuple:
        e = _resolve_handle(token, "File.ReadLine")
        line = e.fp.readline()
        eof = (line == '')
        return (token, line.rstrip('\n') if not eof else '', eof)


class FileReadChunk:
    def __init__(self): pass
    def __ror__(self, tuple_args) -> tuple:
        token, nbytes = tuple_args
        e = _resolve_handle(token, "File.ReadChunk")
        chunk = e.fp.read(nbytes)
        eof = (chunk == '' or chunk == b'')
        return (token, chunk, eof)


class FileWriteChunk:
    def __init__(self): pass
    def __ror__(self, tuple_args) -> tuple:
        token, data = tuple_args
        e = _resolve_handle(token, "File.WriteChunk")
        e.fp.write(data)
        return token


class FileFlush:
    def __init__(self): pass
    def __ror__(self, token) -> tuple:
        e = _resolve_handle(token, "File.Flush")
        e.fp.flush()
        return token


class FileClose:
    def __init__(self): pass
    def __ror__(self, token) -> bool:
        return _close_handle(token)


class FileWithOpen:
    def __init__(self): pass
    def __ror__(self, tuple_args) -> typing.Any:
        if not isinstance(tuple_args, tuple) or len(tuple_args) != 3:
            raise TypeError(f"File.WithOpen expects (path, mode, func), got {tuple_args!r}")
        path, mode, func = tuple_args
        token = _open_handle(str(path), mode)
        try:
            # Handle tokens are tuples; wrap once so make_node callables receive
            # a single `h` parameter instead of token fields as multiple args.
            result = _apply_node(func, (token,))
        finally:
            _close_handle(token)
        return result



