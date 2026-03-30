import fleaux_std_builtins as fstd

def _fleaux_missing_builtin(name):
    class _MissingBuiltin:
        def __ror__(self, _tuple_args):
            raise NotImplementedError(
                f"Builtin '{name}' is not yet implemented in fleaux_std_builtins.py"
            )
    return _MissingBuiltin

import fleaux_generated_module_Std as _mod_Std

def _fleaux_impl_ReadNumber(*_fleaux_args):
    prompt = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return (((prompt,) | _mod_Std.Std_Input()) | _mod_Std.Std_ToNum())
ReadNumber = fstd.make_node(_fleaux_impl_ReadNumber)

def _fleaux_impl_Add(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    return ((a, b) | _mod_Std.Std_Add())
Add = fstd.make_node(_fleaux_impl_Add)

def _fleaux_impl_Multiply(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    return ((a, b) | _mod_Std.Std_Multiply())
Multiply = fstd.make_node(_fleaux_impl_Multiply)

def _fleaux_impl_Divide(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    return ((a, b) | _mod_Std.Std_Divide())
Divide = fstd.make_node(_fleaux_impl_Divide)

def _fleaux_impl_TotalWithTip(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    subtotal = _fleaux_args[0]
    tip_rate = _fleaux_args[1]
    return ((subtotal, ((subtotal, tip_rate) | Multiply())) | Add())
TotalWithTip = fstd.make_node(_fleaux_impl_TotalWithTip)

def _fleaux_impl_PerPerson(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    total = _fleaux_args[0]
    people = _fleaux_args[1]
    return ((total, people) | Divide())
PerPerson = fstd.make_node(_fleaux_impl_PerPerson)

_fleaux_last_value = ((((((('Enter subtotal: ',) | ReadNumber()), (('Enter tip rate (e.g. 0.18): ',) | ReadNumber())) | TotalWithTip()), (('Enter number of people: ',) | ReadNumber())) | PerPerson()) | _mod_Std.Std_Println())
