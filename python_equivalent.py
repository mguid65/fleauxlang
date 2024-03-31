import fleaux_std_lib as fstd


def _materialized_rhs_expression_MultiplyPrint_0(x: float, y: float):
    return ((x, y) | fstd.Multiply(),) | fstd.Println()


MultiplyPrint = fstd.make_node(_materialized_rhs_expression_MultiplyPrint_0)


def _materialized_rhs_expression_Polynomial_1(x: float):
    return (((4, x) | MultiplyPrint(), 7) | fstd.Pow(), ((x, 5) | fstd.Pow())) | fstd.Subtract()


Polynomial = fstd.make_node(_materialized_rhs_expression_Polynomial_1)

((4 | fstd.Wrap() | Polynomial()), (6 | fstd.Wrap() | Polynomial())) | fstd.Println()

1 | fstd.Wrap() | fstd.In() | fstd.ToNum() | fstd.Wrap() | Polynomial() | fstd.Wrap() | fstd.Println()
