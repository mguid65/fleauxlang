def sum_items(items):
    state = (0, 0)
    while state[0] < len(items):
        state = (state[0] + 1, state[1] + items[state[0]])
    return state[1]


print(sum_items((3, 1, 4, 1, 5)))
