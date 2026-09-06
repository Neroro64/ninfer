#pragma once

#include "ninfer/types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ninfer::ops {

// Process-owned vocabulary view for grammar evaluation, built once per loaded model from the
// family tokenizer. Piece bytes for special tokens are empty so the grammar engine treats them
// as non-candidates; end-of-generation ids stay queryable and are allowed exactly when the
// grammar reaches a satisfied state. The table must outlive every Grammar compiled against it,
// including every copy.
class GrammarTokenTable {
public:
    // pieces holds the raw byte string of every token id in [0, pieces.size()); special tokens
    // carry empty bytes. eog lists the end-of-generation token ids in any order.
    GrammarTokenTable(std::vector<std::string> pieces, std::vector<TokenId> eog);

    // Optional text-to-token hook used only by GBNF <token> rules. Returning an empty vector
    // makes those rules fail conversion; JSON-Schema grammars never need it.
    using TokenizeFn = std::function<std::vector<TokenId>(std::string_view text, bool special)>;

    void set_tokenize(TokenizeFn tokenize);
    [[nodiscard]] std::size_t n_vocab() const noexcept;
    [[nodiscard]] const std::vector<std::string>& pieces() const noexcept;
    [[nodiscard]] const std::vector<TokenId>& eog() const noexcept;
    // True when id is an end-of-generation token (binary search over the sorted id list).
    [[nodiscard]] bool is_eog(TokenId id) const noexcept;
    [[nodiscard]] const TokenizeFn& tokenizer() const noexcept;

private:
    std::vector<std::string> pieces_;
    std::vector<TokenId> eog_;
    TokenizeFn tokenize_;
};

// One constrained-decoding automaton per request lane. Immutable compiled rules, mutable
// pushdown state advanced by accept_bytes()/accept_token() in exact committed-output order.
//
// The state is cheaply clonable: copying a Grammar deep-copies only the pushdown state
// (O(rules + stack elements)) and shares the compiled rules and the vocab view. Typical
// speculative use: `Grammar probe = base;` (or `auto probe = base.clone();`), advance the
// probe with accept_token()/accept_bytes(), inspect would_accept()/allowed_mask()/satisfied(),
// then either discard it (rollback) or `base = std::move(probe);` (commit). A Grammar and all
// its copies must not be advanced concurrently; the GrammarTokenTable must outlive all of
// them.
class Grammar {
public:
    // Converts a JSON Schema document to GBNF without any vocabulary. Throws
    // std::invalid_argument when the schema uses a construct the converter cannot express
    // exactly: unsupported assertions (not/if/then/else/unevaluated*/patternProperties/
    // propertyNames/dependencies*/contains/uniqueItems/min-maxProperties/multipleOf),
    // overlapping oneOf alternatives (exact oneOf semantics are unsupported; a union is only
    // emitted when the alternatives are provably disjoint), sibling assertions that would be
    // silently ignored, numeric bounds on non-integer numbers, bounds or lengths without an
    // applicable explicit type, unknown string formats, empty enums, and required property
    // names without a properties entry. root_name namespaces the emitted rules for merging
    // several converted schemas into one grammar text ("" names the start rule "root").
    static std::string schema_to_gbnf(std::string_view schema_json, std::string_view root_name = "");

    // Parses and validates GBNF text (rule references, left recursion, start symbol) without
    // any vocabulary. Throws std::invalid_argument on any parse or validation failure; named
    // <token> elements require a vocabulary and are rejected, while <[id]> elements parse.
    static void validate_gbnf(std::string_view grammar_text);

    // Compiles a JSON Schema document (schema_to_gbnf, then gbnf). Throws std::invalid_argument
    // for schema conversion failures and std::runtime_error for grammar compilation failures.
    static Grammar json_schema(std::string_view schema_json, const GrammarTokenTable& table);

    // Compiles a GBNF grammar document. Throws std::runtime_error on parse, unknown rule
    // reference, or left-recursion failure.
    static Grammar gbnf(std::string_view grammar_text, const GrammarTokenTable& table);

    Grammar(const Grammar& other);
    Grammar& operator=(const Grammar& other);
    Grammar(Grammar&&) noexcept;
    Grammar& operator=(Grammar&&) noexcept;
    ~Grammar();

    // Deep copy of the current pushdown state; equivalent to copy construction.
    [[nodiscard]] Grammar clone() const;

    // Accepts raw token bytes in emission order. Throws std::runtime_error when no pushdown
    // stack survives, which indicates a caller that committed an unconstrained token; the
    // grammar is then dead: every token, including end-of-generation, reports rejected and
    // allowed_mask() is all zeros. No piece bytes are copied.
    void accept_bytes(std::string_view bytes);

    // Accepts one whole token by id, fetching its piece bytes from the vocabulary table.
    // End-of-generation tokens are accepted only while satisfied(). Throws std::runtime_error
    // when no pushdown stack survives (dead grammar, as with accept_bytes()).
    void accept_token(TokenId token);

    // True when accepting the token would keep at least one pushdown stack alive; the state is
    // not modified. End-of-generation tokens are accepted exactly when satisfied() holds.
    [[nodiscard]] bool would_accept(TokenId token) const;

    // Token ids rejected in the current state. End-of-generation ids are rejected unless
    // satisfied() is true.
    [[nodiscard]] std::vector<TokenId> rejected_tokens() const;

    // True when generation may legally stop: at least one stack is complete and no partial
    // UTF-8 sequence is pending from the last accepted token.
    [[nodiscard]] bool satisfied() const;

    // Bitset of size ceil(n_vocab/32) words with bit v set exactly when token v is allowed.
    // Trailing bits past n_vocab are zero. Recomputed on every call; no caching.
    [[nodiscard]] std::vector<std::uint32_t> allowed_mask() const;

private:
    Grammar();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ninfer::ops
