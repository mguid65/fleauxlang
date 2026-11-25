def summarize_numbers(items):
    index = 0
    total = 0
    even_count = 0
    odd_count = 0
    while index < len(items):
        current = items[index]
        total += current
        if current % 2 == 0:
            even_count += 1
        else:
            odd_count += 1
        index += 1
    return total, even_count, odd_count


print(summarize_numbers((3, 1, 4, 1, 5, 9)))
print(summarize_numbers((2, 4, 6, 8)))
