import type { Node, XYPosition } from '@xyflow/react';
import type { FleauxEdge, FleauxNodeData } from './types';

// A left-to-right layered layout. Every connected part of the graph is laid out in columns that follow its edges. The
// parts then run top to bottom in the order of their first node, which is source order, wrapping into further columns
// like a newspaper so that a long program fills a screen-shaped area. Lone nodes, such as `import Std;` or a type
// declaration, share rows.

const COLUMN_GAP = 72;
const ROW_GAP = 24;
const PART_GAP = 64;
const ROW_WIDTH = 1200;
// Width over height of the area the parts wrap into.
const ASPECT_RATIO = 1.6;
const FALLBACK_WIDTH = 180;
const FALLBACK_HEIGHT = 80;

// One or more parts placed together: a part with edges, or a row of lone nodes.
interface Block {
  width: number;
  height: number;
  members: { part: number[]; dx: number }[];
}

interface LayoutItem {
  width: number;
  height: number;
  preds: number[];
  succs: number[];
  layer: number;
  order: number;
  x: number;
  y: number;
}

function average(values: number[]): number {
  return values.reduce((sum, value) => sum + value, 0) / values.length;
}

function unlink(items: LayoutItem[], from: number, to: number): void {
  items[from].succs = items[from].succs.filter((succ) => succ !== to);
  items[to].preds = items[to].preds.filter((pred) => pred !== from);
}

// The connected parts, each as ascending node indices, in the order of their first node.
function connectedParts(items: LayoutItem[]): number[][] {
  const partOf = new Array<number>(items.length).fill(-1);
  const parts: number[][] = [];
  items.forEach((_, start) => {
    if (partOf[start] !== -1) {
      return;
    }
    const part: number[] = [];
    const stack = [start];
    partOf[start] = parts.length;
    while (stack.length > 0) {
      const index = stack.pop()!;
      part.push(index);
      for (const next of [...items[index].preds, ...items[index].succs]) {
        if (partOf[next] === -1) {
          partOf[next] = parts.length;
          stack.push(next);
        }
      }
    }
    parts.push(part.sort((lhs, rhs) => lhs - rhs));
  });
  return parts;
}

// Drops the edges that close a cycle, so that the part is a DAG.
function breakCycles(items: LayoutItem[], part: number[]): void {
  const state = new Map<number, 'open' | 'done'>();
  const visit = (index: number) => {
    state.set(index, 'open');
    for (const succ of [...items[index].succs]) {
      const succState = state.get(succ);
      if (succState === 'open') {
        unlink(items, index, succ);
      } else if (succState === undefined) {
        visit(succ);
      }
    }
    state.set(index, 'done');
  };
  for (const index of part) {
    if (!state.has(index)) {
      visit(index);
    }
  }
}

function topologicalOrder(items: LayoutItem[], part: number[]): number[] {
  const remaining = new Map(part.map((index) => [index, items[index].preds.length]));
  const ready = part.filter((index) => remaining.get(index) === 0);
  const order: number[] = [];
  while (ready.length > 0) {
    ready.sort((lhs, rhs) => lhs - rhs);
    const index = ready.shift()!;
    order.push(index);
    for (const succ of items[index].succs) {
      const left = remaining.get(succ)! - 1;
      remaining.set(succ, left);
      if (left === 0) {
        ready.push(succ);
      }
    }
  }
  return order;
}

// Columns: each node goes one column right of its furthest predecessor. A node without predecessors, such as a
// literal, then moves right to sit just before its nearest successor.
function assignLayers(items: LayoutItem[], order: number[]): number[][] {
  for (const index of order) {
    items[index].layer = Math.max(0, ...items[index].preds.map((pred) => items[pred].layer + 1));
  }
  for (const index of [...order].reverse()) {
    const item = items[index];
    if (item.preds.length === 0 && item.succs.length > 0) {
      item.layer = Math.min(...item.succs.map((succ) => items[succ].layer)) - 1;
    }
  }
  const layers: number[][] = [];
  for (const index of [...order].sort((lhs, rhs) => lhs - rhs)) {
    (layers[items[index].layer] ??= []).push(index);
  }
  return layers.filter((layer) => layer !== undefined);
}

// Orders each column by the average position of the node's neighbours, to cut edge crossings.
function orderLayers(items: LayoutItem[], layers: number[][]): void {
  const sortLayer = (layer: number[], neighbours: (item: LayoutItem) => number[]) => {
    const keys = new Map(
      layer.map((index) => {
        const around = neighbours(items[index]);
        return [index, around.length > 0 ? average(around.map((other) => items[other].order)) : items[index].order];
      }),
    );
    layer.sort((lhs, rhs) => keys.get(lhs)! - keys.get(rhs)! || lhs - rhs);
    layer.forEach((index, order) => {
      items[index].order = order;
    });
  };
  for (const layer of layers) {
    layer.forEach((index, order) => {
      items[index].order = order;
    });
  }
  for (let sweep = 0; sweep < 4; sweep += 1) {
    for (let column = 1; column < layers.length; column += 1) {
      sortLayer(layers[column], (item) => item.preds);
    }
    for (let column = layers.length - 2; column >= 0; column -= 1) {
      sortLayer(layers[column], (item) => item.succs);
    }
  }
}

