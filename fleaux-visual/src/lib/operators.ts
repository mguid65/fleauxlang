// The operators of the language and the Std builtin each one stands for, as in
// core/include/fleaux/frontend/operators.hpp. `-` is Std.UnaryMinus with one operand and Std.Subtract with two.
export type OperatorCategory = 'Arithmetic' | 'Comparison' | 'Logical';

export type OperatorEntry = {
  token: string;
  qualifiedName: string;
  arity: number;
  category: OperatorCategory;
};

export const OPERATOR_CATEGORIES: OperatorCategory[] = ['Arithmetic', 'Comparison', 'Logical'];

export const OPERATORS: OperatorEntry[] = [
  { token: '+', qualifiedName: 'Std.Add', arity: 2, category: 'Arithmetic' },
  { token: '-', qualifiedName: 'Std.Subtract', arity: 2, category: 'Arithmetic' },
  { token: '-', qualifiedName: 'Std.UnaryMinus', arity: 1, category: 'Arithmetic' },
  { token: '*', qualifiedName: 'Std.Multiply', arity: 2, category: 'Arithmetic' },
  { token: '/', qualifiedName: 'Std.Divide', arity: 2, category: 'Arithmetic' },
  { token: '%', qualifiedName: 'Std.Mod', arity: 2, category: 'Arithmetic' },
  { token: '^', qualifiedName: 'Std.Pow', arity: 2, category: 'Arithmetic' },
  { token: '==', qualifiedName: 'Std.Equal', arity: 2, category: 'Comparison' },
  { token: '!=', qualifiedName: 'Std.NotEqual', arity: 2, category: 'Comparison' },
  { token: '<', qualifiedName: 'Std.LessThan', arity: 2, category: 'Comparison' },
  { token: '>', qualifiedName: 'Std.GreaterThan', arity: 2, category: 'Comparison' },
  { token: '<=', qualifiedName: 'Std.LessOrEqual', arity: 2, category: 'Comparison' },
  { token: '>=', qualifiedName: 'Std.GreaterOrEqual', arity: 2, category: 'Comparison' },
  { token: '!', qualifiedName: 'Std.Not', arity: 1, category: 'Logical' },
  { token: '&&', qualifiedName: 'Std.And', arity: 2, category: 'Logical' },
  { token: '||', qualifiedName: 'Std.Or', arity: 2, category: 'Logical' },
];

// The category of an operator token; every token belongs to one category, whatever its operand count.
export function operatorCategory(token: string): OperatorCategory | undefined {
  return OPERATORS.find((entry) => entry.token === token)?.category;
}

export function isOperatorToken(token: string): boolean {
  return OPERATORS.some((entry) => entry.token === token);
}

// The builtin an operator stands for with this many operands, or null when it takes another number.
export function resolveOperator(token: string, arity: number): OperatorEntry | null {
  return OPERATORS.find((entry) => entry.token === token && entry.arity === arity) ?? null;
}
