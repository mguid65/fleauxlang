#!/bin/bash
# Build script for Fleaux WASM coordinator
# Usage: ./build_wasm.sh [clean]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CORE_DIR="$PROJECT_ROOT/core"
WASM_DIR="$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if clean flag is passed
CLEAN_BUILD=false
if [[ "$1" == "clean" ]]; then
    CLEAN_BUILD=true
fi

echo -e "${YELLOW}=== Fleaux WASM Coordinator Build ===${NC}"

# Step 1: Verify Emscripten is available
echo -e "${YELLOW}[1/4] Verifying Emscripten SDK...${NC}"
if ! command -v emcc &> /dev/null; then
    echo -e "${RED}Error: emcc not found. Please install and activate Emscripten SDK.${NC}"
    echo "       Visit: https://emscripten.org/docs/getting_started/downloads.html"
    exit 1
fi

EMCC_VERSION=$(emcc --version | head -n1)
echo -e "${GREEN}✓ Found: $EMCC_VERSION${NC}"

# Step 2: Verify Conan is available
echo -e "${YELLOW}[2/4] Verifying Conan package manager...${NC}"
if ! command -v conan &> /dev/null; then
    echo -e "${RED}Error: conan not found. Please install Conan 2.0 or later.${NC}"
    echo "       Visit: https://conan.io/download"
    exit 1
fi

CONAN_VERSION=$(conan --version)
echo -e "${GREEN}✓ Found: $CONAN_VERSION${NC}"

# Step 3: Clean if requested, then install Conan deps
if [[ "$CLEAN_BUILD" == "true" ]]; then
    echo -e "${YELLOW}[3/4] Cleaning previous build...${NC}"
    rm -rf "$CORE_DIR/cmake-build-wasm"
    rm -rf "$CORE_DIR/conan_build_wasm"
    echo -e "${GREEN}✓ Clean complete${NC}"
fi

echo -e "${YELLOW}[3/4] Installing Conan dependencies (Emscripten profile)...${NC}"
cd "$CORE_DIR"

# Check if Emscripten profile exists
if ! conan profile show -pr emscripten_wasm &>/dev/null 2>&1; then
    echo -e "${YELLOW}   Creating Emscripten profile...${NC}"
    PROFILE_DIR="${HOME}/.conan2/profiles"
    mkdir -p "$PROFILE_DIR"

    if [[ -f "$WASM_DIR/emscripten_wasm_full" ]]; then
        cp "$WASM_DIR/emscripten_wasm_full" "$PROFILE_DIR/emscripten_wasm"
        echo -e "${GREEN}✓ Profile created from: $WASM_DIR/emscripten_wasm_full${NC}"
    else
        echo -e "${YELLOW}   Warning: Profile not found, using default detection${NC}"
        conan profile detect --force
    fi
fi

echo -e "${YELLOW}   Running: conan install . --profile=emscripten_wasm -o '&:build_wasm_coordinator=True' --build=missing${NC}"
conan install . --profile=emscripten_wasm -o "&:build_wasm_coordinator=True" --build=missing
echo -e "${GREEN}✓ Dependencies installed${NC}"

# Step 4: Build with CMake
echo -e "${YELLOW}[4/4] Building WASM coordinator...${NC}"
cd "$CORE_DIR"

TOOLCHAIN_FILE="$CORE_DIR/cmake-build-wasm/Release/generators/conan_toolchain.cmake"
CONAN_BUILD_ENV="$CORE_DIR/cmake-build-wasm/Release/generators/conanbuild.sh"

if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
    echo -e "${RED}Error: Conan toolchain file not found:${NC}"
    echo "  $TOOLCHAIN_FILE"
    exit 1
fi

if [[ ! -f "$CONAN_BUILD_ENV" ]]; then
    echo -e "${RED}Error: Conan build environment file not found:${NC}"
    echo "  $CONAN_BUILD_ENV"
    exit 1
fi

# Activate Conan profile build env (CC/CXX=emcc/em++) for this shell.
# shellcheck disable=SC1090
source "$CONAN_BUILD_ENV"

echo -e "${YELLOW}   Configuring with Conan-generated Emscripten toolchain...${NC}"
cmake -S "$CORE_DIR" -B "$CORE_DIR/cmake-build-wasm/Release" \
    -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DFLEAUX_BUILD_WASM_COORDINATOR=ON

echo -e "${YELLOW}   Building target: fleaux_wasm_coordinator...${NC}"
cmake --build "$CORE_DIR/cmake-build-wasm/Release" --target fleaux_wasm_coordinator -- -j$(nproc)

# Verify output
OUTPUT_DIR="$PROJECT_ROOT/fleaux-visual/public/wasm"
if [[ -f "$OUTPUT_DIR/fleaux_wasm_coordinator.js" ]] && [[ -f "$OUTPUT_DIR/fleaux_wasm_coordinator.wasm" ]]; then
    echo -e "${GREEN}✓ Build successful!${NC}"
    echo -e "${GREEN}  JavaScript: $OUTPUT_DIR/fleaux_wasm_coordinator.js${NC}"
    echo -e "${GREEN}  WebAssembly: $OUTPUT_DIR/fleaux_wasm_coordinator.wasm${NC}"
    echo ""
    echo -e "${YELLOW}Next steps:${NC}"
    echo "  1. Test the WASM module:"
    echo "     npm test  (from fleaux-visual directory)"
    echo "  2. Run the development server:"
    echo "     npm run dev  (from fleaux-visual directory)"
    exit 0
else
    echo -e "${RED}✗ Build failed: Output files not found${NC}"
    echo "  Expected: $OUTPUT_DIR/fleaux_wasm_coordinator.js"
    echo "  Expected: $OUTPUT_DIR/fleaux_wasm_coordinator.wasm"
    exit 1
fi