// Places the columns, then moves each node level with its neighbours without letting nodes of a column overlap.
function placePart(items: LayoutItem[], layers: number[][]): { width: number; height: number } {
  let x = 0;
  for (const layer of layers) {
    for (const index of layer) {
      items[index].x = x;
    }
    x += Math.max(...layer.map((index) => items[index].width)) + COLUMN_GAP;
  }

  const centre = (index: number) => items[index].y + items[index].height / 2;
  const align = (layer: number[], desiredTop: (item: LayoutItem) => number) => {
    let bottom = Number.NEGATIVE_INFINITY;
    for (const index of layer) {
      const item = items[index];
      item.y = Math.max(desiredTop(item), bottom + ROW_GAP);
      bottom = item.y + item.height;
    }
  };
  const towardsPreds = (item: LayoutItem) =>
    item.preds.length > 0 ? average(item.preds.map(centre)) - item.height / 2 : item.y;
  const sourcesTowardsSuccs = (item: LayoutItem) =>
    item.preds.length === 0 && item.succs.length > 0 ? average(item.succs.map(centre)) - item.height / 2 : item.y;

  for (const layer of layers) {
    let y = 0;
    for (const index of layer) {
      items[index].y = y;
      y += items[index].height + ROW_GAP;
    }
  }
  for (const layer of layers) {
    align(layer, towardsPreds);
  }
  for (const layer of [...layers].reverse()) {
    align(layer, sourcesTowardsSuccs);
  }
  for (const layer of layers) {
    align(layer, towardsPreds);
  }

  const part = layers.flat();
  const top = Math.min(...part.map((index) => items[index].y));
  for (const index of part) {
    items[index].y -= top;
  }
  return {
    width: Math.max(...part.map((index) => items[index].x + items[index].width)),
    height: Math.max(...part.map((index) => items[index].y + items[index].height)),
  };
}

// New positions for every node. Sizes come from React Flow's measurements, so lay out after the nodes have rendered.
export function autoLayout(nodes: Node<FleauxNodeData>[], edges: FleauxEdge[]): Map<string, XYPosition> {
  const indexById = new Map(nodes.map((node, index) => [node.id, index]));
  const items: LayoutItem[] = nodes.map((node) => ({
    width: node.measured?.width ?? FALLBACK_WIDTH,
    height: node.measured?.height ?? FALLBACK_HEIGHT,
    preds: [],
    succs: [],
    layer: 0,
    order: 0,
    x: 0,
    y: 0,
  }));
  for (const edge of edges) {
    const from = indexById.get(edge.source);
    const to = indexById.get(edge.target);
    if (from === undefined || to === undefined || from === to || items[from].succs.includes(to)) {
      continue;
    }
    items[from].succs.push(to);
    items[to].preds.push(from);
  }

  const positions = new Map<string, XYPosition>();
  const offsetPart = (part: number[], dx: number, dy: number) => {
    for (const index of part) {
      positions.set(nodes[index].id, { x: items[index].x + dx, y: items[index].y + dy });
    }
  };

  const blocks: Block[] = [];
  let row: Block | null = null;
  for (const part of connectedParts(items)) {
    breakCycles(items, part);
    const layers = assignLayers(items, topologicalOrder(items, part));
    orderLayers(items, layers);
    const size = placePart(items, layers);

    if (part.length > 1) {
      row = null;
      blocks.push({ ...size, members: [{ part, dx: 0 }] });
    } else if (row !== null && row.width + COLUMN_GAP + size.width <= ROW_WIDTH) {
      row.members.push({ part, dx: row.width + COLUMN_GAP });
      row.width += COLUMN_GAP + size.width;
      row.height = Math.max(row.height, size.height);
    } else {
      row = { ...size, members: [{ part, dx: 0 }] };
      blocks.push(row);
    }
  }
  if (blocks.length === 0) {
    return positions;
  }

  const area = blocks.reduce((sum, block) => sum + (block.width + COLUMN_GAP) * (block.height + PART_GAP), 0);
  const columnHeight = Math.max(...blocks.map((block) => block.height), Math.sqrt(area / ASPECT_RATIO));
  let columnX = 0;
  let columnWidth = 0;
  let y = 0;
  for (const block of blocks) {
    if (y > 0 && y + block.height > columnHeight) {
      columnX += columnWidth + 2 * COLUMN_GAP;
      columnWidth = 0;
      y = 0;
    }
    for (const member of block.members) {
      offsetPart(member.part, columnX + member.dx, y);
    }
    columnWidth = Math.max(columnWidth, block.width);
    y += block.height + PART_GAP;
  }
  return positions;
}
