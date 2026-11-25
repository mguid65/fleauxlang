nums = (1, 2, 3, 4, 5)
print(tuple([x * 2 for x in nums]))
print(tuple([x for x in [x * 2 for x in nums] if x > 5]))
print(sum([x for x in [x * 2 for x in nums] if x > 5]))
