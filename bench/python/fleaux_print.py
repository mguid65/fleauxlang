"""Std.Println's text form, used only to check a port's output against the Fleaux output. Timed runs use print."""
import sys


def fleaux_text(value):
    if value is None:
        return 'null'
    if value is True:
        return 'True'
    if value is False:
        return 'False'
    if isinstance(value, float):
        text = repr(value)
        return text[:-2] if text.endswith('.0') else text
    if isinstance(value, (int, str)):
        return str(value)
    if isinstance(value, tuple):
        if len(value) == 1:
            return '(' + fleaux_text(value[0]) + ',)'
        return '(' + ', '.join(fleaux_text(item) for item in value) + ')'
    if isinstance(value, dict):
        return '{' + ', '.join(f'{fleaux_text(key)}: {fleaux_text(item)}' for key, item in value.items()) + '}'
    raise TypeError(f'no Fleaux text form for {type(value).__name__}')


def fleaux_println(*values):
    sys.stdout.write(' '.join(fleaux_text(value) for value in values) + '\n')
