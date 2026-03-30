#include "fleaux_generated_test.hpp"

namespace fleaux {

fleaux::FlowNode Add3 = fleaux::FlowNode([](const fleaux::FlexValue& input) -> fleaux::FlexValue {
    auto tpl = input;
    return (fleaux::FlexValue(std::make_tuple((fleaux::FlexValue(std::make_tuple((fleaux::FlexValue(std::make_tuple(tpl, fleaux::FlexValue(0))) | fleaux::Std_ElementAt), (fleaux::FlexValue(std::make_tuple(tpl, fleaux::FlexValue(1))) | fleaux::Std_ElementAt))) | fleaux::Std_Add), (fleaux::FlexValue(std::make_tuple(tpl, fleaux::FlexValue(2))) | fleaux::Std_ElementAt))) | fleaux::Std_Add);
});

// Top-level expression: (fleaux::FlexValue(std::make_tuple(fleaux::FlexValue(1), fleaux::FlexValue(2), fleaux::FlexValue(3))) | Add3)

} // namespace fleaux
