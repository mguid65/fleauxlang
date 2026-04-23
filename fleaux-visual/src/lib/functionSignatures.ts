export type SignatureParam = {
  name: string;
  type: string;
};

export type SignatureShape = {
  params: SignatureParam[];
  returnType: string;
  typeParams?: string[];
};

export function formatTypeParams(typeParams: string[] | undefined): string {
  if (!typeParams || typeParams.length === 0) {
    return '';
  }
  return '<' + typeParams.join(', ') + '>';
}

export function formatParamSignature(params: SignatureParam[]): string {
  if (params.length === 0) {
    return '()';
  }
  return '(' + params.map((param) => param.name + ': ' + param.type).join(', ') + ')';
}

export function formatFunctionDisplayName(baseName: string, typeParams: string[] | undefined): string {
  return baseName + formatTypeParams(typeParams);
}

export function formatCompactFunctionLabel(baseName: string, shape: SignatureShape): string {
  return formatFunctionDisplayName(baseName, shape.typeParams) + formatParamSignature(shape.params);
}

export function formatFunctionDisplaySignature(baseName: string, shape: SignatureShape): string {
  return formatCompactFunctionLabel(baseName, shape) + ' -> ' + shape.returnType;
}

export function formatQualifiedFunctionKey(qualifiedName: string, shape: SignatureShape): string {
  return formatFunctionDisplayName(qualifiedName, shape.typeParams)
    + formatParamSignature(shape.params)
    + ':'
    + shape.returnType;
}

export function hasVariadicTail(params: SignatureParam[]): boolean {
  if (params.length === 0) {
    return false;
  }
  return params[params.length - 1].type.trim().endsWith('...');
}

export function minArity(params: SignatureParam[]): number {
  return hasVariadicTail(params) ? Math.max(0, params.length - 1) : params.length;
}

export function matchesArity(params: SignatureParam[], arity: number): boolean {
  if (hasVariadicTail(params)) {
    return arity >= minArity(params);
  }
  return arity === params.length;
}

export function formatAritySummary(params: SignatureParam[]): string {
  if (hasVariadicTail(params)) {
    return `${minArity(params)}+ args`;
  }
  return `${params.length} arg${params.length === 1 ? '' : 's'}`;
}

export function isTerminalReturnType(returnType: string): boolean {
  return returnType.trim() === 'Never';
}

