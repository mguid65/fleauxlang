#include "fleaux/frontend/type_check.hpp"

#include "fleaux/frontend/type_system/checker.hpp"

namespace fleaux::frontend::type_check {
auto analyze_program(const ir::IRProgram& program, const std::unordered_set<std::string>& imported_symbols,
                     const std::vector<ir::IRLet>& imported_typed_lets,
                     const std::vector<ir::IRTypeDecl>& imported_type_decls,
                     const std::vector<ir::IRAliasDecl>& imported_alias_decls) -> AnalysisResult {
  constexpr type_system::Checker checker;
  if (const auto checked =
          checker.analyze(program, imported_symbols, imported_typed_lets, imported_type_decls, imported_alias_decls);
      !checked) {
    return tl::unexpected(checked.error());
  } else {
    return checked.value();
  }
}

}  // namespace fleaux::frontend::type_check
