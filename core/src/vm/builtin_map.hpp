// Legacy compatibility header.
// Builtin dispatch now resolves names to BuiltinId during compilation and the
// VM executes builtin operands directly, so there is no runtime builtin map.
#pragma once
#include "fleaux/vm/builtin_catalog.hpp"

