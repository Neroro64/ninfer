#pragma once

// Vendored from llama.cpp src/llama-grammar.h (MIT, see LICENSE and README.ninfer.md).
// Adaptations: llama_vocab is replaced by the abstract llama_grammar_vocab view; llama_token
// is defined locally; the lazy/trigger machinery and the token-data-array sampling interface
// are removed; grammar rejection is reported as an explicit token-id list; piece parameters
// are string_view so token evaluation copies no piece bytes; single-token acceptance probing
// and a satisfaction query (empty stack with no pending partial UTF-8) are added for
// snapshot/branch-style callers.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

typedef int32_t llama_token;

// grammar element type
enum llama_gretype {
    // end of rule definition
    LLAMA_GRETYPE_END            = 0,

    // start of alternate definition for rule
    LLAMA_GRETYPE_ALT            = 1,

    // non-terminal element: reference to rule
    LLAMA_GRETYPE_RULE_REF       = 2,

    // terminal element: character (code point)
    LLAMA_GRETYPE_CHAR           = 3,

    // inverse char(s) ([^a], [^a-b] [^abc])
    LLAMA_GRETYPE_CHAR_NOT       = 4,

    // modifies a preceding LLAMA_GRETYPE_CHAR or LLAMA_GRETYPE_CHAR_ALT to
    // be an inclusive range ([a-z])
    LLAMA_GRETYPE_CHAR_RNG_UPPER = 5,

    // modifies a preceding LLAMA_GRETYPE_CHAR or LLAMA_GRETYPE_CHAR_RNG_UPPER to add an
    // alternate char to match ([ab], [a-zA])
    LLAMA_GRETYPE_CHAR_ALT       = 6,

    // any character (.)
    LLAMA_GRETYPE_CHAR_ANY       = 7,

    // terminal element: token (<[token-id]>)
    LLAMA_GRETYPE_TOKEN          = 8,

    // inverse token (!<[token-id]>)
    LLAMA_GRETYPE_TOKEN_NOT      = 9,
};

typedef struct llama_grammar_element {
    enum llama_gretype type;
    uint32_t           value; // Unicode code point, rule ID, or token ID
} llama_grammar_element;

struct llama_partial_utf8 {
    uint32_t value;    // bit value so far (unshifted)
    int      n_remain; // num bytes remaining; -1 indicates invalid sequence
};

struct llama_grammar_candidate {
    size_t               index;
    const uint32_t     * code_points;
    llama_partial_utf8   partial_utf8;
    llama_token          id;
};

// Tokenizer view the grammar engine needs. Implementations must return raw token bytes from
// token_to_piece; an empty view marks the token as a non-candidate (special tokens, unused
// vocabulary rows). is_eog marks end-of-generation tokens, which are allowed exactly when the
// grammar has reached a satisfied state.
struct llama_grammar_vocab {
    virtual ~llama_grammar_vocab() = default;

    [[nodiscard]] virtual std::string_view token_to_piece(llama_token id) const   = 0;
    [[nodiscard]] virtual bool is_eog(llama_token id) const                       = 0;
    [[nodiscard]] virtual std::vector<llama_token>
        tokenize(std::string_view text, bool special) const                       = 0;
    [[nodiscard]] virtual llama_token n_vocab() const                             = 0;
};

using llama_grammar_rule  = std::vector<      llama_grammar_element>;
using llama_grammar_stack = std::vector<const llama_grammar_element *>;

using llama_grammar_rules      = std::vector<llama_grammar_rule>;
using llama_grammar_stacks     = std::vector<llama_grammar_stack>;
using llama_grammar_candidates = std::vector<llama_grammar_candidate>;

// takes a set of possible pushdown stacks on a grammar, which are required to
// be positioned at a character range (see `llama_grammar_advance_stack`), and
// produces the N possible stacks if the given char is accepted at those
// positions
void llama_grammar_accept(struct llama_grammar * grammar, uint32_t chr);

std::vector<llama_grammar_candidate> llama_grammar_reject_candidates_for_stack(
        const llama_grammar_rules      & rules,
        const llama_grammar_stack      & stack,
        const llama_grammar_candidates & candidates);

struct llama_grammar_parser {
    const llama_grammar_vocab * vocab;
    std::map<std::string, uint32_t> symbol_ids;

    llama_grammar_rules rules;
    std::string error;

    llama_grammar_parser(const llama_grammar_vocab * vocab = nullptr) : vocab(vocab) {}

    llama_grammar_stack c_rules() const;

    uint32_t get_symbol_id(const char * src, size_t len);
    uint32_t generate_symbol_id(const std::string & base_name);

    void add_rule(uint32_t rule_id, const llama_grammar_rule & rule);

    const char * parse_alternates(
            const char        * src,
            const std::string & rule_name,
            uint32_t            rule_id,
            bool                is_nested);

    const char * parse_sequence(
            const char         * src,
            const std::string  & rule_name,
            llama_grammar_rule & rule,
            bool               is_nested);

    const char * parse_rule(const char * src);

    bool parse(const char * src);
};

struct llama_grammar {
    const llama_grammar_vocab * vocab;

    const llama_grammar_rules  rules;
          llama_grammar_stacks stacks;

    // buffer for partially generated UTF-8 sequence from accepted tokens
    llama_partial_utf8 partial_utf8;
};

//
// internal API
//

struct llama_grammar * llama_grammar_init_impl(
        const llama_grammar_vocab * vocab,
        const llama_grammar_element ** rules,
        size_t n_rules,
        size_t start_rule_index);

struct llama_grammar * llama_grammar_init_impl(
        const llama_grammar_vocab * vocab,
        const char * grammar_str,
        const char * grammar_root);

void llama_grammar_free_impl(struct llama_grammar * grammar);

struct llama_grammar * llama_grammar_clone_impl(const struct llama_grammar & grammar);

// Returns the vocabulary token ids rejected in the grammar's current state. End-of-generation
// tokens are rejected unless the grammar has at least one satisfied (empty) stack.
std::vector<llama_token> llama_grammar_rejected_tokens(const struct llama_grammar & grammar);

void llama_grammar_accept_impl(
              struct llama_grammar & grammar,
                       llama_token   token);

void llama_grammar_accept_str(
              struct llama_grammar & grammar,
              std::string_view       piece);

void llama_grammar_accept_token(
              struct llama_grammar & grammar,
                       llama_token   token,
              std::string_view       piece);

// True iff accepting the token in the grammar's current state would keep at least one pushdown
// stack alive. End-of-generation tokens are accepted exactly when llama_grammar_is_satisfied()
// holds. The grammar is not modified.
bool llama_grammar_token_accepted(const struct llama_grammar & grammar, llama_token token);

// True iff generation may legally stop here: at least one stack is complete AND no partial
// UTF-8 sequence is pending (a pending sequence means the last committed token split a
// multi-byte character and the emitted text is not yet valid UTF-8).
bool llama_grammar_is_satisfied(const struct llama_grammar & grammar);
