// ninfer::ops - grammar-constrained decoding bridge over the vendored llama-grammar core.

#include "ninfer/ops/grammar.h"

#include "llama-grammar/llama-grammar.h"
#include "llama-grammar/json-schema-to-grammar.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace ninfer::ops {
namespace {

// Bridges GrammarTokenTable to the vendored grammar engine. Special tokens carry empty pieces,
// so the engine rejects them as candidates while is_eog keeps them reachable at satisfied
// states. The view is shared between a Grammar and all of its copies: it reads through the
// table, which must outlive every copy.
class TableVocabView final : public llama_grammar_vocab {
public:
    explicit TableVocabView(const GrammarTokenTable& table) : table_(table) {}

    [[nodiscard]] std::string_view token_to_piece(llama_token id) const override {
        const auto& pieces = table_.pieces();
        if (id < 0 || static_cast<std::size_t>(id) >= pieces.size()) {
            // Unknown ids are non-candidates: an empty piece is rejected by the engine.
            return {};
        }
        return pieces[static_cast<std::size_t>(id)];
    }

    [[nodiscard]] bool is_eog(llama_token id) const override {
        return table_.is_eog(id);
    }

    [[nodiscard]] std::vector<llama_token>
    tokenize(std::string_view text, bool special) const override {
        const auto& tokenize = table_.tokenizer();
        if (!tokenize) { return {}; }
        auto tokens = tokenize(text, special);
        return {tokens.begin(), tokens.end()};
    }

    [[nodiscard]] llama_token n_vocab() const override {
        return static_cast<llama_token>(table_.pieces().size());
    }

private:
    const GrammarTokenTable& table_;
};

std::string join_lines(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        if (!joined.empty()) { joined += "; "; }
        joined += line;
    }
    return joined;
}

} // namespace

GrammarTokenTable::GrammarTokenTable(std::vector<std::string> pieces, std::vector<TokenId> eog)
    : pieces_(std::move(pieces)) {
    eog_ = std::move(eog);
    std::sort(eog_.begin(), eog_.end());
}

void GrammarTokenTable::set_tokenize(TokenizeFn tokenize) {
    tokenize_ = std::move(tokenize);
}

std::size_t GrammarTokenTable::n_vocab() const noexcept {
    return pieces_.size();
}

const std::vector<std::string>& GrammarTokenTable::pieces() const noexcept {
    return pieces_;
}

const std::vector<TokenId>& GrammarTokenTable::eog() const noexcept {
    return eog_;
}

bool GrammarTokenTable::is_eog(TokenId id) const noexcept {
    return std::binary_search(eog_.begin(), eog_.end(), id);
}

const GrammarTokenTable::TokenizeFn& GrammarTokenTable::tokenizer() const noexcept {
    return tokenize_;
}

struct Grammar::Impl {
    // The vocab view must outlive the automaton: the grammar stores the view pointer. It is
    // shared so that Grammar copies (pushdown-state snapshots) reuse one view.
    std::shared_ptr<llama_grammar_vocab> view;
    llama_grammar* grammar = nullptr;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (grammar != nullptr) { llama_grammar_free_impl(grammar); }
    }
};

Grammar::Grammar() : impl_(std::make_unique<Impl>()) {}

Grammar::Grammar(const Grammar& other) : impl_(std::make_unique<Impl>()) {
    impl_->view = other.impl_->view;
    impl_->grammar = llama_grammar_clone_impl(*other.impl_->grammar);
}

Grammar& Grammar::operator=(const Grammar& other) {
    if (this != &other) {
        Grammar copy(other);
        impl_ = std::move(copy.impl_);
    }
    return *this;
}

Grammar Grammar::clone() const {
    return *this;
}

Grammar::Grammar(Grammar&&) noexcept = default;
Grammar& Grammar::operator=(Grammar&&) noexcept = default;
Grammar::~Grammar()                             = default;

std::string Grammar::schema_to_gbnf(std::string_view schema_json, std::string_view root_name) {
    nlohmann::json schema;
    try {
        schema = nlohmann::json::parse(schema_json);
    } catch (const nlohmann::json::exception& err) {
        throw std::invalid_argument(std::string("grammar schema is not valid JSON: ") + err.what());
    }
    if (!schema.is_object()) {
        throw std::invalid_argument("grammar schema must be a JSON object");
    }

    const json_schema_grammar converted = json_schema_to_grammar(schema, std::string(root_name));
    if (!converted.warnings.empty()) {
        // Degraded constructs would silently relax the constraint; refuse them instead.
        throw std::invalid_argument("grammar schema uses unsupported constructs: " +
                                    join_lines(converted.warnings));
    }
    return converted.gbnf;
}

void Grammar::validate_gbnf(std::string_view grammar_text) {
    llama_grammar* parsed = nullptr;
    try {
        parsed = llama_grammar_init_impl(nullptr, std::string(grammar_text).c_str(), "root");
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& err) {
        throw std::invalid_argument(std::string("grammar text is not valid GBNF: ") + err.what());
    }
    llama_grammar_free_impl(parsed);
}

Grammar Grammar::json_schema(std::string_view schema_json, const GrammarTokenTable& table) {
    return gbnf(schema_to_gbnf(schema_json), table);
}

Grammar Grammar::gbnf(std::string_view grammar_text, const GrammarTokenTable& table) {
    Grammar grammar;
    auto view = std::make_shared<TableVocabView>(table);
    grammar.impl_->view = view;
    grammar.impl_->grammar =
        llama_grammar_init_impl(view.get(), std::string(grammar_text).c_str(), "root");
    return grammar;
}

void Grammar::accept_bytes(std::string_view bytes) {
    llama_grammar_accept_str(*impl_->grammar, bytes);
}

void Grammar::accept_token(TokenId token) {
    llama_grammar_accept_impl(*impl_->grammar, static_cast<llama_token>(token));
}

bool Grammar::would_accept(TokenId token) const {
    return llama_grammar_token_accepted(*impl_->grammar, static_cast<llama_token>(token));
}

std::vector<TokenId> Grammar::rejected_tokens() const {
    if (impl_->grammar->stacks.empty()) {
        // A previous accept committed a token no stack survived: the grammar is dead and
        // nothing, not even end-of-generation, is acceptable anymore.
        std::vector<TokenId> all;
        const auto n_vocab = impl_->grammar->vocab->n_vocab();
        all.reserve(static_cast<std::size_t>(n_vocab));
        for (llama_token id = 0; id < n_vocab; ++id) {
            all.push_back(static_cast<TokenId>(id));
        }
        return all;
    }
    const auto rejected = llama_grammar_rejected_tokens(*impl_->grammar);
    return {rejected.begin(), rejected.end()};
}

bool Grammar::satisfied() const {
    return llama_grammar_is_satisfied(*impl_->grammar);
}

std::vector<std::uint32_t> Grammar::allowed_mask() const {
    const std::size_t n_vocab = impl_->grammar->vocab->n_vocab();
    std::vector<std::uint32_t> words((n_vocab + 31) / 32, 0xFFFFFFFFU);
    const std::size_t remainder = n_vocab % 32;
    if (!words.empty()) {
        words.back() = remainder == 0 ? 0xFFFFFFFFU : (0xFFFFFFFFU >> (32 - remainder));
    }
    for (const TokenId id : rejected_tokens()) {
        if (id >= 0 && static_cast<std::size_t>(id) < n_vocab) {
            words[static_cast<std::size_t>(id) / 32] &=
                ~(1U << (static_cast<std::size_t>(id) % 32));
        }
    }
    return words;
}

} // namespace ninfer::ops
