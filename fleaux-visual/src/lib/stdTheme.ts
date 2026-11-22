import type { CSSProperties } from 'react';
import { OPERATOR_CATEGORIES, operatorCategory } from './operators';
import { STD_NAMESPACES } from './stdCatalogue';
import type { StdFuncData } from './types';

// Every Std namespace and every operator category has its own colour. The hues run around the colour wheel in the
// toolbar's order, from the operators down to the last namespace, so the namespaces of one area (Std.String and
// Std.String.Regex, say) get neighbouring hues. Red is left out: it marks terminal functions (return type Never).

// The toolbar's order of the namespaces, grouped by area. A namespace missing here goes at the end.
const NAMESPACE_ORDER: readonly string[] = [
  // Core
  'Std',
  // Numbers
  'Std.Math',
  'Std.Bit',
  'Std.Random',
  // Text
  'Std.String',
  'Std.String.Regex',
  // Data
  'Std.Tuple',
  'Std.Array',
  'Std.Dict',
  // Effects
  'Std.Result',
  'Std.Task',
  'Std.Parallel',
  // System
  'Std.OS',
  'Std.Path',
  'Std.File',
  'Std.Dir',
];

function orderRank(namespace: string): number {
  const rank = NAMESPACE_ORDER.indexOf(namespace);
  return rank < 0 ? NAMESPACE_ORDER.length : rank;
}

// The Std namespaces in the toolbar's order.
export const STD_NAMESPACES_IN_ORDER: readonly string[] = [...STD_NAMESPACES].sort(
  (lhs, rhs) => orderRank(lhs) - orderRank(rhs),
);

// OKLCH hues, from orange round to pink; red sits at about 25 degrees.
const FIRST_HUE = 50;
const LAST_HUE = 345;

const PALETTE_ORDER: readonly string[] = [...OPERATOR_CATEGORIES, ...STD_NAMESPACES_IN_ORDER];
const HUES = new Map(
  PALETTE_ORDER.map((group, index) => [group, FIRST_HUE + (index * (LAST_HUE - FIRST_HUE)) / (PALETTE_ORDER.length - 1)]),
);

// The hue of a Std namespace or an operator category. A namespace outside the catalogue takes its parent's hue.
function hueOf(group: string): number {
  for (let name = group; name.length > 0; name = name.slice(0, Math.max(0, name.lastIndexOf('.')))) {
    const hue = HUES.get(name);
    if (hue !== undefined) {
      return hue;
    }
  }
  return FIRST_HUE;
}

function oklch(lightness: number, chroma: number, hue: number): string {
  return `oklch(${lightness} ${chroma} ${hue.toFixed(1)})`;
}

// The colour group of a Std function node: an operator node's operator category, any other node's namespace.
export function stdFuncGroup(data: Pick<StdFuncData, 'operator' | 'namespace'>): string {
  return (data.operator === undefined ? undefined : operatorCategory(data.operator)) ?? data.namespace;
}

// The border colour of a node on the canvas, which the minimap draws it in. `group` is a Std namespace or an operator
// category.
export function stdBorderColour(group: string): string {
  return oklch(0.72, 0.14, hueOf(group));
}

// The colours of a node on the canvas.
export function stdNodeStyle(group: string): CSSProperties {
  const hue = hueOf(group);
  return { borderColor: stdBorderColour(group), backgroundColor: oklch(0.27, 0.06, hue), color: oklch(0.9, 0.06, hue) };
}

// The colours of a toolbar entry with a border: a dropdown, or a filter result.
export function stdEntryStyle(group: string): CSSProperties {
  const hue = hueOf(group);
  return { borderColor: oklch(0.52, 0.11, hue), color: oklch(0.86, 0.09, hue) };
}

// The text colour of an item in an open toolbar dropdown.
export function stdTextStyle(group: string): CSSProperties {
  return { color: oklch(0.86, 0.09, hueOf(group)) };
}
