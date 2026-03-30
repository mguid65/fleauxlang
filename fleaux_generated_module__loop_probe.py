import fleaux_std_builtins as fstd

def _fleaux_missing_builtin(name):
    class _MissingBuiltin:
        def __ror__(self, _tuple_args):
            raise NotImplementedError(
                f"Builtin '{name}' is not yet implemented in fleaux_std_builtins.py"
            )
    return _MissingBuiltin

import fleaux_generated_module_Std as _mod_Std

def _fleaux_impl_Identity(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return x
Identity = fstd.make_node(_fleaux_impl_Identity)

def _fleaux_impl_FactStep(*_fleaux_args):
    n = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return (((((n, 1) | _mod_Std.Std_Subtract()) | Factorial()), n) | _mod_Std.Std_Multiply())
FactStep = fstd.make_node(_fleaux_impl_FactStep)

def _fleaux_impl_Factorial(*_fleaux_args):
    n = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((n, 1) | _mod_Std.Std_LessOrEqual()), n, Identity, FactStep) | _mod_Std.Std_Branch())
Factorial = fstd.make_node(_fleaux_impl_Factorial)

_fleaux_last_value = (((0,) | Factorial()) | _mod_Std.Std_Println())
_fleaux_last_value = (((1,) | Factorial()) | _mod_Std.Std_Println())
_fleaux_last_value = (((5,) | Factorial()) | _mod_Std.Std_Println())
_fleaux_last_value = (((10,) | Factorial()) | _mod_Std.Std_Println())
