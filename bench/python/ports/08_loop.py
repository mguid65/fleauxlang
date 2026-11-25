def count_continue(n):
    return n > 0.0


def count_step(n):
    return n - 1.0


n = 10.0
while count_continue(n):
    n = count_step(n)
print(n)


def sum_continue(state):
    return state[0] > 0.0


def sum_step(state):
    return (state[0] - 1.0, state[1] + state[0])


state = (10.0, 0.0)
while sum_continue(state):
    state = sum_step(state)
print(state)


def fib_continue(state):
    return state[0] > 0.0


def fib_step(state):
    return (state[0] - 1.0, state[2], state[1] + state[2])


def fib(n):
    state = (n, 0.0, 1.0)
    while fib_continue(state):
        state = fib_step(state)
    return state[1]


print(fib(10.0))
print(fib(0.0))

state = (5.0, 0.0)
while state[0] > 0.0:
    state = (state[0] - 1.0, state[1] + state[0])
print(state[1])

state = (8.0, 1.0)
iterations = 0
while state[0] > 0.0:
    if iterations >= 10:
        raise RuntimeError('LoopN: exceeded max_iters')
    state = (state[0] - 1.0, state[1] * 2.0)
    iterations += 1
print(state[1])
