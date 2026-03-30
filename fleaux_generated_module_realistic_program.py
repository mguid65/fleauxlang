import fleaux_std_builtins as fstd

def _fleaux_missing_builtin(name):
    class _MissingBuiltin:
        def __ror__(self, _tuple_args):
            raise NotImplementedError(
                f"Builtin '{name}' is not yet implemented in fleaux_std_builtins.py"
            )
    return _MissingBuiltin

import fleaux_generated_module_Std as _mod_Std

def _fleaux_impl_Sum3(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    a = _fleaux_args[0]
    b = _fleaux_args[1]
    c = _fleaux_args[2]
    return ((((a, b) | _mod_Std.Std_Add()), c) | _mod_Std.Std_Add())
Sum3 = fstd.make_node(_fleaux_impl_Sum3)

def _fleaux_impl_ComputeSubtotal(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    unit_price = _fleaux_args[0]
    qty = _fleaux_args[1]
    return ((unit_price, qty) | _mod_Std.Std_Multiply())
ComputeSubtotal = fstd.make_node(_fleaux_impl_ComputeSubtotal)

def _fleaux_impl_NoDiscount(*_fleaux_args):
    subtotal = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return subtotal
NoDiscount = fstd.make_node(_fleaux_impl_NoDiscount)

def _fleaux_impl_VipDiscount(*_fleaux_args):
    subtotal = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((subtotal, 0.9) | _mod_Std.Std_Multiply())
VipDiscount = fstd.make_node(_fleaux_impl_VipDiscount)

def _fleaux_impl_SelectDiscountFn(*_fleaux_args):
    is_vip = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((is_vip, VipDiscount, NoDiscount) | _mod_Std.Std_Select())
SelectDiscountFn = fstd.make_node(_fleaux_impl_SelectDiscountFn)

def _fleaux_impl_ApplyDiscount(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    subtotal = _fleaux_args[0]
    is_vip = _fleaux_args[1]
    return ((subtotal, ((is_vip,) | SelectDiscountFn())) | _mod_Std.Std_Apply())
ApplyDiscount = fstd.make_node(_fleaux_impl_ApplyDiscount)

def _fleaux_impl_SalesTax(*_fleaux_args):
    subtotal = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((subtotal, 0.08) | _mod_Std.Std_Multiply())
SalesTax = fstd.make_node(_fleaux_impl_SalesTax)

def _fleaux_impl_FreeShipping(*_fleaux_args):
    subtotal = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return 0
FreeShipping = fstd.make_node(_fleaux_impl_FreeShipping)

def _fleaux_impl_StandardShipping(*_fleaux_args):
    subtotal = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return 7.5
StandardShipping = fstd.make_node(_fleaux_impl_StandardShipping)

def _fleaux_impl_ShippingCost(*_fleaux_args):
    subtotal = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((((subtotal, 100) | _mod_Std.Std_GreaterThan()), subtotal, FreeShipping, StandardShipping) | _mod_Std.Std_Branch())
ShippingCost = fstd.make_node(_fleaux_impl_ShippingCost)

def _fleaux_impl_FinalizeTotal(*_fleaux_args):
    discounted_subtotal = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((discounted_subtotal, ((discounted_subtotal,) | SalesTax()), ((discounted_subtotal,) | ShippingCost())) | Sum3())
FinalizeTotal = fstd.make_node(_fleaux_impl_FinalizeTotal)

def _fleaux_impl_CheckoutTotal(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    unit_price = _fleaux_args[0]
    qty = _fleaux_args[1]
    is_vip = _fleaux_args[2]
    return (((((unit_price, qty) | ComputeSubtotal()), is_vip) | ApplyDiscount()) | FinalizeTotal())
CheckoutTotal = fstd.make_node(_fleaux_impl_CheckoutTotal)

def _fleaux_impl_ContinuePayoff(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    month = _fleaux_args[0]
    balance = _fleaux_args[1]
    paid = _fleaux_args[2]
    return ((((balance, 0) | _mod_Std.Std_GreaterThan()), ((month, 120) | _mod_Std.Std_LessThan())) | _mod_Std.Std_And())
ContinuePayoff = fstd.make_node(_fleaux_impl_ContinuePayoff)

def _fleaux_impl_StepPayoff(*_fleaux_args):
    if len(_fleaux_args) < 3:
        raise TypeError(f'Expected 3 arguments, got {len(_fleaux_args)}')
    month = _fleaux_args[0]
    balance = _fleaux_args[1]
    paid = _fleaux_args[2]
    return (((month, 1) | _mod_Std.Std_Add()), ((balance, ((((balance, 500) | _mod_Std.Std_GreaterThan()), 500, balance) | _mod_Std.Std_Select())) | _mod_Std.Std_Subtract()), ((paid, ((((balance, 500) | _mod_Std.Std_GreaterThan()), 500, balance) | _mod_Std.Std_Select())) | _mod_Std.Std_Add()))
StepPayoff = fstd.make_node(_fleaux_impl_StepPayoff)

def _fleaux_impl_PayoffLoop(*_fleaux_args):
    balance = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return (((0, balance, 0), ContinuePayoff, StepPayoff) | _mod_Std.Std_Loop())
PayoffLoop = fstd.make_node(_fleaux_impl_PayoffLoop)

def _fleaux_impl_PayoffLoopN(*_fleaux_args):
    if len(_fleaux_args) < 2:
        raise TypeError(f'Expected 2 arguments, got {len(_fleaux_args)}')
    balance = _fleaux_args[0]
    max_months = _fleaux_args[1]
    return (((0, balance, 0), ContinuePayoff, StepPayoff, max_months) | _mod_Std.Std_LoopN())
PayoffLoopN = fstd.make_node(_fleaux_impl_PayoffLoopN)

def _fleaux_impl_GetMonth(*_fleaux_args):
    state = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((state, 0) | _mod_Std.Std_ElementAt())
GetMonth = fstd.make_node(_fleaux_impl_GetMonth)

def _fleaux_impl_GetRemaining(*_fleaux_args):
    state = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((state, 1) | _mod_Std.Std_ElementAt())
GetRemaining = fstd.make_node(_fleaux_impl_GetRemaining)

def _fleaux_impl_GetPaid(*_fleaux_args):
    state = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]
    return ((state, 2) | _mod_Std.Std_ElementAt())
GetPaid = fstd.make_node(_fleaux_impl_GetPaid)

_fleaux_last_value = (((50, 3, ((1200, 1000) | _mod_Std.Std_GreaterThan())) | CheckoutTotal()) | _mod_Std.Std_Println())
_fleaux_last_value = (((20, 4, ((800, 1000) | _mod_Std.Std_GreaterThan())) | CheckoutTotal()) | _mod_Std.Std_Println())
_fleaux_last_value = (((6200,) | PayoffLoop()) | _mod_Std.Std_Println())
_fleaux_last_value = (((6200, 24) | PayoffLoopN()) | _mod_Std.Std_Println())
_fleaux_last_value = ((((6200, 24) | PayoffLoopN()) | GetMonth()) | _mod_Std.Std_Println())
_fleaux_last_value = ((((6200, 24) | PayoffLoopN()) | GetRemaining()) | _mod_Std.Std_Println())
_fleaux_last_value = ((((6200, 24) | PayoffLoopN()) | GetPaid()) | _mod_Std.Std_Println())
