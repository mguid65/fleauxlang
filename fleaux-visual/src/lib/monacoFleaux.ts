import type * as Monaco from 'monaco-editor';

let fleauxRegistered = false;

export function configureFleauxMonaco(monaco: typeof Monaco) {
  if (fleauxRegistered) {
    return;
  }
  fleauxRegistered = true;

  monaco.languages.register({ id: 'fleaux' });

  monaco.languages.setMonarchTokensProvider('fleaux', {
    keywords: ['import', 'let'],
    typeKeywords: [
      'Float64', 'Int64', 'UInt64', 'String', 'Bool', 'Null', 'Any', 'Tuple',
      'Never', 'TaskHandle', 'Result', 'Dict',
    ],
    tokenizer: {
      root: [
        [/\/\/.*$/, 'comment'],
        [/"([^"\\]|\\.)*"/, 'string'],
        [/[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/, 'number'],
        [/__builtin__/, 'keyword'],
        [/[A-Z][A-Za-z0-9_.]*(?=\()/, 'type'],  // Parameterized types: Result(...), Dict(...)
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
    },
  });

  monaco.languages.setLanguageConfiguration('fleaux', {
    comments: {
      lineComment: '//',
    },
    autoClosingPairs: [
      { open: '(', close: ')' },
      { open: '[', close: ']' },
      { open: '"', close: '"' },
    ],
    surroundingPairs: [
      { open: '(', close: ')' },
      { open: '[', close: ']' },
      { open: '"', close: '"' },
    ],
    brackets: [
      ['(', ')'],
      ['[', ']'],
    ],
  });

  monaco.editor.defineTheme('fleaux-dark', {
    base: 'vs-dark',
    inherit: true,
    rules: [
      { token: 'comment', foreground: '6B7280', fontStyle: 'italic' },
      { token: 'keyword', foreground: 'F59E0B', fontStyle: 'bold' },
      { token: 'type', foreground: '22D3EE' },
      { token: 'type.identifier', foreground: '67E8F9', fontStyle: 'bold' },
      { token: 'string', foreground: '34D399' },
      { token: 'number', foreground: '60A5FA' },
      { token: 'predefined', foreground: 'F472B6' },
      { token: 'constant.language', foreground: 'A78BFA', fontStyle: 'bold' },
      { token: 'operator', foreground: 'FBBF24' },
      { token: 'delimiter', foreground: '94A3B8' },
    ],
    colors: {
      'editor.background': '#121420',
      'editor.foreground': '#E2E8F0',
      'editor.lineHighlightBackground': '#1A1D2E',
      'editorLineNumber.foreground': '#64748B',
      'editorLineNumber.activeForeground': '#CBD5E1',
      'editorCursor.foreground': '#F8FAFC',
      'editor.selectionBackground': '#33415580',
      'editor.inactiveSelectionBackground': '#33415540',
    },
  });
}


