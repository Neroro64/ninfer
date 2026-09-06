// ninfer::ops - grammar engine correctness against an independent prefix-validity oracle.
//
// The oracle expectations are derived by hand from each schema's JSON-Schema semantics, not
// from the vendored converter: a curated valid document must be accepted character by
// character with every next-character token allowed by the mask and with the grammar satisfied
// only after the document completes; a curated invalid document must have its first diverging
// character blocked by the mask exactly at the divergence prefix.

#include "ninfer/ops/grammar.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace ninfer;
using namespace ninfer::ops;

namespace {

// End-of-generation token id: within the 100-token vocabulary (printable ASCII occupies
// 0..94, the two-byte UTF-8 token 95, the multi-character token 96, specials 97..99).
constexpr TokenId kEogId = 99;

// Pieces index of the two-byte UTF-8 "é" token.
constexpr TokenId kUtf8Id = 95;

// Pieces index of the multi-character "{\"a" token.
constexpr TokenId kMultiCharId = 96;

GrammarTokenTable make_table() {
    // Single-byte tokens for every printable ASCII character, one multi-byte UTF-8 token, one
    // multi-character token, and three special tokens with empty pieces.
    std::vector<std::string> pieces;
    for (int c = 0x20; c <= 0x7E; ++c) {
        pieces.emplace_back(1, static_cast<char>(c));
    }
    pieces.emplace_back("\xC3\xA9"); // multi-byte UTF-8 token
    pieces.emplace_back("{\"a");     // multi-character token
    pieces.emplace_back("");         // special
    pieces.emplace_back("");         // special
    pieces.emplace_back("");         // special (EOG)
    return GrammarTokenTable(std::move(pieces), {kEogId});
}

TokenId char_id(char c) { return static_cast<TokenId>(c) - 0x20; }

bool token_allowed(const Grammar& grammar, char c) {
    const auto mask  = grammar.allowed_mask();
    const TokenId id = char_id(c);
    return (mask[static_cast<std::size_t>(id) / 32] >> (id % 32)) & 1U;
}

// Accepts s one character at a time and enforces the prefix-validity oracle. Returns failure
// count. divergence >= 0 marks the index of the first character that must be BLOCKED; every
// character before it must be allowed. complete == false means the document is a valid prefix
// only and satisfied() must stay false with EOG rejected.
int check_document(Grammar& grammar, const std::string& s, int divergence, bool complete,
                   const std::string& label) {
    int failures = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool must_block = divergence >= 0 && static_cast<int>(i) == divergence;
        const bool allowed    = token_allowed(grammar, s[i]);
        if (must_block == allowed) {
            std::cerr << label << ": character '" << s[i] << "' at " << i << " allowed="
                      << allowed << " expected allowed=" << !must_block << '\n';
            ++failures;
            return failures;
        }
        if (must_block) { return failures; }
        grammar.accept_bytes(std::string(1, s[i]));
    }
    if (grammar.satisfied() != complete) {
        std::cerr << label << ": satisfied=" << grammar.satisfied() << " expected=" << complete
                  << '\n';
        ++failures;
    }
    const auto rejected  = grammar.rejected_tokens();
    const bool eog_rejected =
        std::find(rejected.begin(), rejected.end(), kEogId) != rejected.end();
    if (eog_rejected == complete) {
        std::cerr << label << ": eog_rejected=" << eog_rejected << " expected=" << !complete
                  << '\n';
        ++failures;
    }
    return failures;
}

int run_schema(const std::string& name, const std::string& schema,
               const std::vector<std::string>& valid_documents,
               const std::vector<std::pair<std::string, int>>& invalid_documents) {
    int failures = 0;
    const GrammarTokenTable table = make_table();
    for (const auto& document : valid_documents) {
        Grammar grammar = Grammar::json_schema(schema, table);
        failures +=
            check_document(grammar, document, -1, true, name + " valid \"" + document + "\"");
    }
    for (const auto& [document, divergence] : invalid_documents) {
        Grammar grammar = Grammar::json_schema(schema, table);
        failures += check_document(grammar, document, divergence, false,
                                   name + " invalid \"" + document + "\"@" +
                                       std::to_string(divergence));
    }
    return failures;
}

} // namespace

