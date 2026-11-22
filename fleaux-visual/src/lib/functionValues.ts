import type { Node } from '@xyflow/react';
import { callSlotCount } from './functionSignatures';
import type { ClosureData, FleauxNodeData, LetData, StdFuncData, UserFuncData } from './types';

// The parameter count of a function type `(A, B) => C`, and whether its last parameter is variadic; null when the
// text is not a function type.
export function functionTypeArity(typeText: string): { count: number; variadic: boolean } | null {
  const text = typeText.trim();
  if (!text.startsWith('(')) {
    return null;
  }
  let depth = 0;
  let close = -1;
  for (let index = 0; index < text.length; index += 1) {
    const ch = text[index];
    if (ch === '(' || ch === '<') depth += 1;
    else if (ch === ')' || (ch === '>' && text[index - 1] !== '=' && text[index - 1] !== '-')) {
      depth -= 1;
      if (depth === 0 && ch === ')') {
        close = index;
        break;
      }
    }
  }
  if (close === -1 || !text.slice(close + 1).trimStart().startsWith('=>')) {
    return null;
  }

  const params: string[] = [];
  const inner = text.slice(1, close);
  let cursor = 0;
  depth = 0;
  for (let index = 0; index < inner.length; index += 1) {
    const ch = inner[index];
    if (ch === '(' || ch === '<') depth += 1;
    else if (ch === ')' || (ch === '>' && inner[index - 1] !== '=' && inner[index - 1] !== '-')) depth -= 1;
    else if (ch === ',' && depth === 0) {
      params.push(inner.slice(cursor, index).trim());
      cursor = index + 1;
    }
  }
  const last = inner.slice(cursor).trim();
  if (last.length > 0) {
    params.push(last);
  }
  return { count: params.length, variadic: params.length > 0 && params[params.length - 1].endsWith('...') };
}

// The argument slots a call of this function value starts with, and its function type for display, when the source
// says what it is: a parameter or value let of function type, a closure, or a function reference.
export function functionValueShape(
  source: Node<FleauxNodeData>,
  sourceHandle: string | null | undefined,
): { slots: number; functionType?: string } | null {
  const fromType = (typeText: string) => {
    const arity = functionTypeArity(typeText);
    return arity ? { slots: arity.count, functionType: typeText } : null;
  };
  const paramIndex = (prefix: string) =>
    sourceHandle?.startsWith(prefix) ? Number.parseInt(sourceHandle.slice(prefix.length), 10) : null;

  switch (source.data.kind) {
    case 'let': {
      const data = source.data as LetData;
      const index = paramIndex('let-param-');
      return index !== null && data.params[index] ? fromType(data.params[index].type) : null;
    }
    case 'closure': {
      const data = source.data as ClosureData;
      const index = paramIndex('closure-param-');
      if (index !== null) {
        return data.params[index] ? fromType(data.params[index].type) : null;
      }
      return { slots: callSlotCount(data.params, undefined) };
    }
    case 'userFunc': {
      const data = source.data as UserFuncData;
      if (data.isReference !== true) return null;
      return data.params.length > 0 ? { slots: callSlotCount(data.params, undefined) } : fromType(data.returnType);
    }
    case 'stdFunc': {
      const data = source.data as StdFuncData;
      return data.isReference === true ? { slots: callSlotCount(data.params, undefined) } : null;
    }
    default:
      return null;
  }
}
