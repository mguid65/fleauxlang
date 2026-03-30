#!/bin/bash
# build_cpp20_example.sh - Build a complete C++20 executable from Fleaux

set -e

FLEAUX_DIR="/home/matthew/CLionProjects/fleauxlang"
BUILD_DIR="${FLEAUX_DIR}/build"
COMPILER="${CXX:-g++}"
FLEAUX_FILE="${1:-test.fleaux}"
MODULE_NAME="${FLEAUX_FILE%.fleaux}"

echo "Fleaux C++20 Build System"
echo "=========================="
echo "Module: $MODULE_NAME"
echo "Compiler: $COMPILER (C++20)"
echo ""

# Step 1: Transpile Fleaux to C++20
echo "Step 1: Transpiling Fleaux to C++20..."
cd "$FLEAUX_DIR"
python3 fleaux_cpp_transpiler.py "$FLEAUX_FILE"
echo "  Generated: fleaux_generated_${MODULE_NAME}.hpp"
echo "  Generated: fleaux_generated_${MODULE_NAME}.cpp"
echo ""

# Step 2: Create build directory
echo "Step 2: Setting up build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
echo "  Build directory: $BUILD_DIR"
echo ""

# Step 3: Compile all C++20 files (including dependencies)
echo "Step 3: Compiling C++20 code..."

# Build list of object files needed
OBJS=("$FLEAUX_DIR/fleaux_generated_${MODULE_NAME}.cpp" "$FLEAUX_DIR/main.cpp")
# Add Std module if not already included
if [ "$MODULE_NAME" != "Std" ]; then
    OBJS+=("$FLEAUX_DIR/fleaux_generated_Std.cpp")
fi

echo "  Command: $COMPILER -std=c++20 -I$FLEAUX_DIR -o ${MODULE_NAME} \\"
for obj in "${OBJS[@]}"; do
    echo "             $(basename "$obj") \\"
done

$COMPILER -std=c++20 -I"$FLEAUX_DIR" -O2 -o "${MODULE_NAME}" "${OBJS[@]}"

echo ""
echo "Build successful!"
echo "=========================="
echo "Executable: $BUILD_DIR/${MODULE_NAME}"
echo ""
echo "To run: $BUILD_DIR/${MODULE_NAME}"

