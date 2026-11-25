def sum_items(items):
    total = 0
    index = 0
    while index < len(items):
        total += items[index]
        index += 1
    return total


print(sum_items((3, 1, 4, 1, 5)))
