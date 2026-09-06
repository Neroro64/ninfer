#include "runtime/contract/grammar_constraint.h"

#include <stdexcept>
#include <utility>

namespace ninfer::runtime {

namespace {

// TokenConstraint view over one ops::Grammar automaton. The Grammar deep-copies only pushdown
// state, so clone() is the speculative probe primitive and accept() commits exact output.
class GrammarTokenConstraint final : public TokenConstraint {
public:
    explicit GrammarTokenConstraint(ops::Grammar grammar) : grammar_(std::move(grammar)) {}

    [[nodiscard]] std::unique_ptr<TokenConstraint> clone() const override {
        return std::make_unique<GrammarTokenConstraint>(grammar_.clone());
    }

    void accept(TokenId token) override { grammar_.accept_token(token); }

    [[nodiscard]] std::vector<std::uint32_t> allowed_mask() const override {
        return grammar_.allowed_mask();
    }

private:
    ops::Grammar grammar_;
};

} // namespace

std::unique_ptr<TokenConstraint> make_token_constraint(const OutputConstraint& constraint,
                                                       const ops::GrammarTokenTable& table) {
    switch (constraint.kind) {
    case OutputConstraint::JsonSchema:
        return std::make_unique<GrammarTokenConstraint>(
            ops::Grammar::json_schema(constraint.text, table));
    case OutputConstraint::Gbnf:
        return std::make_unique<GrammarTokenConstraint>(ops::Grammar::gbnf(constraint.text, table));
    }
    throw std::invalid_argument("unknown output constraint kind");
}

} // namespace ninfer::runtime
