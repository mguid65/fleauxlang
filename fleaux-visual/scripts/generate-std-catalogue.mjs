import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const STD_PATH = path.resolve(__dirname, '../../Std.fleaux');
const OUT_PATH = path.resolve(__dirname, '../src/lib/stdCatalogue.ts');

function stripLineComments(source) {
  const out = [];
  let inString = false;
  let escape = false;

  for (let index = 0; index < source.length; index += 1) {
    const ch = source[index];
    if (inString) {
      out.push(ch);
      if (escape) {
        escape = false;
      } else if (ch === '\\') {
        escape = true;
      } else if (ch === '"') {
        inString = false;
      }
      continue;
    }

    if (ch === '"') {
      inString = true;
      out.push(ch);
      continue;
    }

    if (ch === '/' && source[index + 1] === '/') {
      while (index < source.length && source[index] !== '\n') {
        index += 1;
      }
      if (index < source.length) {
        out.push(source[index]);
      }
      continue;
    }

    out.push(ch);
  }

  return out.join('');
}

function splitStatements(source) {
  const statements = [];
  let depthParen = 0;
  let depthAngle = 0;
  let inString = false;
  let escape = false;
  let start = 0;

  for (let index = 0; index < source.length; index += 1) {
    const ch = source[index];
    if (inString) {
      if (escape) {
        escape = false;
      } else if (ch === '\\') {
        escape = true;
      } else if (ch === '"') {
        inString = false;
      }
      continue;
    }

    if (ch === '"') {
      inString = true;
      continue;
    }

    if (ch === '(') {
      depthParen += 1;
      continue;
    }
    if (ch === ')') {
      depthParen = Math.max(0, depthParen - 1);
      continue;
    }
    if (ch === '<') {
      depthAngle += 1;
      continue;
    }
    if (ch === '>') {
      depthAngle = Math.max(0, depthAngle - 1);
      continue;
    }

    if (ch === ';' && depthParen === 0 && depthAngle === 0) {
      const stmt = source.slice(start, index + 1).trim();
      if (stmt.length > 0) {
        statements.push(stmt.replace(/\s+/g, ' ').trim());
      }
      start = index + 1;
    }
  }

  return statements;
}

function splitTopLevel(input, delimiter) {
  const parts = [];
  let depthParen = 0;
  let depthAngle = 0;
  let token = '';
  for (const ch of input) {
    if (ch === '(') depthParen += 1;
    if (ch === ')') depthParen -= 1;
    if (ch === '<') depthAngle += 1;
    if (ch === '>') depthAngle = Math.max(0, depthAngle - 1);
    if (ch === delimiter && depthParen === 0 && depthAngle === 0) {
      parts.push(token.trim());
      token = '';
      continue;
    }
    token += ch;
  }
  if (token.trim().length > 0) {
    parts.push(token.trim());
  }
  return parts;
}

function findTopLevelChar(input, target) {
  let depthParen = 0;
  let depthAngle = 0;
  for (let index = 0; index < input.length; index += 1) {
    const ch = input[index];
    if (ch === target && depthParen === 0 && depthAngle === 0) return index;
    if (ch === '(') depthParen += 1;
    else if (ch === ')') depthParen -= 1;
    else if (ch === '<') depthAngle += 1;
    else if (ch === '>') depthAngle = Math.max(0, depthAngle - 1);
  }
  return -1;
}

function findMatchingParen(input, openIndex) {
  let depth = 0;
  for (let index = openIndex; index < input.length; index += 1) {
    const ch = input[index];
    if (ch === '(') {
      depth += 1;
    } else if (ch === ')') {
      depth -= 1;
      if (depth === 0) {
        return index;
      }
    }
  }
  return -1;
}

function splitQualifiedNameAndTypeParams(rawName) {
  const trimmed = rawName.trim();
  if (!trimmed.endsWith('>')) {
    return { qualifiedName: trimmed, typeParams: [] };
  }

  let depth = 0;
  for (let index = trimmed.length - 1; index >= 0; index -= 1) {
    const ch = trimmed[index];
    if (ch === '>') {
      depth += 1;
      continue;
    }
    if (ch === '<') {
      depth -= 1;
      if (depth === 0) {
        const qualifiedName = trimmed.slice(0, index).trim();
        const typeParamsText = trimmed.slice(index + 1, -1).trim();
        const typeParams = typeParamsText.length === 0 ? [] : splitTopLevel(typeParamsText, ',');
        return { qualifiedName, typeParams };
      }
    }
  }

  return { qualifiedName: trimmed, typeParams: [] };
}

function parseParam(raw) {
  const idx = raw.indexOf(':');
  if (idx === -1) {
    return { name: raw.trim(), type: 'Any' };
  }
  return {
    name: raw.slice(0, idx).trim(),
    type: raw.slice(idx + 1).trim(),
  };
}

function formatTypeParams(typeParams) {
  if (typeParams.length === 0) {
    return '';
  }
  return '<' + typeParams.join(', ') + '>';
}

function formatParamSignature(params) {
  if (params.length === 0) {
    return '()';
  }
  return '(' + params.map((param) => param.name + ': ' + param.type).join(', ') + ')';
}

function hasVariadicTail(params) {
  if (params.length === 0) {
    return false;
  }
  return params[params.length - 1].type.trim().endsWith('...');
}

function minimumArity(params) {
  return hasVariadicTail(params) ? Math.max(0, params.length - 1) : params.length;
}