int main() {
    int failures = 0;

    // Integer object with a closed property set. The object primitive rule admits whitespace
    // only after '{' and before '}', so divergence indices are hand-derived from that shape.
    const std::string integer_object =
        R"({"type":"object","properties":{"a":{"type":"integer"}},"required":["a"],"additionalProperties":false})";
    failures += run_schema("integer_object", integer_object,
                           {
                               R"cx({"a":42})cx",
                               R"cx({"a":-7})cx",
                               R"cx({"a": 0})cx",
                           },
                           {
                               {R"cx({"b":1})cx", 2},       // unknown property name diverges at 'b'
                               {R"cx({"a":"x"})cx", 5},     // string value under integer schema
                               {R"cx({"a":1.5})cx", 6},     // fraction under integer schema
                               {R"cx({"a":1,"b":2})cx", 6}, // closed set denies the comma
                           });

    // anyOf of string and integer.
    const std::string string_or_integer = R"({"anyOf":[{"type":"string"},{"type":"integer"}]})";
    failures += run_schema("string_or_integer", string_or_integer,
                           {
                               R"cx("hi")cx",
                               "42",
                           },
                           {
                               {"true", 0},
                               {"\"hi", -1},
                           });

    // The two-byte UTF-8 string token: blocked at the root state (no candidate may start
    // there), allowed inside a string, and its full two bytes advance the grammar.
    {
        const GrammarTokenTable table = make_table();
        Grammar grammar               = Grammar::json_schema(string_or_integer, table);
        auto mask                     = grammar.allowed_mask();
        bool allowed                  = (mask[static_cast<std::size_t>(kUtf8Id) / 32] >>
                        (kUtf8Id % 32)) & 1U;
        if (allowed) {
            std::cerr << "utf8: the é token was allowed at the root state\n";
            ++failures;
        }
        grammar.accept_bytes("\"");
        mask    = grammar.allowed_mask();
        allowed = (mask[static_cast<std::size_t>(kUtf8Id) / 32] >> (kUtf8Id % 32)) & 1U;
        if (!allowed) {
            std::cerr << "utf8: the é token was not allowed inside a string\n";
            ++failures;
        }
        grammar.accept_bytes("\xC3\xA9");
        grammar.accept_bytes("\"");
        if (!grammar.satisfied()) {
            std::cerr << "utf8: grammar unsatisfied after a complete string\n";
            ++failures;
        }
    }
    // Enum restricted to two constants: the diverging alternative name is blocked.
    failures += run_schema("enum",
                           R"({"enum":["red","green"]})",
                           {R"cx("red")cx", R"cx("green")cx"},
                           {{R"cx("blue")cx", 1}});

    // $ref into $defs.
    const std::string ref_schema =
        R"({"type":"object","properties":{"n":{"$ref":"#/$defs/i"}},"required":["n"],"additionalProperties":false,"$defs":{"i":{"type":"integer"}}})";
    failures += run_schema("ref", ref_schema, {R"cx({"n":5})cx"}, {{R"cx({"n":5.5})cx", 6}});

    // Bounded string length.
    failures += run_schema("bounded_string",
                           R"({"type":"string","minLength":2,"maxLength":3})",
                           {R"cx("ab")cx", R"cx("abc")cx"},
                           {{R"cx("a")cx", 2}, {"\"abcd", 4}});

    // json_object mode: any object.
    failures += run_schema("json_object",
                           R"({"type":"object"})",
                           {"{}", R"cx({"x":[1,true,null],"y":"z"})cx"},
                           {{R"cx([1])cx", 0}});

    // Valid prefix is accepted but does not satisfy the grammar and keeps EOG rejected.
    {
        const GrammarTokenTable table = make_table();
        Grammar grammar               = Grammar::json_schema(integer_object, table);
        failures += check_document(grammar, R"({"a":42)", -1, false, "integer_object prefix");
    }

    // Multi-character token candidates: the '{"a' token is a legal opening for the object.
    {
        const GrammarTokenTable table = make_table();
        Grammar grammar               = Grammar::json_schema(integer_object, table);
        const auto mask               = grammar.allowed_mask();
        const bool allowed =
            (mask[static_cast<std::size_t>(kMultiCharId) / 32] >> (kMultiCharId % 32)) & 1U;
        if (!allowed) {
            std::cerr << "multi_char: the {\"a token was not allowed at the root state\n";
            ++failures;
        }
    }

    // Conversion failures.
    {
        const GrammarTokenTable table = make_table();
        bool threw = false;
        try {
            Grammar g = Grammar::json_schema("{not json", table);
            (void)g;
        } catch (const std::invalid_argument&) { threw = true; }
        if (!threw) {
            std::cerr << "malformed schema did not throw\n";
            ++failures;
        }

        threw = false;
        try {
            // The converter degrades unsupported patterns to any-string via a warning; NInfer
            // refuses degraded constraints.
            Grammar g = Grammar::json_schema(R"cx({"type":"string","pattern":"(?!x)"})cx", table);
            (void)g;
        } catch (const std::invalid_argument&) { threw = true; }
        if (!threw) {
            std::cerr << "degraded pattern schema did not throw\n";
            ++failures;
        }

        threw = false;
        try {
            Grammar g = Grammar::gbnf("root ::= root \"a\"", table);
            (void)g;
        } catch (const std::runtime_error&) { threw = true; }
        if (!threw) {
            std::cerr << "left-recursive grammar did not throw\n";
            ++failures;
        }
    }

    // ---- strict schema conversion: constraints must never silently broaden ---------------
    {
        const GrammarTokenTable table = make_table();
        const std::pair<const char*, const char*> rejected[] = {
            // oneOf overlap: exactly-one semantics are unsupported and a union would
            // broaden integer-or-number to the same as number.
            {"oneOf int/number overlap",
             R"cx({"oneOf":[{"type":"integer"},{"type":"number"}]})cx"},
            {"oneOf const overlap",
             R"cx({"oneOf":[{"const":"a","type":"string"},{"enum":["a"],"type":"string"}]})cx"},
            // Assertions that could not be enforced must be named, not dropped.
            {"not", R"cx({"not":{"type":"integer"}})cx"},
            {"patternProperties", R"cx({"type":"object","patternProperties":{"^a":{}}})cx"},
            {"multipleOf", R"cx({"type":"integer","multipleOf":3})cx"},
            {"bounds without type", R"cx({"minimum":2})cx"},
            {"minLength without type", R"cx({"minLength":2})cx"},
            {"minItems without type", R"cx({"minItems":2})cx"},
            {"bounds on number", R"cx({"type":"number","minimum":0,"maximum":9})cx"},
            {"required without properties",
             R"cx({"type":"object","required":["a"],"properties":{"b":{"type":"integer"}}})cx"},
            {"const with sibling assertion",
             R"cx({"const":"a","type":"string","minLength":1})cx"},
            {"unknown format", R"cx({"type":"string","format":"klingon"})cx"},
            {"tuple with minItems",
             R"cx({"type":"array","prefixItems":[{"type":"integer"}],"minItems":2})cx"},
            {"enum type mismatch", R"cx({"type":"integer","enum":["a"]})cx"},
        };
        for (const auto& [label, schema] : rejected) {
            bool threw = false;
            try {
                Grammar g = Grammar::json_schema(schema, table);
                (void)g;
            } catch (const std::invalid_argument&) { threw = true; }
            if (!threw) {
                std::cerr << "strictness: schema \"" << label << "\" was accepted\n";
                ++failures;
            }
        }
    }

    // Disjoint oneOf compiles to a faithful union; overlapping alternatives stay rejected.
    failures += run_schema(
        "oneOf_disjoint",
        R"cx({"oneOf":[{"type":"integer","minimum":0,"maximum":9},{"const":"yes","type":"string"}]})cx",
        {"\"yes\"", "7"},
        {{R"cx("no")cx", 1}, {"10", 1}});

    // ---- vocab-independent compile and validation ----------------------------------------
    {
        const std::string gbnf = Grammar::schema_to_gbnf(R"({"type":"integer"})");
        if (gbnf.find("root ::= (\"-\"? integral-part)") == std::string::npos) {
            std::cerr << "schema_to_gbnf: root rule missing from output\n" << gbnf << '\n';
            ++failures;
        }

        const std::string named =
            Grammar::schema_to_gbnf(R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]})",
                                    "param0");
        if (named.find("param0 ::= ") == std::string::npos
            || named.find("param0-x-kv ::= ") == std::string::npos
            || named.find("root ::= ") != std::string::npos) {
            std::cerr << "schema_to_gbnf: root_name did not namespace the rules\n" << named << '\n';
            ++failures;
        }

        bool threw = false;
        try {
            Grammar::schema_to_gbnf(R"({"oneOf":[{"type":"integer"},{"type":"number"}]})");
        } catch (const std::invalid_argument&) { threw = true; }
        if (!threw) {
            std::cerr << "schema_to_gbnf: overlapping oneOf did not throw\n";
            ++failures;
        }

        threw = false;
        try {
            Grammar::schema_to_gbnf("[1,2]");
        } catch (const std::invalid_argument&) { threw = true; }
        if (!threw) {
            std::cerr << "schema_to_gbnf: non-object schema did not throw\n";
            ++failures;
        }

        Grammar::validate_gbnf("root ::= \"a\" | root2\nroot2 ::= [0-9]{1,3}\n");
        Grammar::validate_gbnf("root ::= <[7]>\n");

        struct InvalidGbnf {
            const char* text;
            const char* label;
        };
        const InvalidGbnf invalid[] = {
            {"root ::= missing\n", "undefined rule reference"},
            {"root ::= <eog>\n", "named token element without vocab"},
            {"no := de\n", "malformed operator"},
        };
        for (const auto& bad : invalid) {
            threw = false;
            try {
                Grammar::validate_gbnf(bad.text);
            } catch (const std::invalid_argument&) { threw = true; }
            if (!threw) {
                std::cerr << "validate_gbnf: " << bad.label << " was accepted\n";
                ++failures;
            }
        }
    }

    // ---- token acceptance, probing, and state snapshots ----------------------------------
    {
        const GrammarTokenTable table = make_table();
        Grammar grammar               = Grammar::json_schema(R"({"type":"integer"})", table);

        // would_accept is a non-mutating probe.
        if (!grammar.would_accept(char_id('4')) || !grammar.would_accept(char_id('-'))) {
            std::cerr << "would_accept: digit or minus rejected in fresh integer grammar\n";
            ++failures;
        }
        if (grammar.would_accept(char_id('x'))) {
            std::cerr << "would_accept: letter accepted in integer grammar\n";
            ++failures;
        }
        if (grammar.would_accept(kEogId)) {
            std::cerr << "would_accept: EOG accepted while unsatisfied\n";
            ++failures;
        }
        if (grammar.would_accept(97)) { // special token: empty piece
            std::cerr << "would_accept: special token accepted\n";
            ++failures;
        }
        const auto mask_before = grammar.allowed_mask();
        if (!grammar.would_accept(char_id('4')) || grammar.allowed_mask() != mask_before) {
            std::cerr << "would_accept: probing mutated the grammar state\n";
            ++failures;
        }

        grammar.accept_token(char_id('4'));
        grammar.accept_token(char_id('2'));
        if (!grammar.satisfied()) {
            std::cerr << "accept_token: grammar unsatisfied after a complete integer\n";
            ++failures;
        }
        if (!grammar.would_accept(kEogId)) {
            std::cerr << "would_accept: EOG rejected while satisfied\n";
            ++failures;
        }
        grammar.accept_token(kEogId);

        // A token no stack accepts is reported by the probe; committing it throws and kills
        // the grammar (every token, including end-of-generation, is rejected from then on).
        const auto mask_after_eog = grammar.allowed_mask();
        if (grammar.would_accept(char_id('x'))) {
            std::cerr << "would_accept: token accepted after EOG\n";
            ++failures;
        }
        bool threw = false;
        try {
            grammar.accept_token(char_id('x'));
        } catch (const std::runtime_error&) { threw = true; }
        if (!threw) {
            std::cerr << "accept_token: committed a rejected token without throwing\n";
            ++failures;
        }
        for (const std::uint32_t word : grammar.allowed_mask()) {
            if (word != 0) {
                std::cerr << "accept_token: dead grammar still allows tokens\n";
                ++failures;
                break;
            }
        }
        if (grammar.satisfied() || grammar.would_accept(kEogId)) {
            std::cerr << "accept_token: dead grammar still reports a live state\n";
            ++failures;
        }
    }

    // accept_token refuses EOG while a partial UTF-8 sequence is pending; satisfied() agrees.
    {
        const GrammarTokenTable table = make_table();
        Grammar grammar               = Grammar::json_schema(R"({"type":"string"})", table);
        grammar.accept_bytes("\"");
        grammar.accept_bytes("\xC3"); // first byte of the two-byte token piece
        if (grammar.satisfied() || grammar.would_accept(kEogId)) {
            std::cerr << "utf8: satisfied or EOG allowed with a pending partial sequence\n";
            ++failures;
        }
        grammar.accept_bytes("\xA9");
        grammar.accept_bytes("\"");
        if (!grammar.satisfied() || !grammar.would_accept(kEogId)) {
            std::cerr << "utf8: completion did not restore satisfied/EOG state\n";
            ++failures;
        }
    }

    // Copies are independent snapshots; move commits a probe branch.
    {
        const GrammarTokenTable table = make_table();
        Grammar base                  = Grammar::json_schema(R"({"type":"string"})", table);
        base.accept_bytes("\"");

        Grammar probe = base.clone();
        probe.accept_bytes("hi\"");
        if (!probe.satisfied()) {
            std::cerr << "clone: probe did not advance independently\n";
            ++failures;
        }
        if (base.satisfied()) {
            std::cerr << "clone: base state leaked into the snapshot source\n";
            ++failures;
        }
        if (!base.would_accept(char_id('h'))) {
            std::cerr << "clone: base lost its open-string state\n";
            ++failures;
        }

        Grammar copy_assigned = Grammar::json_schema(R"({"type":"integer"})", table);
        copy_assigned = base;
        copy_assigned.accept_bytes("zz\"");
        if (!copy_assigned.satisfied()) {
            std::cerr << "clone: copy assignment did not take ownership of pushed state\n";
            ++failures;
        }
        if (base.satisfied()) {
            std::cerr << "clone: copy assignment shared pushdown state\n";
            ++failures;
        }

        Grammar committed = std::move(probe); // move keeps the advanced state
        if (!committed.satisfied()) {
            std::cerr << "move: moved-from probe lost its advanced state\n";
            ++failures;
        }
    }

    // Mask word count and trailing-bit discipline for n_vocab = 100.
    {
        const GrammarTokenTable table = make_table();
        Grammar grammar               = Grammar::json_schema(R"({"type":"integer"})", table);
        const auto mask               = grammar.allowed_mask();
        if (mask.size() != 4) {
            std::cerr << "mask: expected 4 words for n_vocab=100, got " << mask.size() << '\n';
            ++failures;
        }
        if ((mask.back() & ~((1U << 4) - 1U)) != 0) {
            std::cerr << "mask: trailing bits past n_vocab are not zero\n";
            ++failures;
        }
        const TokenId rejected_id = char_id('x'); // letters cannot start an integer
        if ((mask[static_cast<std::size_t>(rejected_id) / 32] >> (rejected_id % 32)) & 1U) {
            std::cerr << "mask: 'x' allowed in an integer-only grammar\n";
            ++failures;
        }
    }

    std::cout << (failures ? "FAIL" : "OK") << " grammar\n";
    return failures ? 1 : 0;
}
