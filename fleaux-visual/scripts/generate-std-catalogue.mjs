import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const STD_PATH = path.resolve(__dirname, '../../stdlib/Std.fleaux');
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

// A '>' closes a type argument list unless it ends '=>' (function types) or '->' (pipelines).
function isClosingAngle(input, index) {
  return input[index] === '>' && input[index - 1] !== '=' && input[index - 1] !== '-';
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
    if (isClosingAngle(source, index)) {
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
  for (let index = 0; index < input.length; index += 1) {
    const ch = input[index];
    if (ch === '(') depthParen += 1;
    if (ch === ')') depthParen -= 1;
    if (ch === '<') depthAngle += 1;
    if (isClosingAngle(input, index)) depthAngle = Math.max(0, depthAngle - 1);
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
    else if (isClosingAngle(input, index)) depthAngle = Math.max(0, depthAngle - 1);
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
  const open = trimmed.indexOf('<');
  if (open === -1) {
    return { qualifiedName: trimmed, typeParams: [] };
  }

  const close = findMatchingAngle(trimmed, open);
  if (close !== trimmed.length - 1) {
    throw new Error('Malformed generic parameter list in "' + trimmed + '"');
  }

  const qualifiedName = trimmed.slice(0, open).trim();
  const typeParamsText = trimmed.slice(open + 1, close).trim();
  const typeParams = typeParamsText.length === 0 ? [] : splitTopLevel(typeParamsText, ',').map(normalizeTypeParam);
  return { qualifiedName, typeParams };
}

function findMatchingAngle(input, openIndex) {
  let depth = 0;
  for (let index = openIndex; index < input.length; index += 1) {
    if (input[index] === '<') {
      depth += 1;
    } else if (isClosingAngle(input, index)) {
      depth -= 1;
      if (depth === 0) {
        return index;
      }
    }
  }
  return -1;
}

// Generic parameters are kept as display text, e.g. 'T' or 'T: Numeric' or 'T: Int64 | UInt64'.
function normalizeTypeParam(raw) {
  const colon = raw.indexOf(':');
  if (colon === -1) {
    return raw.trim();
  }
  return raw.slice(0, colon).trim() + ': ' + raw.slice(colon + 1).trim();
}

// type Name<T, E> :: __builtin__;  or  type Name = Target;
function parseTypeStatement(statement) {
  const match = /^type\s+([A-Za-z_][\w]*(?:\s*<.*?>)?)\s*(::|=)\s*(.+);$/.exec(statement);
  if (!match) {
    return null;
  }
  const { qualifiedName: name, typeParams } = splitQualifiedNameAndTypeParams(match[1]);
  return { name, typeParams, separator: match[2], target: match[3].trim() };
}

// alias Name = Target;
function parseAliasStatement(statement) {
  const match = /^alias\s+([A-Za-z_][\w]*)\s*=\s*(.+);$/.exec(statement);
  if (!match) {
    return null;
  }
  return { name: match[1], target: match[2].trim() };
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
const stdTypes = [];
const stdAliases = [];
const namespaceOrder = [];
const seenNs = new Set();
const unparsed = [];

for (const statement of statements) {
  if (/^import\s/.test(statement)) {
    continue;
  }
  if (statement.startsWith('type ')) {
    const typeDecl = parseTypeStatement(statement);
    if (!typeDecl) {
      unparsed.push(statement);
      continue;
    }
    stdTypes.push({
      name: typeDecl.name,
      typeParams: typeDecl.typeParams,
      isBuiltin: typeDecl.separator === '::' && typeDecl.target === '__builtin__',
      target: typeDecl.target,
      displayName: typeDecl.name + formatTypeParams(typeDecl.typeParams),
    });
    continue;
  }
  if (statement.startsWith('alias ')) {
    const aliasDecl = parseAliasStatement(statement);
    if (!aliasDecl) {
      unparsed.push(statement);
      continue;
    }
    stdAliases.push({ name: aliasDecl.name, target: aliasDecl.target });
    continue;
  }

  const parsed = statement.startsWith('let ') ? parseLetStatement(statement) : null;
  if (!parsed) {
    unparsed.push(statement);
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

if (unparsed.length > 0) {
  console.error('Could not parse ' + unparsed.length + ' statement(s) in ' + STD_PATH + ':');
  for (const statement of unparsed) {
    console.error('  ' + statement);
  }
  process.exit(1);
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
  '// AUTO-GENERATED by scripts/generate-std-catalogue.mjs from ../stdlib/Std.fleaux',
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
  'export type StdTypeEntry = {',
  '  name: string;',
  '  typeParams: string[];',
  '  isBuiltin: boolean;',
  '  target: string;',
  '  displayName: string;',
  '};',
  '',
  'export type StdAliasEntry = {',
  '  name: string;',
  '  target: string;',
  '};',
  '',
  'export const STD_TYPES: StdTypeEntry[] = ' + JSON.stringify(stdTypes, null, 2) + ';',
  '',
  'export const STD_ALIASES: StdAliasEntry[] = ' + JSON.stringify(stdAliases, null, 2) + ';',
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
