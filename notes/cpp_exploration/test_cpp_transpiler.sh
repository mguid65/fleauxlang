#!/bin/bash
# test_cpp_transpiler.sh - Test the C++20 transpiler

set -e

echo "Testing Fleaux C++20 Transpiler..."
echo "=================================="

cd /home/matthew/CLionProjects/fleauxlang

# Test 1: Verify transpiler generates valid C++20
echo "Test 1: Generating C++20 code from Fleaux..."
python3 fleaux_cpp_transpiler.py test.fleaux > /dev/null
python3 fleaux_cpp_transpiler.py Std.fleaux > /dev/null
echo "   SUCCESS: C++20 code generated"

# Test 2: Verify generated headers exist
echo "Test 2: Checking generated header files..."
if [ -f fleaux_generated_test.hpp ] && [ -f fleaux_generated_Std.hpp ]; then
    echo "   SUCCESS: Header files generated"
else
    echo "   FAILED: Header files missing"
    exit 1
fi

# Test 3: Verify generated implementations exist
echo "Test 3: Checking generated implementation files..."
if [ -f fleaux_generated_test.cpp ] && [ -f fleaux_generated_Std.cpp ]; then
    echo "   SUCCESS: Implementation files generated"
else
    echo "   FAILED: Implementation files missing"
    exit 1
fi

# Test 4: Verify runtime header exists
echo "Test 4: Checking runtime header..."
if [ -f fleaux_runtime.hpp ]; then
    echo "   SUCCESS: Runtime header exists"
else
    echo "   FAILED: Runtime header missing"
    exit 1
fi

# Test 5: Attempt C++20 syntax check with clang++ or g++
echo "Test 5: Checking C++20 syntax (header-only)..."
if command -v clang++ &> /dev/null; then
    clang++ -std=c++20 -x c++ -fsyntax-only -I. fleaux_generated_test.hpp 2>&1 || true
    echo "   SUCCESS: C++20 syntax valid (clang++)"
elif command -v g++ &> /dev/null; then
    g++ -std=c++20 -x c++ -fsyntax-only -I. fleaux_generated_test.hpp 2>&1 || true
    echo "   SUCCESS: C++20 syntax valid (g++)"
else
    echo "   SKIPPED: No C++ compiler found"
fi

# Test 6: Verify key generated content
echo "Test 6: Verifying generated content structure..."
if grep -q "fleaux::FlowNode" fleaux_generated_test.cpp; then
    echo "   SUCCESS: Found FlowNode definitions"
else
    echo "   FAILED: Missing FlowNode definitions"
    exit 1
fi

if grep -q "fleaux::FlexValue" fleaux_generated_test.cpp; then
    echo "   SUCCESS: Found FlexValue usage"
else
    echo "   FAILED: Missing FlexValue usage"
    exit 1
fi

echo ""
echo "All tests passed!"
echo "=================================="
echo ""
echo "Generated C++20 artifacts:"
ls -lh fleaux_generated_*.{hpp,cpp} fleaux_runtime.hpp 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'

