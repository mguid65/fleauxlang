import fleaux_std_builtins as fstd

def _fleaux_missing_builtin(name):
    class _MissingBuiltin:
        def __ror__(self, _tuple_args):
            raise NotImplementedError(
                f"Builtin '{name}' is not yet implemented in fleaux_std_builtins.py"
            )
    return _MissingBuiltin

import fleaux_generated_module_Std as _mod_Std

def _fleaux_impl_Add2(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((x, 2) | _mod_Std.Std_Add())
Add2 = fstd.make_node(_fleaux_impl_Add2)

def _fleaux_impl_IsGreaterThan5(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((x, 5) | _mod_Std.Std_GreaterThan())
IsGreaterThan5 = fstd.make_node(_fleaux_impl_IsGreaterThan5)

def _fleaux_impl_Multiply(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_Multiply())
Multiply = fstd.make_node(_fleaux_impl_Multiply)

_fleaux_last_value = ((3,) | Add2())
_fleaux_last_value = ((7,) | IsGreaterThan5())
_fleaux_last_value = ((2, 5) | Multiply())
