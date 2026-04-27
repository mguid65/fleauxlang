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

## GitHub Pages deployment

This app is set up for manual deployment to a `gh-pages` branch.

Expected default URL for a repository named `fleauxlang`:

- `https://<your-user>.github.io/fleauxlang/`

The build uses Vite's `base` setting via `VITE_BASE_PATH`, so repo Pages and user/org Pages can both work:

- repo Pages example: `VITE_BASE_PATH=/fleauxlang/`
- user/org Pages example: `VITE_BASE_PATH=/`

Local Pages-style build test:

```bash
cd /home/matthew/CLionProjects/fleauxlang/fleaux-visual
VITE_BASE_PATH=/fleauxlang/ npm run build:pages
```

Repository setup:

1. Push the repository to GitHub.
2. In GitHub, open `Settings -> Pages`.
3. Set `Source` to `Deploy from a branch`.
4. Select branch `gh-pages` and folder `/ (root)`.

Deploy manually from your local clone:

```bash
cd /home/matthew/CLionProjects/fleauxlang/fleaux-visual
npm run deploy:gh-pages
```

Dry-run mode (build + prepare commit, no push):

```bash
cd /home/matthew/CLionProjects/fleauxlang/fleaux-visual
bash scripts/deploy-gh-pages.sh --dry-run
```

Important note:

- The deployed site serves the committed assets in `public/`, including `public/wasm/`. If you rebuild the WASM coordinator locally, commit the updated files before deploying.

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
    - `literal` (`String` / `Int64|UInt64|Float64` / `Bool` / `Null`)
- Std namespace nodes (from `stdlib/Std.fleaux`) available in toolbar:
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
