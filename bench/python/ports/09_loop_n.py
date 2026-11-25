def count_continue(n):
    return n > 0.0


def count_step(n):
    return n - 1.0


n = 10.0
iterations = 0
while count_continue(n):
    if iterations >= 100:
        raise RuntimeError('LoopN: exceeded max_iters')
    n = count_step(n)
    iterations += 1
print(n)


def collatz_continue(n):
    return n > 1.0


def collatz_even_step(n):
    return n / 2.0


def collatz_odd_step(n):
    return n * 3.0 + 1.0


def collatz_step(n):
    return collatz_even_step(n) if n % 2.0 == 0.0 else collatz_odd_step(n)


n = 27.0
iterations = 0
while collatz_continue(n):
    if iterations >= 1000:
        raise RuntimeError('LoopN: exceeded max_iters')
    n = collatz_step(n)
    iterations += 1
print(n)


def pow_continue(n):
    return n < 500.0


def pow_step(n):
    return n * 2.0


n = 1.0
iterations = 0
while pow_continue(n):
    if iterations >= 50:
        raise RuntimeError('LoopN: exceeded max_iters')
    n = pow_step(n)
    iterations += 1
print(n)
