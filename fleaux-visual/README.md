# Fleaux Visual Editor

A node-based visual programming interface for Fleaux, built with:

- Vite + React + TypeScript
- React Flow (`@xyflow/react`) for graph editing
- Zustand + Immer for graph state
- Tailwind CSS v4 for styling

## Prerequisites

- Node.js 22 or newer (Node 26 needs `@playwright/test` 1.65 or newer, which `package.json` pins)
- npm 10+

On Windows, PowerShell's script execution policy may block the `npm` and `npx` shims; use `npm.cmd` and `npx.cmd`.

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
npm run lint
npm run build
```

The sample round trip imports every `samples/*.fleaux` file into a graph, serializes it back, and checks that both
versions print the same. It needs a built `fleaux` binary (found under `core/cmake-build-debug/bin` and similar) and
Playwright's Chromium:

```bash
npx playwright install chromium
npm run samples:roundtrip
```

Options: `--sample <name>` for one sample, `--fleaux-bin <path>` for another binary, `--keep-temp` to keep the
generated sources, and `--browser-channel msedge` (or `chrome`) to run an installed browser instead of Playwright's
Chromium. On Windows, put the MinGW runtime folder on `PATH` so `fleaux.exe` finds its DLLs.

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
- The build also ships `public/coi-serviceworker.js`. On GitHub Pages, the first secure-page load should register that service worker and automatically reload once before threaded WASM becomes available. This is expected.
- The service worker must be reachable at the deployed site root for its scope. For a repo Pages deployment, Vite's `base` must stay aligned with the repository path so `%BASE_URL%coi-serviceworker.js` resolves under that site root.

Local static-host verification without real response headers:

```bash
cd /home/matthew/CLionProjects/fleauxlang/fleaux-visual
VITE_BASE_PATH=/fleauxlang/ npm run build:pages
tmpdir="$(mktemp -d)"
mkdir -p "$tmpdir/fleauxlang"
cp -R dist/. "$tmpdir/fleauxlang/"
python3 -m http.server 4173 --directory "$tmpdir"
```

Then open `http://127.0.0.1:4173/fleauxlang/`. The page should reload once after registering the service worker.

## Current Features

- Import Fleaux source into a graph and serialize a graph back to source. Statements keep their source order; a
  statement the graph cannot represent (currently blocks) stays as a raw source node with the reason.
- Node kinds:
  - declarations: `import`, `type`, `alias`, `let` (a function, or a value let `let Name: Type = expr`)
  - values: literals (`String`, `Int64`, `UInt64`, `Float64`, `Bool`, `Null`), Std constants, tuples
  - calls: Std builtins, operators (`+`, `-`, `!`, ...), user functions, and call value nodes for calling a parameter,
    value let, or closure
  - `closure` (generic closures included), `spread` (`x...`), and raw source
- A call node has one argument slot per parameter and serializes as the parameter list of its slots; a variadic tail
  has as many slots as you set with +/-. A spread may supply several arguments, so the slots after it may stay empty.
- An operator call keeps its operator, so an operator on a strong type still reaches the type's operator function.
- The connection guard rejects self-loops, duplicate edges, edges to or from declarations and raw source, and spread
  outputs that do not go into a call's argument slot.
- The Std toolbar is generated from `stdlib/Std.fleaux`.
- The action bar opens any of the repository's `samples/*.fleaux` files, loads and saves `.fleaux` files, and has undo
  and redo (Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y) and Tidy, which lays the graph out again. An imported graph is laid out
  automatically, left to right.
- The open graph is kept in the browser's local storage, so a reload brings it back.
- Validation and runs go through the WASM coordinator built from `core`.

## Project Structure

- `src/components/Canvas.tsx` - main React Flow surface
- `src/components/Toolbar.tsx` - node creation controls
- `src/components/ActionBar.tsx` - samples, file, history, and layout actions
- `src/store/flowStore.ts` - Zustand graph state/actions
- `src/store/initialGraph.ts` - starter demo graph
- `src/lib/fleauxToGraph.ts` - Fleaux source to graph import
- `src/lib/graphToFleaux.ts` - graph to Fleaux source serialization
- `src/lib/edgeValidation.ts` - pipeline edge validation rules
- `src/lib/autoLayout.ts` - the layered left-to-right layout
- `src/lib/graphStorage.ts` - keeping the graph in local storage
- `src/lib/samples.ts` - the bundled `samples/*.fleaux` files
- `src/lib/operators.ts` - the operators and the Std builtins they stand for
- `src/lib/functionValues.ts` - function types of called values
- `src/lib/stdCatalogue.ts` - generated from `stdlib/Std.fleaux` by `scripts/generate-std-catalogue.mjs`
- `src/nodes/*` - Fleaux node renderers
- `src/lib/types.ts` - Fleaux node and edge data model
- `scripts/roundtrip-samples.mjs` - the sample round trip

## Next Steps

- Visual blocks (`{ ... }`), which import as raw source today
- Drawable body markers for lets and closures; a let built in the editor finds its body from its parameters, and a
  closure built in the editor has no body yet
- Save/load (JSON project files)
- Node inspectors and form editing for node data