function parseLetStatement(statement) {
  const trimmed = statement.trim();
  if (!trimmed.startsWith('let ') || !trimmed.endsWith(';')) {
    return null;
  }

  const bodySeparator = trimmed.lastIndexOf('::');
  if (bodySeparator === -1) {
    return null;
  }

  const header = trimmed.slice(4, bodySeparator).trim();
  const body = trimmed.slice(bodySeparator + 2, -1).trim();
  const open = findTopLevelChar(header, '(');
  if (open === -1) {
    return null;
  }

  const close = findMatchingParen(header, open);
  if (close === -1) {
    return null;
  }

  const nameSection = header.slice(0, open).trim();
  const paramsRaw = header.slice(open + 1, close).trim();
  const rest = header.slice(close + 1).trim();
  if (!rest.startsWith(':')) {
    return null;
  }

  const { qualifiedName, typeParams } = splitQualifiedNameAndTypeParams(nameSection);
  const params = paramsRaw.length === 0 ? [] : splitTopLevel(paramsRaw, ',').map(parseParam);
  const returnType = rest.slice(1).trim();

  return { qualifiedName, typeParams, params, returnType, body };
}

function namespaceFrom(qualifiedName) {
  const parts = qualifiedName.split('.');
  if (parts.length <= 1) {
    return 'Std';
  }
  return parts.slice(0, -1).join('.');
}

function symbolFrom(qualifiedName) {
  const parts = qualifiedName.split('.');
  return parts[parts.length - 1] ?? qualifiedName;
}

const src = readFileSync(STD_PATH, 'utf8');
const statements = splitStatements(stripLineComments(src));

const stdValues = [];
const stdFunctions = [];
const namespaceOrder = [];
const seenNs = new Set();

for (const statement of statements) {
  const parsed = parseLetStatement(statement);
  if (!parsed) {
    continue;
  }
  if (!parsed.qualifiedName.startsWith('Std.')) {
    continue;
  }

  const namespace = namespaceFrom(parsed.qualifiedName);
  const name = symbolFrom(parsed.qualifiedName);

  if (!seenNs.has(namespace)) {
    seenNs.add(namespace);
    namespaceOrder.push(namespace);
  }

  if (parsed.body === '__builtin__') {
    const displayName = parsed.qualifiedName + formatTypeParams(parsed.typeParams);
    stdFunctions.push({
      qualifiedName: parsed.qualifiedName,
      namespace,
      name,
      typeParams: parsed.typeParams,
      params: parsed.params,
      returnType: parsed.returnType,
      displayName,
      displaySignature: displayName + formatParamSignature(parsed.params) + ' -> ' + parsed.returnType,
      signatureKey: displayName + formatParamSignature(parsed.params) + ':' + parsed.returnType,
      hasVariadicTail: hasVariadicTail(parsed.params),
      minimumArity: minimumArity(parsed.params),
      isTerminal: parsed.returnType === 'Never',
    });
  } else if (parsed.params.length === 0) {
    stdValues.push({
      qualifiedName: parsed.qualifiedName,
      namespace,
      name,
      valueType: parsed.returnType,
    });
  }
}

const overloadCounts = new Map();
for (const fn of stdFunctions) {
  overloadCounts.set(fn.qualifiedName, (overloadCounts.get(fn.qualifiedName) ?? 0) + 1);
}

const overloadOrdinals = new Map();
for (const fn of stdFunctions) {
  const nextOrdinal = (overloadOrdinals.get(fn.qualifiedName) ?? 0) + 1;
  overloadOrdinals.set(fn.qualifiedName, nextOrdinal);
  fn.overloadIndex = nextOrdinal;
  fn.overloadCount = overloadCounts.get(fn.qualifiedName) ?? 1;
}

const output = [
  '// AUTO-GENERATED by scripts/generate-std-catalogue.mjs from ../Std.fleaux',
  '// Do not edit by hand.',
  '',
  'export type StdParam = { name: string; type: string };',
  '',
  'export type StdValueEntry = {',
  '  qualifiedName: string;',
  '  namespace: string;',
  '  name: string;',
  '  valueType: string;',
  '};',
  '',
  'export type StdFunctionEntry = {',
  '  qualifiedName: string;',
  '  namespace: string;',
  '  name: string;',
  '  typeParams: string[];',
  '  params: StdParam[];',
  '  returnType: string;',
  '  displayName: string;',
  '  displaySignature: string;',
  '  signatureKey: string;',
  '  hasVariadicTail: boolean;',
  '  minimumArity: number;',
  '  overloadIndex: number;',
  '  overloadCount: number;',
  '  isTerminal: boolean;',
  '};',
  '',
  'export const STD_VALUES: StdValueEntry[] = ' + JSON.stringify(stdValues, null, 2) + ';',
  '',
  'export const STD_FUNCTIONS: StdFunctionEntry[] = ' + JSON.stringify(stdFunctions, null, 2) + ';',
  '',
  'export const STD_VALUES_BY_NAMESPACE: Map<string, StdValueEntry[]> = (() => {',
  '  const map = new Map<string, StdValueEntry[]>();',
  '  for (const value of STD_VALUES) {',
  '    const group = map.get(value.namespace) ?? [];',
  '    group.push(value);',
  '    map.set(value.namespace, group);',
  '  }',
  '  return map;',
  '})();',
  '',
  'export const STD_BY_NAMESPACE: Map<string, StdFunctionEntry[]> = (() => {',
  '  const map = new Map<string, StdFunctionEntry[]>();',
  '  for (const fn of STD_FUNCTIONS) {',
  '    const group = map.get(fn.namespace) ?? [];',
  '    group.push(fn);',
  '    map.set(fn.namespace, group);',
  '  }',
  '  return map;',
  '})();',
  '',
  'export const STD_NAMESPACES = ' + JSON.stringify(namespaceOrder, null, 2) + ' as const;',
  '',
].join('\n');

mkdirSync(path.dirname(OUT_PATH), { recursive: true });
writeFileSync(OUT_PATH, output);
console.log('Wrote ' + OUT_PATH);
