def is_even(x):
    return x % 2 == 0


def current_item(items, state):
    return items[state[0]]


def summarize_numbers(items):
    state = (0, 0, 0, 0)
    while state[0] < len(items):
        state = (state[0] + 1,
                 state[1] + current_item(items, state),
                 state[2] + 1 if is_even(current_item(items, state)) else state[2],
                 state[3] if is_even(current_item(items, state)) else state[3] + 1)
    return (state[1], state[2], state[3])


print(summarize_numbers((3, 1, 4, 1, 5, 9)))
print(summarize_numbers((2, 4, 6, 8)))
