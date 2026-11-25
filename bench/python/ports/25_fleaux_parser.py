def is_not_empty(line):
    return line != ''


def strip_comment_line(line):
    return '' if line.lstrip().startswith('//') else line


def normalize_for_statements(src):
    return '\n'.join(map(strip_comment_line, src.split('\n')))


def split_statements(src):
    return tuple(filter(is_not_empty, tuple(map(str.strip, src.split(';\n')))))


def demo_source():
    return ''.join(('import Std;\n',
                    'let AddOne(x: Int64): Int64 = (x, 1) -> Std.Add;\n',
                    '(41) -> AddOne -> Std.Println;\n'))


def parse_preview(src):
    return split_statements(normalize_for_statements(src))


def main():
    print(parse_preview(demo_source()))


main()
