#pragma once

#include "ninfer/ops/grammar.h"
#include "ninfer/types.h"
#include "runtime/contract/token_constraint.h"

#include <memory>

namespace ninfer::runtime {

// Builds the runtime-side constraint for one OutputSession from the request-level
// OutputConstraint. JsonSchema compiles through the grammar engine's schema converter; Gbnf
// parses directly. Throws the grammar engine's compilation errors unchanged. The vocabulary
// table must outlive the returned constraint and every clone taken from it.
[[nodiscard]] std::unique_ptr<TokenConstraint> make_token_constraint(
    const OutputConstraint& constraint, const ops::GrammarTokenTable& table);

} // namespace ninfer::runtime
