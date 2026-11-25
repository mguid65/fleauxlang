from functools import reduce


def double(x):
    return x * 2


def greater_than_five(x):
    return x > 5


def add(acc, x):
    return acc + x


print(tuple(map(double, (1, 2, 3, 4, 5))))
print(tuple(filter(greater_than_five, tuple(map(double, (1, 2, 3, 4, 5))))))
print(reduce(add, tuple(filter(greater_than_five, tuple(map(double, (1, 2, 3, 4, 5))))), 0))
