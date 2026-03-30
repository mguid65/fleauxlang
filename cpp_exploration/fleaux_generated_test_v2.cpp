#include "fleaux_generated_test_v2.hpp"

namespace fleaux {

std::function<double(std::tuple<double, double, double>)> Add3 =
    [](const auto&... args) {
        auto tpl = std::get<0>(std::tuple_cat(args...));
        return (std::make_tuple((std::make_tuple((std::make_tuple(tpl, 0.0) | ElementAt), (std::make_tuple(tpl, 1.0) | ElementAt)) | Add), (std::make_tuple(tpl, 2.0) | ElementAt)) | Add);
    };

// Top-level expression: (std::make_tuple(1.0, 2.0, 3.0) | Add3)

} // namespace fleaux
