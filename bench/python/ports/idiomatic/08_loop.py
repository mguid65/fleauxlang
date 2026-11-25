n = 10.0
while n > 0.0:
    n -= 1.0
print(n)

counter, total = 10.0, 0.0
while counter > 0.0:
    counter, total = counter - 1.0, total + counter
print((counter, total))


def fib(n):
    a, b = 0.0, 1.0
    while n > 0.0:
        n, a, b = n - 1.0, b, a + b
    return a


print(fib(10.0))
print(fib(0.0))

n, total = 5.0, 0.0
while n > 0.0:
    n, total = n - 1.0, total + n
print(total)

remaining, current = 8.0, 1.0
for _ in range(10):
    if not remaining > 0.0:
        break
    remaining, current = remaining - 1.0, current * 2.0
else:
    if remaining > 0.0:
        raise RuntimeError('LoopN: exceeded max_iters')
print(current)
