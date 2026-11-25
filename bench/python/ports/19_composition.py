def square(x):
    return x * x


def double(x):
    return x * 2.0


def inc(x):
    return x + 1.0


def negate(x):
    return 0.0 - x


def absolute(x):
    return x if x >= 0.0 else 0.0 - x


print(double(square(5.0)))
print(absolute(negate(-7.0)))


def apply_twice(x, f):
    return f(f(x))


print(apply_twice(3.0, double))
print(apply_twice(2.0, inc))

print(tuple(map(square, (1.0, 2.0, 3.0, 4.0, 5.0))))
print(tuple(map(double, (1.0, 2.0, 3.0, 4.0, 5.0))))


def is_even(x):
    return x % 2.0 == 0.0


print(tuple(map(double, tuple(filter(is_even, (1.0, 2.0, 3.0, 4.0, 5.0, 6.0))))))


def positive(x):
    return x + 1.0


def non_positive(x):
    return 0.0 - x


def transform(x):
    return positive(x) if x > 0.0 else non_positive(x)


print(transform(5.0))
print(transform(-3.0))
