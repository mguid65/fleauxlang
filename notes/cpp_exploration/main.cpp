// main.cpp - Example usage of Fleaux transpiled C++20 code
#include <iostream>
#include "fleaux_generated_test.hpp"

int main() {
    try {
        // Example: Call Add3 with tuple (1, 2, 3)
        // Expected result: ((1, 2) -> Add + 3) -> Add = 6
        auto result = fleaux::Add3(
            fleaux::FlexValue(std::make_tuple(
                fleaux::FlexValue(1.0),
                fleaux::FlexValue(2.0),
                fleaux::FlexValue(3.0)
            ))
        );

        // Print result
        try {
            double value = std::any_cast<double>(result);
            std::cout << "Result: " << value << std::endl;
        } catch (const std::bad_any_cast&) {
            std::cout << "Result type is not double" << std::endl;
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

