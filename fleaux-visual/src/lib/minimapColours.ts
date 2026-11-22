import { isTerminalStdFunc } from './functionSignatures';
import { stdBorderColour, stdFuncGroup } from './stdTheme';
import type { FleauxNodeData, LiteralValueType } from './types';

// The minimap draws each node in its border colour. The node components take their borders from Tailwind's 500
// shades; these are the same values, from tailwindcss/theme.css. Keep the two in step.
const TAILWIND_500 = {
  red: 'oklch(63.7% 0.237 25.331)',
  orange: 'oklch(70.5% 0.213 47.604)',
  amber: 'oklch(76.9% 0.188 70.08)',
  yellow: 'oklch(79.5% 0.184 86.047)',
  lime: 'oklch(76.8% 0.233 130.85)',
  green: 'oklch(72.3% 0.219 149.579)',
  emerald: 'oklch(69.6% 0.17 162.48)',
  teal: 'oklch(70.4% 0.14 182.503)',
  cyan: 'oklch(71.5% 0.143 215.221)',
  violet: 'oklch(60.6% 0.25 292.717)',
  purple: 'oklch(62.7% 0.265 303.9)',
  fuchsia: 'oklch(66.7% 0.295 322.15)',
  pink: 'oklch(65.6% 0.241 354.308)',
  rose: 'oklch(64.5% 0.246 16.439)',
};

const LITERAL_COLOURS: Record<LiteralValueType, string> = {
  Float64: TAILWIND_500.orange,
  Int64: TAILWIND_500.green,
  UInt64: TAILWIND_500.yellow,
  String: TAILWIND_500.emerald,
  Bool: TAILWIND_500.amber,
  Null: TAILWIND_500.pink,
};

// Every kind whose colour does not depend on the node's data.
const KIND_COLOURS: Record<Exclude<FleauxNodeData['kind'], 'literal' | 'stdValue' | 'stdFunc'>, string> = {
  wildcard: TAILWIND_500.cyan,
  typeDecl: TAILWIND_500.purple,
  aliasDecl: TAILWIND_500.lime,
  let: TAILWIND_500.fuchsia,
  import: TAILWIND_500.teal,
  rawSource: TAILWIND_500.amber,
  tuple: TAILWIND_500.orange,
  spread: TAILWIND_500.rose,
  userFunc: TAILWIND_500.purple,
  closure: TAILWIND_500.violet,
  callValue: TAILWIND_500.teal,
};

export function minimapColour(data: FleauxNodeData): string {
  switch (data.kind) {
    case 'literal':
      return LITERAL_COLOURS[data.valueType];
    case 'stdValue':
      return stdBorderColour(data.namespace);
    case 'stdFunc':
      return isTerminalStdFunc(data) ? TAILWIND_500.red : stdBorderColour(stdFuncGroup(data));
    default:
      return KIND_COLOURS[data.kind];
  }
}
