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

def _fleaux_impl_Negate(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((0, x) | _mod_Std.Std_Subtract())
Negate = fstd.make_node(_fleaux_impl_Negate)

def _fleaux_impl_AddTen(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((x, 10) | _mod_Std.Std_Add())
AddTen = fstd.make_node(_fleaux_impl_AddTen)

def _fleaux_impl_AbsViaSelect(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((x, 0) | _mod_Std.Std_GreaterOrEqual()), x, ((0, x) | _mod_Std.Std_Subtract())) | _mod_Std.Std_Select())
AbsViaSelect = fstd.make_node(_fleaux_impl_AbsViaSelect)

def _fleaux_impl_AbsViaBranch(*_fleaux_args):
    x = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((((x, 0) | _mod_Std.Std_GreaterOrEqual()), x, Double, Negate) | _mod_Std.Std_Branch()), 2) | _mod_Std.Std_Divide())
AbsViaBranch = fstd.make_node(_fleaux_impl_AbsViaBranch)

def _fleaux_impl_RunWith5(*_fleaux_args):
    f = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((5, f) | _mod_Std.Std_Apply())
RunWith5 = fstd.make_node(_fleaux_impl_RunWith5)

_fleaux_last_value = (((-4,) | AbsViaSelect()) | _mod_Std.Std_Println())
_fleaux_last_value = (((4,) | AbsViaSelect()) | _mod_Std.Std_Println())
_fleaux_last_value = (((-3,) | AbsViaBranch()) | _mod_Std.Std_Println())
_fleaux_last_value = (((8,) | AbsViaBranch()) | _mod_Std.Std_Println())
_fleaux_last_value = (((AddTen,) | RunWith5()) | _mod_Std.Std_Println())
_fleaux_last_value = (((Double,) | RunWith5()) | _mod_Std.Std_Println())
_fleaux_last_value = (((((7, 0) | _mod_Std.Std_GreaterThan()), 7, Double, AddTen) | _mod_Std.Std_Branch()) | _mod_Std.Std_Println())
_fleaux_last_value = (((((-2, 0) | _mod_Std.Std_GreaterThan()), -2, Double, AddTen) | _mod_Std.Std_Branch()) | _mod_Std.Std_Println())
