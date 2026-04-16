# Fleaux Visual Editor

A node-based visual programming interface for Fleaux, built with:

- Vite + React + TypeScript
- React Flow (`@xyflow/react`) for graph editing
- Zustand + Immer for graph state
- Tailwind CSS v4 for styling

## Prerequisites

- Node.js 22.x (recommended)
- npm 10+

If Node is not installed, this project works well with `fnm`:

```bash
curl -fsSL https://fnm.vercel.app/install | bash -s -- --install-dir "$HOME/.local/share/fnm" --skip-shell
export PATH="$HOME/.local/share/fnm:$PATH"
eval "$(fnm env --use-on-cd --shell bash)"
fnm install 22
fnm use 22
```

## Install

```bash
cd /home/matthew/CLionProjects/fleauxlang/fleaux-visual
npm install
```

## Run (dev)

```bash
cd /home/matthew/CLionProjects/fleauxlang/fleaux-visual
npm run dev
```

## Validate

```bash
cd /home/matthew/CLionProjects/fleauxlang/fleaux-visual
npm run typecheck
npm run build
```

## Current Features

- Drag/drop node graph canvas
- Connect/disconnect typed pipeline edges (`data.kind = "pipeline"`)
- Connection guard rejects:
  - self-loops
  - duplicate `source -> target` edges
  - any edge where source or target node is `import`
- Starter Fleaux node types:
  - `import`
  - `let`
  - `tuple`
  - `std` (explicit Std namespace nodes)
  - `literal` (`String` / `Number` / `Bool` / `Null`)
- Std namespace nodes (from `Std.fleaux`) available in toolbar:
  - `Std`, `Std.Exp`, `Std.String`, `Std.String.Regex`, `Std.Tuple`, `Std.Math`, `Std.Path`, `Std.File`, `Std.Dir`, `Std.OS`, `Std.Dict`
- Quick add toolbar for creating nodes
- Dark theme suitable for large graph editing

## Project Structure

- `src/components/Canvas.tsx` - main React Flow surface
- `src/components/Toolbar.tsx` - node creation controls
- `src/store/flowStore.ts` - Zustand graph state/actions
- `src/store/initialGraph.ts` - starter demo graph
- `src/lib/edgeValidation.ts` - pipeline edge validation rules
- `src/nodes/*` - Fleaux node renderers
- `src/lib/types.ts` - Fleaux node and edge data model

## Next Steps

- Add graph -> Fleaux source code generation
- Add Fleaux source -> graph import (parser bridge)
- Add save/load (JSON project files)
- Add node inspectors and form editing for node data
