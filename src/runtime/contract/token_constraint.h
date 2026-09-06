#pragma once

#include "ninfer/types.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ninfer::runtime {

// Compiled output constraint evaluated by the sampling runtime. One instance belongs to one
// OutputSession and advances in exact committed-output token order; speculative exploration
// runs on clones so a rejected draft prefix never contaminates the committed state.
//
// allowed_mask() covers the full token domain: ceil(n_vocab/32) little-endian words with bit v
// set exactly when token v may be committed next. End-of-generation ids are allowed exactly
// when the constraint is satisfied. Implementations must remain safe to call from the Program
// execution thread only; no internal synchronization is required.
class TokenConstraint {
public:
    TokenConstraint()          = default;
    virtual ~TokenConstraint() = default;

    TokenConstraint(const TokenConstraint&)            = delete;
    TokenConstraint& operator=(const TokenConstraint&) = delete;
    TokenConstraint(TokenConstraint&&)                 = delete;
    TokenConstraint& operator=(TokenConstraint&&)      = delete;

    // Deep copy of the current constraint state for speculative prefix exploration.
    [[nodiscard]] virtual std::unique_ptr<TokenConstraint> clone() const = 0;

    // Commits one token in emission order. Throws std::runtime_error when the token cannot be
    // committed (callers must consult allowed_mask() first).
    virtual void accept(TokenId token) = 0;

    // Bitset of size ceil(n_vocab/32) with bit v set exactly when token v is allowed next.
    // Recomputed on every call; no caching.
    [[nodiscard]] virtual std::vector<std::uint32_t> allowed_mask() const = 0;
};

} // namespace ninfer::runtime
