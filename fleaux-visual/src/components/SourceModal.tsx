import { useEffect, useMemo, useState, type ReactNode } from 'react';

type SourceModalProps = {
  isOpen: boolean;
  sourceText: string;
  onClose: () => void;
};

const KEYWORDS = new Set(['import', 'let']);
const CONSTANTS = new Set(['True', 'False', 'null']);
const MULTI_CHAR_SYMBOLS = ['...', '->', '::', '==', '!=', '>=', '<=', '&&', '||'];
const SINGLE_CHAR_SYMBOLS = new Set('()[],:;.=+-*/%^!<>');

function isIdentStart(ch: string): boolean {
  return /[A-Za-z_]/.test(ch);
}

function isIdentChar(ch: string): boolean {
  return /[A-Za-z0-9_.]/.test(ch);
}

function highlightLine(line: string): ReactNode[] {
  const nodes: ReactNode[] = [];
  let i = 0;
  let key = 0;

  const push = (text: string, className?: string) => {
    if (!text) {
      return;
    }
    if (className) {
      nodes.push(
        <span key={key++} className={className}>
          {text}
        </span>,
      );
      return;
    }
    nodes.push(<span key={key++}>{text}</span>);
  };

  while (i < line.length) {
    const rest = line.slice(i);

    if (rest.startsWith('//')) {
      push(rest, 'fleaux-token-comment');
      break;
    }

    if (line[i] === '"') {
      let j = i + 1;
      while (j < line.length) {
        if (line[j] === '"' && line[j - 1] !== '\\') {
          j += 1;
          break;
        }
        j += 1;
      }
      push(line.slice(i, j), 'fleaux-token-string');
      i = j;
      continue;
    }

    let matchedSymbol = false;
    for (const sym of MULTI_CHAR_SYMBOLS) {
      if (rest.startsWith(sym)) {
        push(sym, 'fleaux-token-operator');
        i += sym.length;
        matchedSymbol = true;
        break;
      }
    }
    if (matchedSymbol) {
      continue;
    }

    if (SINGLE_CHAR_SYMBOLS.has(line[i])) {
      push(line[i], 'fleaux-token-operator');
      i += 1;
      continue;
    }

    if (/[0-9]/.test(line[i])) {
      let j = i + 1;
      while (j < line.length && /[0-9.eE+-]/.test(line[j])) {
        if ((line[j] === '+' || line[j] === '-') && !(line[j - 1] === 'e' || line[j - 1] === 'E')) {
          break;
        }
        j += 1;
      }
      push(line.slice(i, j), 'fleaux-token-number');
      i = j;
      continue;
    }

    if (isIdentStart(line[i])) {
      let j = i + 1;
      while (j < line.length && isIdentChar(line[j])) {
        j += 1;
      }
      const word = line.slice(i, j);
      if (KEYWORDS.has(word)) {
        push(word, 'fleaux-token-keyword');
      } else if (CONSTANTS.has(word)) {
        push(word, 'fleaux-token-constant');
      } else if (word === 'Std' || word.startsWith('Std.')) {
        push(word, 'fleaux-token-builtin');
      } else if (/^[A-Z][A-Za-z0-9_.]*$/.test(word)) {
        push(word, 'fleaux-token-type');
      } else {
        push(word);
      }
      i = j;
      continue;
    }

    push(line[i]);
    i += 1;
  }

  return nodes;
}

export function SourceModal({ isOpen, sourceText, onClose }: SourceModalProps) {
  const [copyLabel, setCopyLabel] = useState('Copy');

  const lines = useMemo(() => sourceText.split('\n'), [sourceText]);
  const gutterWidth = useMemo(() => String(Math.max(lines.length, 1)).length, [lines.length]);

  useEffect(() => {
    if (!isOpen) {
      return;
    }
    const onKeyDown = (evt: KeyboardEvent) => {
      if (evt.key === 'Escape') {
        onClose();
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [isOpen, onClose]);

  if (!isOpen) {
    return null;
  }

  const copySource = async () => {
    try {
      await navigator.clipboard.writeText(sourceText);
      setCopyLabel('Copied');
      setTimeout(() => setCopyLabel('Copy'), 1200);
    } catch (error) {
      console.error('Failed to copy generated source', error);
      setCopyLabel('Copy failed');
      setTimeout(() => setCopyLabel('Copy'), 1400);
    }
  };

  return (
    <div
      className="fixed inset-0 z-50 bg-black/70 backdrop-blur-[1px] flex items-center justify-center p-4"
      onClick={onClose}
      role="dialog"
      aria-modal="true"
      aria-label="Generated Fleaux Source"
    >
      <div
        className="w-[min(1100px,96vw)] h-[min(88vh,96vh)] rounded-xl border border-[#2d3148] bg-[#121420] shadow-2xl flex flex-col"
        onClick={(evt) => evt.stopPropagation()}
      >
        <div className="flex items-center justify-between px-4 py-3 border-b border-[#2d3148]">
          <div className="text-sm font-semibold text-slate-200">Generated Fleaux Source</div>
          <div className="flex items-center gap-2">
            <button
              type="button"
              onClick={() => void copySource()}
              className="text-xs font-mono border border-cyan-700 text-cyan-300 hover:bg-cyan-950 rounded px-3 py-1.5 transition-colors cursor-pointer"
            >
              {copyLabel}
            </button>
            <button
              type="button"
              onClick={onClose}
              className="text-xs font-mono border border-slate-700 text-slate-300 hover:bg-slate-800 rounded px-3 py-1.5 transition-colors cursor-pointer"
            >
              Close
            </button>
          </div>
        </div>

        <div className="flex-1 overflow-auto p-4">
          {sourceText.trim().length === 0 ? (
            <div className="text-sm text-slate-500">No generated source yet. Run the graph first.</div>
          ) : (
            <pre className="fleaux-code-view text-[13px] leading-6 min-w-max">
              {lines.map((line, index) => (
                <div key={`line-${index}`} className="flex">
                  <span className="fleaux-code-gutter" style={{ width: `${gutterWidth}ch` }}>
                    {index + 1}
                  </span>
                  <span className="mr-3 text-slate-600 select-none">|</span>
                  <span>{highlightLine(line)}</span>
                </div>
              ))}
            </pre>
          )}
        </div>
      </div>
    </div>
  );
}

