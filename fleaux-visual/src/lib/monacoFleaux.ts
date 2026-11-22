import type * as Monaco from 'monaco-editor';

let fleauxRegistered = false;

export function configureFleauxMonaco(monaco: typeof Monaco) {
  if (fleauxRegistered) {
    return;
  }
  fleauxRegistered = true;

  monaco.languages.register({ id: 'fleaux' });

  monaco.languages.setMonarchTokensProvider('fleaux', {
    keywords: ['import', 'let', 'type', 'alias'],
    typeKeywords: [
      'Float64', 'Int64', 'UInt64', 'String', 'Bool', 'Null', 'Any', 'Tuple',
      'Never', 'TaskHandle', 'Result', 'Dict', 'FileHandle', 'RandomGenerator',
      'Numeric', 'Integer', 'Signed', 'Comparable',
    ],
    tokenizer: {
      root: [
        [/\/\/.*$/, 'comment'],
        // A raw string opens with three or more quotes and closes with the same number; the state name carries them.
        // It is multi-line when only whitespace follows the opening quotes.
        [/("{3,})(?=[ \t\r]*$)/, { token: 'string', next: '@rawMultiLine.$1' }],
        [/"{3,}/, { token: 'string', next: '@rawOneLine.$#' }],
        [/"([^"\\]|\\.)*"/, 'string'],
        [/[0-9]+(?:u64|(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)/, 'number'],
        [/__builtin__/, 'keyword'],
        [/[A-Z][A-Za-z0-9_.]*(?=<)/, 'type'],  // Parameterized types: Result<T, E>, Dict<K, V>
        [/[A-Z][A-Za-z0-9_.]*/, {
          cases: {
            '@typeKeywords': 'type.identifier',
            '@default': 'type',
          },
        }],
        [/[a-zA-Z_][A-Za-z0-9_.]*/, {
          cases: {
            '@keywords': 'keyword',
            'True|False|null': 'constant.language',
            'Std|Std.[A-Za-z0-9_.]+': 'predefined',
            '@default': 'identifier',
          },
        }],
        [/\./, 'delimiter'],
        [/[,;:()[\]{}]/, 'delimiter'],
        [/\.\.\.|->|::|=>|==|!=|>=|<=|&&|\|\||[\^/%*+\-<>!=|]/, 'operator'],
        [/\s+/, 'white'],
      ],
      // Raw string content has no escapes: only a run of exactly the opening quotes ends it.
      rawMultiLine: [
        [/"+/, { cases: { '$#==$S2': { token: 'string', next: '@pop' }, '@default': 'string' } }],
        [/[^"]+/, 'string'],
      ],
      // A one-line raw string that reaches the end of its line is unterminated.
      rawOneLine: [
        [/"+/, {
          cases: {
            '$#==$S2': { token: 'string', next: '@pop' },
            '@eos': { token: 'string.invalid', next: '@pop' },
            '@default': 'string',
          },
        }],
        [/[^"]+/, { cases: { '@eos': { token: 'string.invalid', next: '@pop' }, '@default': 'string' } }],
      ],
    },
  });

  monaco.languages.setLanguageConfiguration('fleaux', {
    comments: {
      lineComment: '//',
    },
    autoClosingPairs: [
      { open: '(', close: ')' },
      { open: '[', close: ']' },
      { open: '{', close: '}' },
      { open: '"', close: '"' },
    ],
    surroundingPairs: [
      { open: '(', close: ')' },
      { open: '[', close: ']' },
      { open: '{', close: '}' },
      { open: '"', close: '"' },
    ],
    brackets: [
      ['(', ')'],
      ['[', ']'],
      ['{', '}'],
    ],
  });

  monaco.editor.defineTheme('fleaux-dark', {
    base: 'vs-dark',
    inherit: true,
    rules: [
      { token: 'comment', foreground: '737373', fontStyle: 'italic' },
      { token: 'keyword', foreground: 'F59E0B', fontStyle: 'bold' },
      { token: 'type', foreground: 'C4B5FD' },
      { token: 'type.identifier', foreground: 'DDD6FE', fontStyle: 'bold' },
      { token: 'string', foreground: '34D399' },
      { token: 'string.invalid', foreground: 'F87171' },
      { token: 'number', foreground: 'FDBA74' },
      { token: 'predefined', foreground: 'F472B6' },
      { token: 'constant.language', foreground: 'F0ABFC', fontStyle: 'bold' },
      { token: 'operator', foreground: 'FBBF24' },
      { token: 'delimiter', foreground: 'A3A3A3' },
    ],
    colors: {
      'editor.background': '#151517',
      'editor.foreground': '#E5E5E5',
      'editor.lineHighlightBackground': '#1C1C1F',
      'editorLineNumber.foreground': '#737373',
      'editorLineNumber.activeForeground': '#D4D4D4',
      'editorCursor.foreground': '#FAFAFA',
      'editor.selectionBackground': '#52525B80',
      'editor.inactiveSelectionBackground': '#52525B40',
      'scrollbar.shadow': '#00000000',
      'scrollbarSlider.background': '#3F3F4580',
      'scrollbarSlider.hoverBackground': '#52525BB0',
      'scrollbarSlider.activeBackground': '#71717AC0',
    },
  });
}


