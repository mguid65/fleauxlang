#pragma once

#include <string>

#include <tl/expected.hpp>

#include "fleaux/frontend/ast.hpp"

namespace fleaux::frontend::lowering {

struct LoweringError {
  std::string message{};
  std::optional<std::string> hint{std::nullopt};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

using LoweringResult = tl::expected<ir::IRProgram, LoweringError>;

// Lowers a parsed type node to its IR representation.
[[nodiscard]] auto lower_type(const model::TypeNode& type) -> ir::IRSimpleType;

// Formats an IR type in canonical source syntax, e.g. 'Result<Tuple<Int64...>, String>'. A top-level variadic
// marker is omitted; nested variadic markers are kept.
[[nodiscard]] auto format_type_text(const ir::IRSimpleType& type) -> std::string;

class Lowerer {
public:
  [[nodiscard]] auto lower_only(const model::Program& program) const -> LoweringResult;
  [[nodiscard]] auto lower(const model::Program& program) const -> LoweringResult;
  [[nodiscard]] auto dump_ir(const ir::IRProgram& program) const -> std::string;
};

}  // namespace fleaux::frontend::lowering
