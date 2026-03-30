import fleaux_std_builtins as fstd

def _fleaux_missing_builtin(name):
    class _MissingBuiltin:
        def __ror__(self, _tuple_args):
            raise NotImplementedError(
                f"Builtin '{name}' is not yet implemented in fleaux_std_builtins.py"
            )
    return _MissingBuiltin

import fleaux_generated_module_Std as _mod_Std

def _fleaux_impl_Double(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((x, 2) | _mod_Std.Std_Multiply())
Double = fstd.make_node(_fleaux_impl_Double)

def _fleaux_impl_Triple(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((x, 3) | _mod_Std.Std_Multiply())
Triple = fstd.make_node(_fleaux_impl_Triple)

def _fleaux_impl_Square(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((x, x) | _mod_Std.Std_Multiply())
Square = fstd.make_node(_fleaux_impl_Square)

def _fleaux_impl_AddFiveThenDouble(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((x, 5) | _mod_Std.Std_Add()), 2) | _mod_Std.Std_Multiply())
AddFiveThenDouble = fstd.make_node(_fleaux_impl_AddFiveThenDouble)

def _fleaux_impl_DoubleThenAdd10(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((x, 2) | _mod_Std.Std_Multiply()), 10) | _mod_Std.Std_Add())
DoubleThenAdd10 = fstd.make_node(_fleaux_impl_DoubleThenAdd10)

def _fleaux_impl_Add(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_Add())
Add = fstd.make_node(_fleaux_impl_Add)

def _fleaux_impl_Subtract(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_Subtract())
Subtract = fstd.make_node(_fleaux_impl_Subtract)

def _fleaux_impl_Multiply(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_Multiply())
Multiply = fstd.make_node(_fleaux_impl_Multiply)

def _fleaux_impl_Divide(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_Divide())
Divide = fstd.make_node(_fleaux_impl_Divide)

def _fleaux_impl_Average(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((((x, y) | _mod_Std.Std_Add()), 2) | _mod_Std.Std_Divide())
Average = fstd.make_node(_fleaux_impl_Average)

def _fleaux_impl_Sum3(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    c = _fleaux_args[2]
    return ((((a, b) | _mod_Std.Std_Add()), c) | _mod_Std.Std_Add())
Sum3 = fstd.make_node(_fleaux_impl_Sum3)

def _fleaux_impl_Product3(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    c = _fleaux_args[2]
    return ((((a, b) | _mod_Std.Std_Multiply()), c) | _mod_Std.Std_Multiply())
Product3 = fstd.make_node(_fleaux_impl_Product3)

def _fleaux_impl_Combine(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    c = _fleaux_args[2]
    return ((((a, b) | _mod_Std.Std_Add()), c) | _mod_Std.Std_Multiply())
Combine = fstd.make_node(_fleaux_impl_Combine)

def _fleaux_impl_FahrenheitToCelsius(*_fleaux_args):
    f = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((((f, 32) | _mod_Std.Std_Subtract()), 5) | _mod_Std.Std_Multiply()), 9) | _mod_Std.Std_Divide())
FahrenheitToCelsius = fstd.make_node(_fleaux_impl_FahrenheitToCelsius)

def _fleaux_impl_IsGreater(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_GreaterThan())
IsGreater = fstd.make_node(_fleaux_impl_IsGreater)

def _fleaux_impl_IsLess(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_LessThan())
IsLess = fstd.make_node(_fleaux_impl_IsLess)

def _fleaux_impl_AreEqual(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_Equal())
AreEqual = fstd.make_node(_fleaux_impl_AreEqual)

_fleaux_last_value = (((5,) | Double()) | _mod_Std.Std_Println())
_fleaux_last_value = (((4,) | Square()) | _mod_Std.Std_Println())
_fleaux_last_value = (((10,) | AddFiveThenDouble()) | _mod_Std.Std_Println())
_fleaux_last_value = (((7,) | DoubleThenAdd10()) | _mod_Std.Std_Println())
_fleaux_last_value = (((10, 5) | Add()) | _mod_Std.Std_Println())
_fleaux_last_value = (((20, 8) | Divide()) | _mod_Std.Std_Println())
_fleaux_last_value = (((3, 4) | Multiply()) | _mod_Std.Std_Println())
_fleaux_last_value = (((10, 20) | Average()) | _mod_Std.Std_Println())
_fleaux_last_value = (((2, 3, 4) | Sum3()) | _mod_Std.Std_Println())
_fleaux_last_value = (((2, 3, 4) | Product3()) | _mod_Std.Std_Println())
_fleaux_last_value = (((5, 3, 2) | Combine()) | _mod_Std.Std_Println())
_fleaux_last_value = (((212,) | FahrenheitToCelsius()) | _mod_Std.Std_Println())
_fleaux_last_value = (((10, 5) | IsGreater()) | _mod_Std.Std_Println())
_fleaux_last_value = (((3, 9) | IsLess()) | _mod_Std.Std_Println())
_fleaux_last_value = (((7, 7) | AreEqual()) | _mod_Std.Std_Println())
