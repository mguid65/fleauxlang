import fleaux_std_builtins as fstd

def _fleaux_missing_builtin(name):
    class _MissingBuiltin:
        def __ror__(self, _tuple_args):
            raise NotImplementedError(
                f"Builtin '{name}' is not yet implemented in fleaux_std_builtins.py"
            )
    return _MissingBuiltin

import fleaux_generated_module_Std as _mod_Std

def _fleaux_impl_Add3(*_fleaux_args):
    tpl = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((((tpl, 0) | _mod_Std.Std_ElementAt()), ((tpl, 1) | _mod_Std.Std_ElementAt())) | _mod_Std.Std_Add()), ((tpl, 2) | _mod_Std.Std_ElementAt())) | _mod_Std.Std_Add())
Add3 = fstd.make_node(_fleaux_impl_Add3)

_fleaux_last_value = ((1, 2, 3) | Add3())
