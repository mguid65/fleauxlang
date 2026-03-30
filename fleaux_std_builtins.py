import itertools
import math
import typing
from typing import TypeVar
from rich.traceback import install
import sys


install(show_locals=True)

T = TypeVar('T')


def decompose_call(func: typing.Callable, tuple_args: tuple):
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


class In:
    def __init__(self):
        pass

    def __ror__(self, tuple_args: tuple[int]) -> tuple[str, ...] | tuple[str]:
        def get_input(count: int):
            if not count > 0:
                raise ValueError(f"{count} is not a valid input for In operation")
            if count == 1:
                return tuple([input()])
            return tuple([input() for _ in range(count)])

        return decompose_call(get_input, tuple_args)


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


def make_node(func):
    class Node:
        def __init__(self):
            self.func = func

        def __ror__(self, tuple_args: tuple) -> typing.Any:
            return decompose_call(self.func, tuple_args)

    return Node
