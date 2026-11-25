def mid(lo, hi):
    return (lo + hi) >> 1


def mid_value(arr, lo, hi):
    return arr[mid(lo, hi)]


def binary_search_next(arr, target, less_than_target, lo, hi):
    if less_than_target(mid_value(arr, lo, hi)):
        return binary_search_range(arr, target, less_than_target, mid(lo, hi) + 1, hi)
    return binary_search_range(arr, target, less_than_target, lo, mid(lo, hi) - 1)


def binary_search_step(arr, target, less_than_target, lo, hi):
    if mid_value(arr, lo, hi) == target:
        return mid(lo, hi)
    return binary_search_next(arr, target, less_than_target, lo, hi)


def binary_search_range(arr, target, less_than_target, lo, hi):
    if lo > hi:
        return -1
    return binary_search_step(arr, target, less_than_target, lo, hi)


def binary_search_index(arr, target, less_than_target):
    return binary_search_range(arr, target, less_than_target, 0, len(arr) - 1)


def make_int64_less_than_target(target):
    return lambda candidate: candidate < target


def search_int64(arr, target):
    return binary_search_index(arr, target, make_int64_less_than_target(target))


print(search_int64((1, 3, 5, 7, 9, 11), 7))
print(search_int64((1, 3, 5, 7, 9, 11), 4))
