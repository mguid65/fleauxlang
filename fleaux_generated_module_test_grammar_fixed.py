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

def _fleaux_impl_AddThenMultiply(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    z = _fleaux_args[2]
    return (((x, y) | _mod_Std.Std_Add()) | _mod_Std.Std_Multiply())
AddThenMultiply = fstd.make_node(_fleaux_impl_AddThenMultiply)

def _fleaux_impl_MultiplyNumbers(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_Multiply())
MultiplyNumbers = fstd.make_node(_fleaux_impl_MultiplyNumbers)

def _fleaux_impl_IsGreater(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    x = _fleaux_args[0]
    y = _fleaux_args[1]
    return ((x, y) | _mod_Std.Std_GreaterThan())
IsGreater = fstd.make_node(_fleaux_impl_IsGreater)

def _fleaux_impl_GetArguments(*_fleaux_args):
    pass
    return (() | _mod_Std.Std_GetArgs())
GetArguments = fstd.make_node(_fleaux_impl_GetArguments)

_fleaux_last_value = ((5,) | Add2())
_fleaux_last_value = ((3, 4) | MultiplyNumbers())
_fleaux_last_value = ((10, 5) | IsGreater())
_fleaux_last_value = ((7, 2) | AddThenMultiply())
_fleaux_last_value = (() | GetArguments())
