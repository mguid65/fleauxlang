def double(x):
    return x * 2.0


def square(x):
    return x * x


def identity(x):
    return x


def build_op_tags():
    return {'grow': 'double', 'power': 'square'}


def apply_with_tag(name, value):
    tag = build_op_tags().get(name, 'identity')
    if tag == 'double':
        return double(value)
    elif tag == 'square':
        return square(value)
    else:
        return value


def build_ops():
    return {'grow': double, 'power': square}


def apply_with_callable(name, value):
    return build_ops().get(name, identity)(value)


print(apply_with_tag('grow', 5.0))
print(apply_with_tag('power', 5.0))
print(apply_with_tag('unknown', 5.0))
print(apply_with_callable('grow', 5.0))
print(apply_with_callable('power', 5.0))
print(apply_with_callable('unknown', 5.0))
