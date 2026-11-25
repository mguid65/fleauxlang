def double(x):
    return x * 2.0


def square(x):
    return x * x


def identity(x):
    return x


def build_ops():
    return {'double': double, 'square': square}


def apply_named_op(name, value):
    return build_ops().get(name, identity)(value)


print(apply_named_op('double', 5.0))
print(apply_named_op('square', 5.0))
print(apply_named_op('unknown', 5.0))
