// main_v2.cpp - Example usage of strongly-typed Fleaux C++20 code
#include <iostream>
#include "fleaux_generated_test_v2.hpp"

int main() {
    try {
        // Test 1: Simple tuple piping
        std::cout << "Test 1: Simple arithmetic" << std::endl;
        auto result1 = (std::make_tuple(2.0, 3.0) | fleaux::Add);
        std::cout << "  (2, 3) -> Add = " << result1 << std::endl;

        // Test 2: Chained operations
        std::cout << "\nTest 2: Chained arithmetic" << std::endl;
        auto result2 = ((std::make_tuple(2.0, 3.0) | fleaux::Add, 4.0) | fleaux::Add);
        std::cout << "  ((2, 3) -> Add, 4) -> Add = " << result2 << std::endl;

        // Test 3: Test Add3 with tuple
        std::cout << "\nTest 3: Add3 function" << std::endl;
        auto result3 = fleaux::Add3(std::make_tuple(1.0, 2.0, 3.0));
        std::cout << "  Add3((1, 2, 3)) = " << result3 << std::endl;
        std::cout << "  Expected: 6 (i.e., (1+2)+3)" << std::endl;

        std::cout << "\nAll tests completed successfully!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

