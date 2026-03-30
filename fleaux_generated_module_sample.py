import fleaux_std_builtins as fstd

def _fleaux_missing_builtin(name):
    class _MissingBuiltin:
        def __ror__(self, _tuple_args):
            raise NotImplementedError(
                f"Builtin '{name}' is not yet implemented in fleaux_std_builtins.py"
            )
    return _MissingBuiltin

import fleaux_generated_module_Std as _mod_Std

def _fleaux_impl_Square(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((x, x) | _mod_Std.Std_Multiply())
Square = fstd.make_node(_fleaux_impl_Square)

def _fleaux_impl_Cube(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((x, x) | _mod_Std.Std_Multiply()), x) | _mod_Std.Std_Multiply())
Cube = fstd.make_node(_fleaux_impl_Cube)

def _fleaux_impl_Hypotenuse(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    return (((((a, a) | _mod_Std.Std_Multiply()), ((b, b) | _mod_Std.Std_Multiply())) | _mod_Std.Std_Add()) | _mod_Std.Std_Sqrt())
Hypotenuse = fstd.make_node(_fleaux_impl_Hypotenuse)

def _fleaux_impl_Sum3(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    c = _fleaux_args[2]
    return ((((a, b) | _mod_Std.Std_Add()), c) | _mod_Std.Std_Add())
Sum3 = fstd.make_node(_fleaux_impl_Sum3)

def _fleaux_impl_Mean3(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    c = _fleaux_args[2]
    return ((((a, b, c) | Sum3()), 3) | _mod_Std.Std_Divide())
Mean3 = fstd.make_node(_fleaux_impl_Mean3)

def _fleaux_impl_Poly(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((2, ((x, x) | _mod_Std.Std_Multiply())) | _mod_Std.Std_Multiply()), ((3, x) | _mod_Std.Std_Multiply()), 1) | Sum3())
Poly = fstd.make_node(_fleaux_impl_Poly)

def _fleaux_impl_Abs(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((x, 0) | _mod_Std.Std_GreaterOrEqual()), x, ((0, x) | _mod_Std.Std_Subtract())) | _mod_Std.Std_Select())
Abs = fstd.make_node(_fleaux_impl_Abs)

def _fleaux_impl_Max(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    return ((((a, b) | _mod_Std.Std_GreaterOrEqual()), a, b) | _mod_Std.Std_Select())
Max = fstd.make_node(_fleaux_impl_Max)

def _fleaux_impl_Min(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    return ((((a, b) | _mod_Std.Std_LessOrEqual()), a, b) | _mod_Std.Std_Select())
Min = fstd.make_node(_fleaux_impl_Min)

def _fleaux_impl_Clamp(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    lo = _fleaux_args[1]
    hi = _fleaux_args[2]
    return ((((x, lo) | Max()), hi) | Min())
Clamp = fstd.make_node(_fleaux_impl_Clamp)

_fleaux_last_value = (((4,) | Square()) | _mod_Std.Std_Println())
_fleaux_last_value = (((3,) | Cube()) | _mod_Std.Std_Println())
_fleaux_last_value = (((3, 4) | Hypotenuse()) | _mod_Std.Std_Println())
_fleaux_last_value = (((10, 20, 30) | Sum3()) | _mod_Std.Std_Println())
_fleaux_last_value = (((10, 20, 30) | Mean3()) | _mod_Std.Std_Println())
_fleaux_last_value = (((5,) | Poly()) | _mod_Std.Std_Println())
_fleaux_last_value = (((-7,) | Abs()) | _mod_Std.Std_Println())
_fleaux_last_value = (((3,) | Abs()) | _mod_Std.Std_Println())
_fleaux_last_value = (((4, 9) | Max()) | _mod_Std.Std_Println())
_fleaux_last_value = (((4, 9) | Min()) | _mod_Std.Std_Println())
_fleaux_last_value = (((15, 0, 10) | Clamp()) | _mod_Std.Std_Println())
_fleaux_last_value = (((-5, 0, 10) | Clamp()) | _mod_Std.Std_Println())
_fleaux_last_value = (((7, 0, 10) | Clamp()) | _mod_Std.Std_Println())
