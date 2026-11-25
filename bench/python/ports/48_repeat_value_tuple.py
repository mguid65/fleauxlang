def repeat(value, count):
    if count == 0:
        return ()
    return (value,) + repeat(value, count - 1)


print(repeat('ha', 4))
print(repeat(7, 5))
print(len(repeat('x', 0)))
