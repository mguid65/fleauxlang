# Fleaux

Fleaux is a functional, pipeline-oriented language with a C++ frontend, VM runtime, and optional web visual editor.

This repository contains:

- `core/`: parser, analysis/lowering, VM compiler, VM, and CLIs
- `samples/`: runnable `.fleaux` examples
- `Std.fleaux`: standard library surface (builtins and helpers)
- `fleaux-visual/`: React + TypeScript visual editor and WASM integration

## Documentation

- Developer feature routing map: `docs/developer_feature_map.md`
- Third-party notices: `THIRD_PARTY_NOTICES.md`

## Basic language usage

Fleaux programs are expression-based and commonly use the pipeline operator `->`.

### Hello world

```fleaux
import Std;

("Hello, World!") -> Std.Println;
("Hello", "from", "Fleaux") -> Std.Println;
```

### Function definitions and calls

```fleaux
import Std;

let Square(x: Float64): Float64 = (x, x) -> Std.Multiply;
let Average(a: Float64, b: Float64): Float64 =
    (a, b) -> Std.Add -> (_, 2) -> Std.Divide;

(6) -> Square -> Std.Println;
(10, 20) -> Average -> Std.Println;
```

### Imports

`lib.fleaux`

```fleaux
import Std;

let Add4(x: Float64): Float64 = (4, x) -> Std.Add;
```

`main.fleaux`

```fleaux
import lib;

(4) -> Add4 -> Std.Println;
```

## Build prerequisites

For native builds (`core/`):

- CMake 3.20+
- A C++20 compiler (GCC, Clang, or MSVC)
- Python 3 (for Conan)
- Conan 2.x

Notes:

- `pcre2` and `catch2` are pulled by Conan from `core/conandata.yml`.
- VM targets require PCRE2 (handled through Conan in the instructions below).

For optional visual/WASM builds (`fleaux-visual/`):

- Node.js 22.x and npm 10+
- Emscripten SDK (`emcc` on `PATH`)

## Native build instructions (core)

These steps build all native CLI targets and run tests.

From the repository root:

```bash
cd core
mkdir -p build
conan profile detect --force
conan install . -s build_type=Debug -of=build --build=missing
source build/generators/conanbuild.sh
cmake -S . -B build -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=build/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Build outputs are placed under:

- `core/build/bin/` (executables)
- `core/build/lib/` (libraries)

Main executables:

- `fleaux` (available when PCRE2 target resolves)
- `fleaux_core_tests`

### Running the CLIs

```bash
cd core
source build/generators/conanrun.sh
./build/bin/fleaux ../samples/01_hello_world.fleaux
./build/bin/fleaux --repl
```

Useful VM CLI options:

- `--repl`
- `--no-run`
- `--no-emit-bytecode` to disable default `.fleaux.bc` cache writes
- `--` to forward runtime args to the program

Batch sample execution is handled by `run_samples.py`.

Show help:

```bash
./build/bin/fleaux --help
```

## Release build, install, and package

```bash
cd core
mkdir -p build-release
conan install . -s build_type=Release -of=build-release --build=missing
source build-release/generators/conanbuild.sh
cmake -S . -B build-release -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=build-release/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
cmake --install build-release --prefix /tmp/fleaux-install
cpack --config build-release/CPackConfig.cmake
```

By default, CPack produces a `.tar.gz` package in `core/build-release/packages/`.

## Optional: build WASM coordinator and visual editor

### Build WASM coordinator used by the visual editor

```bash
cd fleaux-visual
npm install
npm run wasm:configure
npm run wasm:build
```

This generates:

- `fleaux-visual/public/wasm/fleaux_wasm_coordinator.js`
- `fleaux-visual/public/wasm/fleaux_wasm_coordinator.wasm`

Alternative script:

```bash
cd .
bash fleaux-visual/wasm/build_wasm.sh
```

### Run visual editor

```bash
cd fleaux-visual
npm run dev
```

### Deploy visual editor to GitHub Pages

```bash
cd fleaux-visual
npm run deploy:gh-pages
```

To test deployment without pushing:

```bash
cd fleaux-visual
bash scripts/deploy-gh-pages.sh --dry-run
```

To test the same base path locally before pushing:

```bash
cd fleaux-visual
VITE_BASE_PATH=/fleauxlang/ npm run build:pages
```

## Try the sample set

The `samples/` directory contains numbered examples from hello world to imports, loops, tuples, dictionaries, error handling, and experimental parallel helpers.

A quick smoke run after building:

```bash
cd core
source build/generators/conanrun.sh
cd ..
python3 run_samples.py --mode vm

# Target a sample that expects argv, such as the parser sample.
python3 run_samples.py --mode vm --sample 25_fleaux_parser.fleaux
```

## Acknowledgements

- `tl::expected` for lightweight `expected` support in the C++ implementation.
- `PCRE2` for regex support in the runtime and tooling.
- `Catch2` for the native test suite.
