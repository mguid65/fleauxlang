import itertools
import math
import typing
from typing import overload, TypeVar

T = TypeVar('T')


def decompose_call(func: typing.Callable, tuple_args: tuple):
    return func(*tuple_args)


class Extract:
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

    def __ror__(self, tuple_args: tuple[int]) -> tuple[str, ...] | str | None:
        def get_input(count: int):
            if count == 0:
                return None
            if count == 1:
                return input()
            return tuple([input() for _ in range(count)])

        return decompose_call(get_input, tuple_args)


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


def make_node(func):
    class Node:
        def __init__(self):
            self.func = func

        def __ror__(self, tuple_args: tuple) -> typing.Any:
            return decompose_call(self.func, tuple_args)

    return Node
