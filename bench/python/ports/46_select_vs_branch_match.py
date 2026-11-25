def is_even(n):
    return n % 2 == 0


def choose_parity_label(n):
    return 'even' if is_even(n) else 'odd'


def return_one(_):
    return 1


def multiply_by_recursive_branch_step(n):
    return n * factorial_with_branch(n - 1)


def factorial_with_branch(n):
    return return_one(n) if n == 0 else multiply_by_recursive_branch_step(n)


def factorial_with_match(n):
    if n == 0:
        return 1
    return n * factorial_with_match(n - 1)


print(choose_parity_label(4))
print(factorial_with_branch(0))
print(factorial_with_branch(5))
print(factorial_with_match(5))
