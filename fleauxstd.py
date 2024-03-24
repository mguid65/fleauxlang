import itertools
import math
import typing
from typing import overload
from collections.abc import Sequence


def decompose_call(func: typing.Callable, args: typing.Any):
    if isinstance(args, tuple) or isinstance(args, list):
        return func(*args)
    return func(args)


class Extract:
    def __init__(self):
        pass

    def __ror__(self, args: Sequence[Sequence[typing.Any], int]) -> typing.Any:
        def extract(seq: Sequence[typing.Any], idx: int):
            return seq[idx]
        return extract(*args)


class ToNum:
    def __init__(self):
        pass

    def __ror__(self, args: Sequence[str] | str) -> float:
        def parse_number(data: str) -> float:
            num = float(data)
            if num.is_integer():
                return int(num)
            return num
        return decompose_call(parse_number, args)


class In:
    def __init__(self):
        pass

    def __ror__(self, args: Sequence[int] | int):
        def get_input(count: int):
            return tuple([input() for _ in range(count)])
        return decompose_call(get_input, args)


class Slice:
    def __init__(self):
        pass

    def __ror__(self, args: Sequence[Sequence, int] |
                            Sequence[Sequence, int, int] |
                            Sequence[Sequence, int, int, int]
                ) -> Sequence | None:
        if len(args) == 2:
            def _slice_stop(iterable, stop):
                return iterable[:stop]

            return decompose_call(_slice_stop, args)

        if len(args) == 3:
            def _slice_start_stop(iterable, start, stop):
                return iterable[start: stop:]

            return decompose_call(_slice_start_stop, args)

        if len(args) == 4:
            def _slice_start_stop_step(iterable, start, stop, step):
                return iterable[start: stop: step]

            return decompose_call(_slice_start_stop_step, args)


class Pow:
    def __init__(self):
        pass

    def __ror__(self, args: tuple[float, float] | list[float, float]) -> float:
        return math.pow(*args)


class Subtract:
    def __init__(self):
        pass

    def __ror__(self, args: tuple[float, float] | list[float, float]) -> float:
        def subtract(lhs, rhs):
            return lhs - rhs

        return subtract(*args)


class Multiply:
    def __init__(self):
        pass

    def __ror__(self, args: tuple[float, float] | list[float, float]) -> float:
        def multiply(lhs, rhs):
            return lhs * rhs

        return multiply(*args)


class Divide:
    def __init__(self):
        pass

    def __ror__(self, args: tuple[float, float] | list[float, float]) -> float:
        def divide(lhs, rhs):
            return lhs / rhs

        return divide(*args)


class Add:
    def __init__(self):
        pass

    def __ror__(self, args: tuple[float, float] | list[float, float]) -> float:
        def add(lhs, rhs):
            return rhs + lhs

        return add(*args)


class Sqrt:
    def __init__(self):
        pass

    def __ror__(self, args: tuple[float] | list[float] | float) -> float:
        return decompose_call(math.sqrt, args)


class Println:
    def __init__(self):
        pass

    def __ror__(self, args: typing.Any) -> typing.Any:
        decompose_call(print, args)
        return args


class Printf:
    def __init__(self):
        pass

    def __ror__(self, args: tuple[str, typing.Any] | list[str, typing.Any] | str) -> typing.Any:
        def printf(fmt_str: str, *args_inner):
            print(fmt_str.format(args_inner))

        decompose_call(printf, args)
        return args


class Tan:
    def __init__(self):
        pass

    def __ror__(self, args: tuple[float] | float) -> float:
        return decompose_call(math.tan, args)


class Cos:
    def __init__(self):
        pass

    def __ror__(self, args) -> float:
        return decompose_call(math.cos, args)


class Sin:
    def __init__(self):
        pass

    def __ror__(self, args) -> float:
        return decompose_call(math.sin, args)


def make_node(func):
    class Node:
        def __init__(self):
            self.func = func

        def __ror__(self, args):
            return decompose_call(self.func, args)

    return Node
