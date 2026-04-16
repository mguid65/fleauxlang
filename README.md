# Fleaux

Fleaux is a functional, pipeline-oriented language with a C++ frontend, bytecode runtime, and optional web visual editor.

This repository contains:

- `core/`: parser, lowering, transpiler, bytecode compiler, VM, and CLIs
- `samples/`: runnable `.fleaux` examples
- `Std.fleaux`: standard library surface (builtins and helpers)
- `fleaux-visual/`: React + TypeScript visual editor and WASM integration

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

let Square(x: Number): Number = (x, x) -> Std.Multiply;
let Average(a: Number, b: Number): Number =
    (a, b) -> Std.Add -> (_, 2) -> Std.Divide;

(6) -> Square -> Std.Println;
(10, 20) -> Average -> Std.Println;
```

### Imports

`lib.fleaux`

```fleaux
import Std;

let Add4(x: Number): Number = (4, x) -> Std.Add;
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

```bash
cd /home/matthew/CLionProjects/fleauxlang/core
conan profile detect --force
conan install . -s build_type=Debug -of=cmake-build-debug --build=missing
source cmake-build-debug/generators/conanbuild.sh
cmake -S . -B cmake-build-debug -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=cmake-build-debug/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug --parallel
ctest --test-dir cmake-build-debug --output-on-failure
```

Build outputs are placed under:

- `core/cmake-build-debug/bin/` (executables)
- `core/cmake-build-debug/lib/` (libraries)

Main executables:

- `fleaux_transpile_cli`
- `fleaux_vm_cli` (available when PCRE2 target resolves)
- `fleaux_core_tests`

### Running the CLIs

```bash
cd /home/matthew/CLionProjects/fleauxlang/core
source cmake-build-debug/generators/conanrun.sh
./cmake-build-debug/bin/fleaux_vm_cli ../samples/01_hello_world.fleaux
./cmake-build-debug/bin/fleaux_vm_cli --mode interpreter ../samples/04_function_definitions.fleaux
./cmake-build-debug/bin/fleaux_vm_cli --all-samples
./cmake-build-debug/bin/fleaux_transpile_cli ../samples/01_hello_world.fleaux
```

Useful VM CLI options:

- `--mode bytecode|interpreter`
- `--repl`
- `--all-samples`
- `--no-run`
- `--` to forward runtime args to the program

Show help:

```bash
./cmake-build-debug/bin/fleaux_vm_cli --help
./cmake-build-debug/bin/fleaux_transpile_cli --help
```

## Release build, install, and package

```bash
cd /home/matthew/CLionProjects/fleauxlang/core
conan install . -s build_type=Release -of=cmake-build-release --build=missing
source cmake-build-release/generators/conanbuild.sh
cmake -S . -B cmake-build-release -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=cmake-build-release/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --parallel
cmake --install cmake-build-release --prefix /tmp/fleaux-install
cpack --config cmake-build-release/CPackConfig.cmake
```

By default, CPack produces a `.tar.gz` package in `core/cmake-build-release/packages/`.

## Optional: build WASM coordinator and visual editor

### Build WASM coordinator used by the visual editor

```bash
cd /home/matthew/CLionProjects/fleauxlang/fleaux-visual
npm install
npm run wasm:configure
npm run wasm:build
```

This generates:

- `fleaux-visual/public/wasm/fleaux_wasm_coordinator.js`
- `fleaux-visual/public/wasm/fleaux_wasm_coordinator.wasm`

Alternative script:

```bash
cd /home/matthew/CLionProjects/fleauxlang
bash fleaux-visual/wasm/build_wasm.sh
```

### Run visual editor

```bash
cd /home/matthew/CLionProjects/fleauxlang/fleaux-visual
npm run dev
```

## Try the sample set

The `samples/` directory contains numbered examples from hello world to imports, loops, tuples, dictionaries, error handling, and experimental parallel helpers.

A quick smoke run after building:

```bash
cd /home/matthew/CLionProjects/fleauxlang/core
source cmake-build-debug/generators/conanrun.sh
./cmake-build-debug/bin/fleaux_vm_cli --all-samples
```

