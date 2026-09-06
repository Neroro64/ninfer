#include "json-schema-to-grammar.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

static std::string string_repeat(const std::string & s, size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; i++) {
        out += s;
    }
    return out;
}

static std::vector<std::string> string_split(const std::string & s, const std::string & delim) {
    std::vector<std::string> parts;
    size_t begin = 0;
    while (true) {
        const size_t pos = s.find(delim, begin);
        if (pos == std::string::npos) {
            parts.push_back(s.substr(begin));
            return parts;
        }
        parts.push_back(s.substr(begin, pos - begin));
        begin = pos + delim.size();
    }
}

static std::string string_join(const std::vector<std::string> & v, const std::string & delim) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        if (i > 0) {
            out += delim;
        }
        out += v[i];
    }
    return out;
}

static std::string build_repetition(const std::string & item_rule, int min_items, int max_items, const std::string & separator_rule = "") {
    auto has_max = max_items != std::numeric_limits<int>::max();

    if (max_items == 0) {
        return "";
    }
    if (min_items == 0 && max_items == 1) {
        return item_rule + "?";
    }

    if (separator_rule.empty()) {
        if (min_items == 1 && !has_max) {
            return item_rule + "+";
        }
        if (min_items == 0 && !has_max) {
            return item_rule + "*";
        }
        return item_rule + "{" + std::to_string(min_items) + "," + (has_max ? std::to_string(max_items) : "") + "}";
    }

    auto result = item_rule + " " + build_repetition("(" + separator_rule + " " + item_rule + ")", min_items == 0 ? 0 : min_items - 1, has_max ? max_items - 1 : max_items);
    if (min_items == 0) {
        result = "(" + result + ")?";
    }
    return result;
}

static void build_min_max_int(int64_t min_value, int64_t max_value, std::stringstream & out, int decimals_left = 16, bool top_level = true) {
    auto has_min = min_value != std::numeric_limits<int64_t>::min();
    auto has_max = max_value != std::numeric_limits<int64_t>::max();

    auto digit_range = [&](char from, char to) {
        out << "[";
        if (from == to) {
            out << from;
        } else {
            out << from << "-" << to;
        }
        out << "]";
    };
    auto more_digits = [&](int min_digits, int max_digits) {
        out << "[0-9]";
        if (min_digits == max_digits && min_digits == 1) {
            return;
        }
        out << "{";
        out << min_digits;
        if (max_digits != min_digits) {
            out << ",";
            if (max_digits != std::numeric_limits<int>::max()) {
                out << max_digits;
            }
        }
        out << "}";
    };
    std::function<void(const std::string_view &, const std::string_view &)> uniform_range =
        [&](const std::string_view & from, const std::string_view & to) {
            size_t i = 0;
            while (i < from.length() && i < to.length() && from[i] == to[i]) {
                i++;
            }
            if (i > 0) {
                out << "\"" << from.substr(0, i) << "\"";
            }
            if (i < from.length() && i < to.length()) {
                if (i > 0) {
                    out << " ";
                }
                auto sub_len = from.length() - i - 1;
                if (sub_len > 0) {
                    auto from_sub = from.substr(i + 1);
                    auto to_sub = to.substr(i + 1);
                    auto sub_zeros = string_repeat("0", sub_len);
                    auto sub_nines = string_repeat("9", sub_len);

                    auto to_reached = false;
                    out << "(";
                    if (from_sub == sub_zeros) {
                        digit_range(from[i], to[i] - 1);
                        out << " ";
                        more_digits(sub_len, sub_len);
                    } else {
                        out << "[" << from[i] << "] ";
                        out << "(";
                        uniform_range(from_sub, sub_nines);
                        out << ")";
                        if (from[i] < to[i] - 1) {
                            out << " | ";
                            if (to_sub == sub_nines) {
                                digit_range(from[i] + 1, to[i]);
                                to_reached = true;
                            } else {
                                digit_range(from[i] + 1, to[i] - 1);
                            }
                            out << " ";
                            more_digits(sub_len, sub_len);
                        }
                    }
                    if (!to_reached) {
                        out << " | ";
                        digit_range(to[i], to[i]);
                        out << " ";
                        uniform_range(sub_zeros, to_sub);
                    }
                    out << ")";
                } else {
                    out << "[" << from[i] << "-" << to[i] << "]";
                }
            }
        };

    if (has_min && has_max) {
        if (min_value < 0 && max_value < 0) {
            out << "\"-\" (";
            build_min_max_int(-max_value, -min_value, out, decimals_left, /* top_level= */ true);
            out << ")";
            return;
        }

        if (min_value < 0) {
            out << "\"-\" (";
            build_min_max_int(0, -min_value, out, decimals_left, /* top_level= */ true);
            out << ") | ";
            min_value = 0;
        }

        auto min_s = std::to_string(min_value);
        auto max_s = std::to_string(max_value);
        auto min_digits = min_s.length();
        auto max_digits = max_s.length();

        for (auto digits = min_digits; digits < max_digits; digits++) {
            uniform_range(min_s, string_repeat("9", digits));
            min_s = "1" + string_repeat("0", digits);
            out << " | ";
        }
        uniform_range(min_s, max_s);
        return;
    }

    auto less_decimals = std::max(decimals_left - 1, 1);

    if (has_min) {
        if (min_value < 0) {
            out << "\"-\" (";
            build_min_max_int(std::numeric_limits<int64_t>::min(), -min_value, out, decimals_left, /* top_level= */ false);
            out << ") | [0] | [1-9] ";
            more_digits(0, decimals_left - 1);
        } else if (min_value == 0) {
            if (top_level) {
                out << "[0] | [1-9] ";
                more_digits(0, less_decimals);
            } else {
                more_digits(1, decimals_left);
            }
        } else if (min_value <= 9) {
            char c = '0' + min_value;
            auto range_start = top_level ? '1' : '0';
            if (c > range_start) {
                digit_range(range_start, c - 1);
                out << " ";
                more_digits(1, less_decimals);
                out << " | ";
            }
            digit_range(c, '9');
            out << " ";
            more_digits(0, less_decimals);
        } else {
            auto min_s = std::to_string(min_value);
            auto len = min_s.length();
            auto c = min_s[0];

            if (c > '1') {
                digit_range(top_level ? '1' : '0', c - 1);
                out << " ";
                more_digits(len, less_decimals);
                out << " | ";
            }
            digit_range(c, c);
            out << " (";
            build_min_max_int(std::stoll(min_s.substr(1)), std::numeric_limits<int64_t>::max(), out, less_decimals, /* top_level= */ false);
            out << ")";
            if (c < '9') {
                out << " | ";
                digit_range(c + 1, '9');
                out << " ";
                more_digits(len - 1, less_decimals);
            }
        }
        return;
    }

    if (has_max) {
        if (max_value >= 0) {
            if (top_level) {
                out << "\"-\" [1-9] ";
                more_digits(0, less_decimals);
                out << " | ";
            }
            build_min_max_int(0, max_value, out, decimals_left, /* top_level= */ true);
        } else {
            out << "\"-\" (";
            build_min_max_int(-max_value, std::numeric_limits<int64_t>::max(), out, decimals_left, /* top_level= */ false);
            out << ")";
        }
        return;
    }

    throw std::runtime_error("At least one of min_value or max_value must be set");
}

const std::string SPACE_RULE = "| \" \" | \"\\n\"{1,2} [ \\t]{0,20}";

struct BuiltinRule {
    std::string content;
    std::vector<std::string> deps;
};

static std::unordered_map<std::string, BuiltinRule> PRIMITIVE_RULES = {
    {"boolean", {"(\"true\" | \"false\")", {}}},
    {"decimal-part", {"[0-9]{1,16}", {}}},
    {"integral-part", {"[0] | [1-9] [0-9]{0,15}", {}}},
    {"number", {"(\"-\"? integral-part) (\".\" decimal-part)? ([eE] [-+]? integral-part)?", {"integral-part", "decimal-part"}}},
    {"integer", {"(\"-\"? integral-part)", {"integral-part"}}},
    {"value", {"object | array | string | number | boolean | null", {"object", "array", "string", "number", "boolean", "null"}}},
    {"object", {"\"{\" space ( string \":\" space value (\",\" space string \":\" space value)* )? space \"}\"", {"string", "value"}}},
    {"array", {"\"[\" space ( value (\",\" space value)* )? space \"]\"", {"value"}}},
    {"uuid", {"\"\\\"\" [0-9a-fA-F]{8} \"-\" [0-9a-fA-F]{4} \"-\" [0-9a-fA-F]{4} \"-\" [0-9a-fA-F]{4} \"-\" [0-9a-fA-F]{12} \"\\\"\"", {}}},
    {"char",   {"[^\"\\\\\\x7F\\x00-\\x1F] | [\\\\] ([\"\\\\bfnrt] | \"u\" [0-9a-fA-F]{4})", {}}},
    {"string", {"\"\\\"\" char* \"\\\"\"", {"char"}}},
    {"null", {"\"null\"", {}}},
};

static std::unordered_map<std::string, BuiltinRule> STRING_FORMAT_RULES = {
    {"date", {"[0-9]{4} \"-\" ( \"0\" [1-9] | \"1\" [0-2] ) \"-\" ( \"0\" [1-9] | [1-2] [0-9] | \"3\" [0-1] )", {}}},
    {"time", {"([01] [0-9] | \"2\" [0-3]) \":\" [0-5] [0-9] \":\" [0-5] [0-9] ( \".\" [0-9]{3} )? ( \"Z\" | ( \"+\" | \"-\" ) ( [01] [0-9] | \"2\" [0-3] ) \":\" [0-5] [0-9] )", {}}},
    {"date-time", {"date \"T\" time", {"date", "time"}}},
    {"date-string", {"\"\\\"\" date \"\\\"\"", {"date"}}},
    {"time-string", {"\"\\\"\" time \"\\\"\"", {"time"}}},
    {"date-time-string", {"\"\\\"\" date-time \"\\\"\"", {"date-time"}}}
};

static bool is_reserved_name(const std::string & name) {
    static const std::unordered_set<std::string> RESERVED_NAMES = [] {
        std::unordered_set<std::string> s;
        s.insert("root");
        for (const auto & p : PRIMITIVE_RULES) {
            s.insert(p.first);
        }
        for (const auto & p : STRING_FORMAT_RULES) {
            s.insert(p.first);
        }
        return s;
    }();
    return RESERVED_NAMES.find(name) != RESERVED_NAMES.end();
}

static std::regex INVALID_RULE_CHARS_RE("[^a-zA-Z0-9-]+");
static std::regex GRAMMAR_LITERAL_ESCAPE_RE("[\r\n\"\\\\]");
static std::regex GRAMMAR_RANGE_LITERAL_ESCAPE_RE("[\r\n\"\\]\\-\\\\]");
static std::unordered_map<char, std::string> GRAMMAR_LITERAL_ESCAPES = {
    {'\r', "\\r"}, {'\n', "\\n"}, {'"', "\\\""}, {'-', "\\-"}, {']', "\\]"}, {'\\', "\\\\"}
};

static const int MAX_PATTERN_DEPTH = 100;

static std::unordered_set<char> NON_LITERAL_SET = {'|', '.', '(', ')', '[', ']', '{', '}', '*', '+', '?', '^', '$'};
static std::unordered_set<char> ESCAPED_IN_REGEXPS_BUT_NOT_IN_LITERALS = {'^', '$', '.', '[', ']', '(', ')', '|', '{', '}', '*', '+', '?'};

// NInfer policy: a schema is compiled exactly or rejected. Keywords whose constraint has no
// faithful grammar equivalent must fail conversion instead of being silently dropped, because
// a dropped assertion broadens what the model may emit.

// Annotation/meta keywords: no runtime effect on validity; safe to ignore.
static const std::unordered_set<std::string> META_KEYWORDS = {
    "$schema", "$id", "$comment", "$anchor", "$vocabulary", "$defs", "definitions",
    "title", "description", "default", "deprecated", "examples", "readOnly", "writeOnly",
    "contentMediaType", "contentEncoding", "contentSchema",
};

// Assertion keywords with no exact grammar representation: presence is a conversion error.
static const std::unordered_set<std::string> UNSUPPORTED_KEYWORDS = {
    "not", "if", "then", "else",
    "unevaluatedProperties", "unevaluatedItems", "patternProperties", "propertyNames",
    "dependentSchemas", "dependentRequired", "dependencies",
    "contains", "minContains", "maxContains",
    "uniqueItems", "minProperties", "maxProperties",
};

static std::string replacePattern(const std::string & input, const std::regex & regex, const std::function<std::string(const std::smatch  &)> & replacement) {
    std::smatch match;
    std::string result;

    std::string::const_iterator searchStart(input.cbegin());
    std::string::const_iterator searchEnd(input.cend());

    while (std::regex_search(searchStart, searchEnd, match, regex)) {
        result.append(searchStart, searchStart + match.position());
        result.append(replacement(match));
        searchStart = match.suffix().first;
    }

    result.append(searchStart, searchEnd);

    return result;
}

static std::string format_literal(const std::string & literal) {
    std::string escaped = replacePattern(literal, GRAMMAR_LITERAL_ESCAPE_RE, [&](const std::smatch & match) {
        char c = match.str()[0];
        return GRAMMAR_LITERAL_ESCAPES.at(c);
    });
    return "\"" + escaped + "\"";
}


static size_t gbnf_escape_length(const std::string & pattern, size_t pos) {
    if (pos + 1 >= pattern.length() || pattern[pos] != '\\') {
        return 0;
    }
    size_t n_hex = 0;
    switch (pattern[pos + 1]) {
        case 'x': n_hex = 2; break;
        case 'u': n_hex = 4; break;
        case 'U': n_hex = 8; break;
        case 't': case 'r': case 'n': case '\\': case '"': case '[': case ']':
            return 2;
        default:
            return 0;
    }
    if (pos + 2 + n_hex > pattern.length()) {
        return 0;
    }
    for (size_t i = pos + 2; i < pos + 2 + n_hex; i++) {
        char h = pattern[i];
        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) {
            return 0;
        }
    }
    return 2 + n_hex;
}

class common_schema_converter {
private:
    std::function<json(const std::string &)> _fetch_json;
    bool _dotall;
    std::map<std::string, std::string> _rules;
    std::unordered_map<std::string, json> _refs;
    std::unordered_set<std::string> _refs_being_resolved;
    std::vector<std::string> _errors;
    std::vector<std::string> _warnings;

    std::string _add_rule(const std::string & name, const std::string & rule) {
        std::string esc_name = regex_replace(name, INVALID_RULE_CHARS_RE, "-");
        if (_rules.find(esc_name) == _rules.end() || _rules[esc_name] == rule) {
            _rules[esc_name] = rule;
            return esc_name;
        }
        int i = 0;
        while (_rules.find(esc_name + std::to_string(i)) != _rules.end() && _rules[esc_name + std::to_string(i)] != rule) {
            i++;
        }
        std::string key = esc_name + std::to_string(i);
        _rules[key] = rule;
        return key;
    }

    std::string _generate_union_rule(const std::string & name, const std::vector<json> & alt_schemas) {
        std::vector<std::string> rules;
        rules.reserve(alt_schemas.size());
        for (size_t i = 0; i < alt_schemas.size(); i++) {
            rules.push_back(visit(alt_schemas[i], name + (name.empty() ? "alternative-" : "-") + std::to_string(i)));
        }
        return string_join(rules, " | ");
    }

    // thrown when the pattern is a valid regex with no grammar equivalent
    struct unsupported_pattern : public std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // thrown when the pattern is not a valid regex
    struct invalid_pattern : public std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    std::string _visit_pattern(const std::string & pattern, const std::string & name) {
        auto rules_snapshot = _rules;
        try {
            return _pattern_to_rule(pattern, name);
        } catch (const unsupported_pattern & err) {
            // revert rules
            _rules = std::move(rules_snapshot);
            _warnings.push_back("pattern " + pattern + " is not supported (" + err.what() + "), accepting any string");
            return _add_rule(name, _add_primitive("string", PRIMITIVE_RULES.at("string")));
        } catch (const invalid_pattern & err) {
            _rules = std::move(rules_snapshot);
            _errors.push_back("Invalid pattern " + pattern + ": " + err.what());
            return "";
        }
    }

    std::string _pattern_to_rule(const std::string & pattern, const std::string & name) {
        if (pattern.length() < 2 || pattern.front() != '^' || pattern.back() != '$') {
            throw unsupported_pattern("not anchored with '^' and '$'");
        }
        std::string sub_pattern = pattern.substr(1, pattern.length() - 2);
        std::unordered_map<std::string, std::string> sub_rule_ids;

        size_t i = 0;
        size_t length = sub_pattern.length();
        int paren_depth = 0;

        using literal_or_rule = std::pair<std::string, bool>;
        auto to_rule = [&](const literal_or_rule & ls) {
            auto is_literal = ls.second;
            auto s = ls.first;
            return is_literal ? "\"" + s + "\"" : s;
        };
        std::function<literal_or_rule()> transform = [&]() -> literal_or_rule {
            std::vector<literal_or_rule> seq;

            auto get_dot = [&]() {
                std::string rule;
                if (_dotall) {
                    rule = "[\\U00000000-\\U0010FFFF]";
                } else {
                    rule = "[^\\x0A\\x0D]";
                }
                return _add_rule("dot", rule);
            };

            // Joins the sequence, merging consecutive literals together.
            auto join_seq = [&]() {
                std::vector<literal_or_rule> ret;

                std::string literal;
                auto flush_literal = [&]() {
                    if (literal.empty()) {
                        return false;
                    }
                    ret.emplace_back(literal, true);
                    literal.clear();
                    return true;
                };

                for (const auto & item : seq) {
                    auto is_literal = item.second;
                    if (is_literal) {
                        literal += item.first;
                    } else {
                        flush_literal();
                        ret.push_back(item);
                    }
                }
                flush_literal();

                std::vector<std::string> results;
                results.reserve(ret.size());
                for (const auto & item : ret) {
                    results.push_back(to_rule(item));
                }
                return std::make_pair(string_join(results, " "), false);
            };

            while (i < length) {
                char c = sub_pattern[i];
                if (c == '.') {
                    seq.emplace_back(get_dot(), false);
                    i++;
                } else if (c == '(') {
                    i++;
                    if (i < length && sub_pattern[i] == '?') {
                        if (i + 1 < length && sub_pattern[i + 1] == ':') {
                            i += 2; // skip "?:" for non-capturing group, treat as regular group
                        } else {
                            // lookaround, named group, inline flags, ...
                            throw unsupported_pattern("unsupported group syntax");
                        }
                    }
                    paren_depth++;
                    if (paren_depth > MAX_PATTERN_DEPTH) {
                        throw unsupported_pattern("pattern nesting too deep");
                    }
                    seq.emplace_back("(" + to_rule(transform()) + ")", false);
                } else if (c == ')') {
                    i++;
                    if (paren_depth == 0) {
                        throw invalid_pattern("unbalanced parentheses");
                    }
                    paren_depth--;
                    return join_seq();
                } else if (c == '^' || c == '$') {
                    throw unsupported_pattern("anchor inside the pattern");
                } else if (c == '[') {
                    std::string square_brackets = std::string(1, c);
                    i++;
                    while (i < length && sub_pattern[i] != ']') {
                        if (sub_pattern[i] == '\\') {
                            auto escape_length = gbnf_escape_length(sub_pattern, i);
                            if (escape_length == 0) {
                                throw unsupported_pattern("unsupported escape in character class: " + sub_pattern.substr(i, 2));
                            }
                            square_brackets += sub_pattern.substr(i, escape_length);
                            i += escape_length;
                        } else {
                            square_brackets += sub_pattern[i];
                            i++;
                        }
                    }
                    if (i >= length) {
                        throw invalid_pattern("unterminated character class");
                    }
                    square_brackets += ']';
                    i++;
                    seq.emplace_back(square_brackets, false);
                } else if (c == '|') {
                    seq.emplace_back("|", false);
                    i++;
                } else if (c == '*' || c == '+' || c == '?') {
                    if (seq.empty()) {
                        throw invalid_pattern("nothing to repeat");
                    }
                    seq.back() = std::make_pair(to_rule(seq.back()) + c, false);
                    i++;
                } else if (c == '{') {
                    std::string curly_brackets = std::string(1, c);
                    i++;
                    while (i < length && sub_pattern[i] != '}') {
                        curly_brackets += sub_pattern[i];
                        i++;
                    }
                    if (i >= length) {
                        throw unsupported_pattern("unterminated curly brackets");
                    }
                    curly_brackets += '}';
                    i++;
                    auto nums = string_split(curly_brackets.substr(1, curly_brackets.length() - 2), ",");
                    int min_times = 0;
                    int max_times = std::numeric_limits<int>::max();
                    if (nums.size() != 1 && nums.size() != 2) {
                        throw unsupported_pattern("wrong number of values in curly brackets");
                    }
                    try {
                        if (nums.size() == 1) {
                            min_times = max_times = std::stoi(nums[0]);
                        } else {
                            if (!nums[0].empty()) {
                                min_times = std::stoi(nums[0]);
                            }
                            if (!nums[1].empty()) {
                                max_times = std::stoi(nums[1]);
                            }
                        }
                    } catch (const std::logic_error &) {
                        throw unsupported_pattern("invalid number in curly brackets");
                    }
                    if (seq.empty()) {
                        throw invalid_pattern("nothing to repeat");
                    }
                    auto &last = seq.back();
                    auto &sub = last.first;
                    auto sub_is_literal = last.second;

                    if (!sub_is_literal) {
                        std::string & sub_id = sub_rule_ids[sub];
                        if (sub_id.empty()) {
                            sub_id = _add_rule(name + "-" + std::to_string(sub_rule_ids.size()), sub);
                        }
                        sub = sub_id;
                    }
                    seq.back().first = build_repetition(
                        sub_is_literal ? "\"" + sub + "\"" : sub,
                        min_times,
                        max_times,
                        ""
                    );
                    seq.back().second = false;
                } else {
                    std::string literal;
                    auto is_non_literal = [&](char c) {
                        return NON_LITERAL_SET.find(c) != NON_LITERAL_SET.end();
                    };
                    while (i < length) {
                        if (sub_pattern[i] == '\\') {
                            if (i == length - 1) {
                                throw invalid_pattern("trailing backslash");
                            }
                            char next = sub_pattern[i + 1];
                            if (ESCAPED_IN_REGEXPS_BUT_NOT_IN_LITERALS.find(next) != ESCAPED_IN_REGEXPS_BUT_NOT_IN_LITERALS.end()) {
                                i++;
                                literal += sub_pattern[i];
                                i++;
                            } else {
                                auto escape_length = gbnf_escape_length(sub_pattern, i);
                                if (escape_length == 0) {
                                    throw unsupported_pattern("unsupported escape: " + sub_pattern.substr(i, 2));
                                }
                                literal += sub_pattern.substr(i, escape_length);
                                i += escape_length;
                            }
                        } else if (sub_pattern[i] == '"') {
                            literal += "\\\"";
                            i++;
                        } else if (!is_non_literal(sub_pattern[i]) &&
                                (i == length - 1 || literal.empty() || sub_pattern[i + 1] == '.' || !is_non_literal(sub_pattern[i + 1]))) {
                            literal += sub_pattern[i];
                            i++;
                        } else {
                            break;
                        }
                    }
                    if (literal.empty()) { // nothing was consumed, ex. a stray ']' or '}'
                        throw unsupported_pattern(std::string("unsupported character: ") + c);
                    }
                    seq.emplace_back(literal, true);
                }
            }
            return join_seq();
        };

        auto rule = to_rule(transform());
        if (paren_depth != 0) {
            throw invalid_pattern("unbalanced parentheses");
        }

        return _add_rule(name, "\"\\\"\" (" + rule + ") \"\\\"\"");
    }

    /*
        Returns a rule that matches a JSON string that is none of the provided strings

        not_strings({"a"})
            -> ["] ( [a] char+ | [^"a] char* )? ["]
        not_strings({"and", "also"})
            -> ["] ( [a] ([l] ([s] ([o] char+ | [^"o] char*) | [^"s] char*) | [n] ([d] char+ | [^"d] char*) | [^"ln] char*) | [^"a] char* )? ["]
    */
    std::string _not_strings(const std::vector<std::string> & strings) {

        struct TrieNode {
            std::map<char, TrieNode> children;
            bool is_end_of_string;

            TrieNode() : is_end_of_string(false) {}

            void insert(const std::string & string) {
                auto *node = this;
                for (char c : string) {
                    node = &node->children[c];
                }
                node->is_end_of_string = true;
            }
        };

        TrieNode trie;
        for (const auto & s : strings) {
            trie.insert(s);
        }

        std::string char_rule = _add_primitive("char", PRIMITIVE_RULES.at("char"));
        std::ostringstream out;
        out << "[\"] ( ";
        std::function<void(const TrieNode &)> visit = [&](const TrieNode & node) {
            std::ostringstream rejects;
            auto first = true;
            for (const auto & kv : node.children) {
                rejects << kv.first;
                if (first) {
                    first = false;
                } else {
                    out << " | ";
                }
                out << "[" << kv.first << "]";
                if (!kv.second.children.empty()) {
                    out << " (";
                    visit(kv.second);
                    out << ")";
                } else if (kv.second.is_end_of_string) {
                    out << " " << char_rule << "+";
                }
            }
            if (!node.children.empty()) {
                if (!first) {
                    out << " | ";
                }
                out << "[^\"" << rejects.str() << "] " << char_rule << "*";
            }
        };
        visit(trie);

        out << " )";
        if (!trie.is_end_of_string) {
            out << "?";
        }
        out << " [\"]";
        return out.str();
    }

    std::string _resolve_ref(const std::string & ref) {
        auto it = ref.find('#');
        std::string ref_fragment = it != std::string::npos ? ref.substr(it + 1) : ref;
        static const std::regex nonalphanumeric_regex(R"([^a-zA-Z0-9-]+)");
        std::string ref_name = "ref" + std::regex_replace(ref_fragment, nonalphanumeric_regex, "-");
        if (_rules.find(ref_name) == _rules.end() && _refs_being_resolved.find(ref) == _refs_being_resolved.end()) {
            _refs_being_resolved.insert(ref);
            json resolved = _refs[ref];
            ref_name = visit(resolved, ref_name);
            _refs_being_resolved.erase(ref);
        }
        return ref_name;
    }

    std::string _build_object_rule(
        const std::vector<std::pair<std::string, json>> & properties,
        const std::unordered_set<std::string> & required,
        const std::string & name,
        const json & additional_properties)
    {
        std::vector<std::string> required_props;
        std::vector<std::string> optional_props;
        std::unordered_map<std::string, std::string> prop_kv_rule_names;
        std::vector<std::string> prop_names;
        for (const auto & kv : properties) {
            const auto &prop_name = kv.first;
            const auto &prop_schema = kv.second;

            std::string prop_rule_name = visit(prop_schema, name + (name.empty() ? "" : "-") + prop_name);
            prop_kv_rule_names[prop_name] = _add_rule(
                name + (name.empty() ? "" : "-") + prop_name + "-kv",
                format_literal(json(prop_name).dump()) + " space \":\" space " + prop_rule_name
            );
            if (required.find(prop_name) != required.end()) {
                required_props.push_back(prop_name);
            } else {
                optional_props.push_back(prop_name);
            }
            prop_names.push_back(prop_name);
        }
        if ((additional_properties.is_boolean() && additional_properties.get<bool>()) || additional_properties.is_object()) {
            std::string sub_name = name + (name.empty() ? "" : "-") + "additional";
            std::string value_rule =
                additional_properties.is_object() ? visit(additional_properties, sub_name + "-value")
                : _add_primitive("value", PRIMITIVE_RULES.at("value"));

            auto key_rule =
                prop_names.empty() ? _add_primitive("string", PRIMITIVE_RULES.at("string"))
                : _add_rule(sub_name + "-k", _not_strings(prop_names));
            std::string kv_rule = _add_rule(sub_name + "-kv", key_rule + " \":\" space " + value_rule);
            prop_kv_rule_names["*"] = kv_rule;
            optional_props.push_back("*");
        }

        for (const auto & r : required) {
            if (!prop_kv_rule_names.count(r)) {
                // Silently emitting an empty object here would drop the requirement entirely.
                _errors.push_back("Required property \"" + r + "\" has no schema in \"properties\"; "
                    "declare it in \"properties\" or remove it from \"required\" (object: " + name + ")");
            }
        }
        if (required_props.empty() && optional_props.empty()) {
            return "\"{\" space \"}\"";
        }

        std::string rule = "\"{\" space ";
        for (size_t i = 0; i < required_props.size(); i++) {
            if (i > 0) {
                rule += " \",\" space ";
            }
            rule += prop_kv_rule_names[required_props[i]];
        }

        if (!optional_props.empty()) {
            rule += " (";
            if (!required_props.empty()) {
                rule += " \",\" space ( ";
            }

            std::function<std::string(const std::vector<std::string> &, bool)> get_recursive_refs = [&](const std::vector<std::string> & ks, bool first_is_optional) {
                std::string res;
                if (ks.empty()) {
                    return res;
                }
                const std::string& k = ks[0];
                std::string kv_rule_name = prop_kv_rule_names[k];
                std::string comma_ref = "( \",\" space " + kv_rule_name + " )";
                if (first_is_optional) {
                    res = comma_ref + (k == "*" ? "*" : "?");
                } else {
                    res = kv_rule_name + (k == "*" ? " " + comma_ref + "*" : "");
                }
                if (ks.size() > 1) {
                    res += " " + _add_rule(
                        name + (name.empty() ? "" : "-") + k + "-rest",
                        get_recursive_refs(std::vector<std::string>(ks.begin() + 1, ks.end()), true)
                    );
                }
                return res;
            };

            for (size_t i = 0; i < optional_props.size(); i++) {
                if (i > 0) {
                    rule += " | ";
                }
                rule += get_recursive_refs(std::vector<std::string>(optional_props.begin() + i, optional_props.end()), false);
            }
            if (!required_props.empty()) {
                rule += " )";
            }
            rule += " )?";
        }

        rule += " space \"}\"";

        return rule;
    }

    std::string _add_primitive(const std::string & name, const BuiltinRule & rule) {
        auto n = _add_rule(name, rule.content);
        for (const auto & dep : rule.deps) {
            BuiltinRule dep_rule;
            auto it = PRIMITIVE_RULES.find(dep);
            if (it == PRIMITIVE_RULES.end()) {
                it = STRING_FORMAT_RULES.find(dep);
                if (it == STRING_FORMAT_RULES.end()) {
                    _errors.push_back("Rule " + dep + " not known");
                    continue;
                }
            }
            if (_rules.find(dep) == _rules.end()) {
                _add_primitive(dep, it->second);
            }
        }
        return n;
    }

public:
    common_schema_converter(
        const std::function<json(const std::string &)> & fetch_json,
        bool dotall)
          : _fetch_json(fetch_json), _dotall(dotall)
    {
        _rules["space"] = SPACE_RULE;
    }

    void resolve_refs(json & schema, const std::string & url) {
        /*
        * Resolves all $ref fields in the given schema, fetching any remote schemas,
        * replacing each $ref with absolute reference URL and populates _refs with the
        * respective referenced (sub)schema dictionaries.
        */
        std::function<void(json &)> visit_refs = [&](json & n) {
            if (n.is_array()) {
                for (auto & x : n) {
                    visit_refs(x);
                }
            } else if (n.is_object()) {
                if (n.contains("$ref")) {
                    std::string ref = n["$ref"];
                    if (_refs.find(ref) == _refs.end()) {
                        json target;
                        if (ref.find("https://") == 0) {
                            std::string base_url = ref.substr(0, ref.find('#'));
                            auto it = _refs.find(base_url);
                            if (it != _refs.end()) {
                                target = it->second;
                            } else {
                                // Fetch the referenced schema and resolve its refs
                                auto referenced = _fetch_json(ref);
                                resolve_refs(referenced, base_url);
                                _refs[base_url] = referenced;
                            }
                            if (ref.find('#') == std::string::npos || ref.substr(ref.find('#') + 1).empty()) {
                                return;
                            }
                        } else if (ref.find("#/") == 0) {
                            target = schema;
                            n["$ref"] = url + ref;
                            ref = url + ref;
                        } else {
                            _errors.push_back("Unsupported ref: " + ref);
                            return;
                        }
                        std::string pointer = ref.substr(ref.find('#') + 1);
                        std::vector<std::string> tokens = string_split(pointer, "/");
                        for (size_t i = 1; i < tokens.size(); ++i) {
                            const std::string& sel = tokens[i];
                            if (target.is_object() && target.contains(sel)) {
                                target = target[sel];
                            } else if (target.is_array()) {
                                size_t sel_index;
                                try {
                                    sel_index = std::stoull(sel);
                                } catch (const std::invalid_argument & e) {
                                    sel_index = target.size();
                                }
                                if (sel_index >= target.size()) {
                                    _errors.push_back("Error resolving ref " + ref + ": " + sel + " not in " + target.dump());
                                    return;
                                }
                                target = target[sel_index];
                            } else {
                                _errors.push_back("Error resolving ref " + ref + ": " + sel + " not in " + target.dump());
                                return;
                            }
                        }
                        _refs[ref] = target;
                    }
                } else {
                    for (const auto & kv : n.items()) {
                        visit_refs(kv.value());
                    }
                }
            }
        };

        visit_refs(schema);
    }

    static std::string _generate_constant_rule(const json & value) {
        return format_literal(value.dump());
    }

    // ---- strictness helpers -------------------------------------------------------------
    //
    // The converter either compiles a schema exactly or records an error in _errors (which
    // json_schema_to_grammar turns into a thrown std::invalid_argument). It never drops an
    // applicable assertion silently: a dropped assertion broadens the accepted language and
    // silently weakens the constraint the caller asked for.

    static std::string _json_value_type(const json & v) {
        if (v.is_null()) return "null";
        if (v.is_boolean()) return "boolean";
        if (v.is_number_integer()) return "integer";
        if (v.is_number_float()) return "number";
        if (v.is_string()) return "string";
        if (v.is_array()) return "array";
        return "object";
    }

    // Rejects keywords and keyword/type combinations whose assertion cannot be enforced
    // exactly. Non-applicable assertions (e.g. minLength next to "type": "object") are
    // spec-faithful no-ops and are ignored; meta/annotation keywords are ignored.
    void _check_supported_assertions(const json & schema, const json & schema_type) {
        for (const auto & kw : UNSUPPORTED_KEYWORDS) {
            if (schema.contains(kw)) {
                _errors.push_back("Unsupported JSON Schema keyword \"" + kw + "\": " + schema.dump());
            }
        }

        if (schema.contains("multipleOf")) {
            const auto & m = schema["multipleOf"];
            const bool no_op = m.is_number_integer() && m.get<int64_t>() == 1;
            if (!no_op) {
                _errors.push_back("Unsupported JSON Schema keyword \"multipleOf\": " + schema.dump());
            }
        }

        auto scalar_type = [&]() -> std::optional<std::string> {
            if (schema_type.is_string()) return schema_type.get<std::string>();
            return std::nullopt;
        };

        const bool has_bounds = schema.contains("minimum") || schema.contains("maximum")
            || schema.contains("exclusiveMinimum") || schema.contains("exclusiveMaximum");
        if (has_bounds) {
            const auto t = scalar_type();
            if (t == std::optional<std::string>("number")) {
                _errors.push_back(
                    "Numeric bounds on \"type\": \"number\" are unsupported (only integer bounds are "
                    "enforced exactly; fractional values cannot be constrained by this grammar): " + schema.dump());
            } else if (t == std::optional<std::string>("integer")) {
                for (const char * kw : {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"}) {
                    if (schema.contains(kw) && !schema[kw].is_number()) {
                        _errors.push_back(std::string("JSON Schema keyword \"") + kw + "\" must be a number: " + schema.dump());
                    }
                }
                if ((schema.contains("exclusiveMinimum") && schema["exclusiveMinimum"].is_boolean())
                    || (schema.contains("exclusiveMaximum") && schema["exclusiveMaximum"].is_boolean())) {
                    _errors.push_back(
                        "Draft-4 boolean \"exclusiveMinimum\"/\"exclusiveMaximum\" are unsupported; use the "
                        "numeric draft 2020-12 form: " + schema.dump());
                }
                if (schema.contains("minimum") && schema["minimum"].is_number()
                    && schema.contains("maximum") && schema["maximum"].is_number()
                    && schema["minimum"].get<int64_t>() > schema["maximum"].get<int64_t>()) {
                    _errors.push_back("Empty integer range: minimum exceeds maximum: " + schema.dump());
                }
            } else if (!schema_type.is_string() && !schema_type.is_array()) {
                // No type: bounds would apply to number members the grammar cannot constrain.
                _errors.push_back(
                    "Numeric bounds require an explicit \"type\": \"integer\"; without a type the bounds "
                    "would silently not apply to non-integer values: " + schema.dump());
            }
            // Bounds next to string/boolean/array/object types are spec no-ops: ignored.
        }

        if (schema.contains("minLength") || schema.contains("maxLength")) {
            const auto t = scalar_type();
            if (t == std::optional<std::string>("string")) {
                for (const char * kw : {"minLength", "maxLength"}) {
                    if (schema.contains(kw) && (!schema[kw].is_number_integer() || schema[kw].get<int64_t>() < 0)) {
                        _errors.push_back(std::string("JSON Schema keyword \"") + kw + "\" must be a non-negative integer: " + schema.dump());
                    }
                }
                if (schema.contains("minLength") && schema["minLength"].is_number_integer()
                    && schema.contains("maxLength") && schema["maxLength"].is_number_integer()
                    && schema["minLength"].get<int64_t>() > schema["maxLength"].get<int64_t>()) {
                    _errors.push_back("Empty range: minLength exceeds maxLength: " + schema.dump());
                }
            } else if (!schema_type.is_string() && !schema_type.is_array()) {
                _errors.push_back(
                    "minLength/maxLength require an explicit \"type\": \"string\"; without a type the "
                    "length bound would silently not apply to non-string values: " + schema.dump());
            }
            // Length bounds next to non-string types are spec no-ops: ignored.
        }

        if (schema.contains("minItems") || schema.contains("maxItems")) {
            const auto t = scalar_type();
            const bool has_items = schema.contains("items") || schema.contains("prefixItems");
            if (t == std::optional<std::string>("array") || (schema_type.is_null() && has_items)) {
                if (has_items && ((schema.contains("items") && schema["items"].is_array())
                    || schema.contains("prefixItems"))) {
                    _errors.push_back(
                        "minItems/maxItems with tuple-form items/prefixItems is unsupported; the bound "
                        "would be silently ignored for tuples: " + schema.dump());
                }
                for (const char * kw : {"minItems", "maxItems"}) {
                    if (schema.contains(kw) && (!schema[kw].is_number_integer() || schema[kw].get<int64_t>() < 0)) {
                        _errors.push_back(std::string("JSON Schema keyword \"") + kw + "\" must be a non-negative integer: " + schema.dump());
                    }
                }
                if (schema.contains("minItems") && schema["minItems"].is_number_integer()
                    && schema.contains("maxItems") && schema["maxItems"].is_number_integer()
                    && schema["minItems"].get<int64_t>() > schema["maxItems"].get<int64_t>()) {
                    _errors.push_back("Empty range: minItems exceeds maxItems: " + schema.dump());
                }
            } else if (schema_type.is_null() && !has_items) {
                _errors.push_back(
                    "minItems/maxItems require an explicit \"type\": \"array\"; without a type the bound "
                    "would silently not apply to non-array values: " + schema.dump());
            }
            // Item bounds next to non-array types are spec no-ops: ignored.
        }

        if (schema.contains("enum") && (!schema["enum"].is_array() || schema["enum"].empty())) {
            _errors.push_back("\"enum\" must be a non-empty array: " + schema.dump());
        }

        if (schema.contains("additionalItems") && (schema.contains("items") || schema.contains("prefixItems"))) {
            const json & items = schema.contains("items") ? schema["items"] : schema["prefixItems"];
            const bool forbidden = items.is_array()
                && !(schema["additionalItems"].is_boolean() && !schema["additionalItems"].get<bool>());
            if (forbidden) {
                _errors.push_back(
                    "\"additionalItems\" with array-form items/prefixItems is unsupported; only "
                    "\"additionalItems\": false (which this grammar enforces for tuples) is accepted: " + schema.dump());
            }
        }

        if (schema.contains("format")) {
            const json & f = schema["format"];
            if (!f.is_string() && !f.is_null()) {
                _errors.push_back("\"format\" must be a string: " + schema.dump());
            } else if (f.is_string()) {
                const bool string_context = schema_type.is_null() || schema_type.is_array()
                    || scalar_type() == std::optional<std::string>("string");
                const std::string fmt = f.get<std::string>();
                const bool known = std::regex_match(fmt, std::regex("^uuid[1-5]?$"))
                    || STRING_FORMAT_RULES.find(fmt + "-string") != STRING_FORMAT_RULES.end();
                if (string_context && !known) {
                    _errors.push_back("Unsupported string \"format\": \"" + fmt + "\": " + schema.dump());
                }
                // Formats annotating non-string types are annotations: ignored.
            }
        }

        // const/enum are exact literal constraints; sibling assertions the converter's
        // const/enum branches would drop must not weaken them.
        if (schema.contains("const") || schema.contains("enum")) {
            std::optional<std::string> declared_type = scalar_type();
            std::set<std::string> declared_types;
            bool type_checked = false;
            if (declared_type) {
                declared_types.insert(*declared_type);
                if (*declared_type == "number") declared_types.insert("integer");
                type_checked = true;
            }
            const json * values = schema.contains("const") ? &schema["const"] : &schema["enum"];
            const bool consistent = !type_checked || [&] {
                if (schema.contains("const")) {
                    return declared_types.count(_json_value_type(schema["const"])) > 0;
                }
                for (const auto & v : schema["enum"]) {
                    if (declared_types.count(_json_value_type(v)) == 0) return false;
                }
                return true;
            }();
            if (!consistent) {
                _errors.push_back("const/enum values conflict with the declared \"type\": " + schema.dump());
            }
            for (const auto & kv : schema.items()) {
                if (kv.key() != "const" && kv.key() != "enum" && kv.key() != "type"
                    && !META_KEYWORDS.count(kv.key())) {
                    _errors.push_back("Combining const/enum with assertion \"" + kv.key()
                        + "\" is unsupported; the sibling would be silently ignored: " + schema.dump());
                }
            }
            (void)values;
        }

        // oneOf/anyOf: sibling assertions cannot be intersected with the union here.
        if (schema.contains("oneOf") || schema.contains("anyOf")) {
            for (const auto & kv : schema.items()) {
                if (kv.key() != "oneOf" && kv.key() != "anyOf" && kv.key() != "type"
                    && !META_KEYWORDS.count(kv.key())) {
                    _errors.push_back("Combining oneOf/anyOf with assertion \"" + kv.key()
                        + "\" is unsupported; the sibling would be silently ignored: " + schema.dump());
                }
            }
        }

        // allOf: only the mergeable sibling keys plus the declared type are allowed.
        if (schema.contains("allOf")) {
            const auto t = scalar_type();
            if (t && *t != "object" && *t != "string" && *t != "null") {
                _errors.push_back("\"allOf\" with \"type\": \"" + *t + "\" is unsupported; the components "
                    "would be silently dropped for this type: " + schema.dump());
            }
            for (const auto & kv : schema.items()) {
                if (kv.key() != "allOf" && kv.key() != "properties" && kv.key() != "required"
                    && kv.key() != "type" && !META_KEYWORDS.count(kv.key())) {
                    _errors.push_back("Combining allOf with assertion \"" + kv.key()
                        + "\" is unsupported; the sibling would be silently ignored: " + schema.dump());
                }
            }
        }

        // Structural keywords attached to a type they can never apply to signal a schema bug;
        // silently emitting the bare primitive would drop the author's intent.
        if (schema.contains("properties") || schema.contains("required")
            || (schema.contains("additionalProperties") && !(schema["additionalProperties"].is_boolean() && schema["additionalProperties"].get<bool>()))) {
            const auto t = scalar_type();
            if (t && *t != "object") {
                _errors.push_back("Object keywords combined with \"type\": \"" + *t + "\" are unsupported: " + schema.dump());
            }
        }
        if (schema.contains("items") || schema.contains("prefixItems")) {
            const auto t = scalar_type();
            if (t && *t != "array") {
                _errors.push_back("Array keywords combined with \"type\": \"" + *t + "\" are unsupported: " + schema.dump());
            }
        }
        if (schema.contains("pattern")) {
            const auto t = scalar_type();
            if (t && *t != "string") {
                _errors.push_back("\"pattern\" combined with \"type\": \"" + *t + "\" is unsupported: " + schema.dump());
            }
        }
    }

    // Over-approximated set of JSON types a schema can accept ("integer", "string", ...);
    // nullopt when it cannot be proven. "number" expands to {"number", "integer"} so that
    // integer vs number intersections are detected. Ordered conventions: types are kept in a
    // std::set so diagnostics are deterministic.
    std::optional<std::set<std::string>> _possible_types(const json & schema,
                                                         std::unordered_set<const json *> & active) const {
        struct CycleGuard {
            std::unordered_set<const json *> & set;
            const json * ptr;
            CycleGuard(decltype(set) s, const json * p) : set(s), ptr(p) { set.insert(p); }
            ~CycleGuard() { set.erase(ptr); }
        } guard{active, &schema};

        if (!schema.is_object() || schema.empty()) {
            return std::nullopt;
        }
        if (schema.contains("$ref")) {
            const std::string ref = schema["$ref"].get<std::string>();
            if (_refs_being_resolved.count(ref)) {
                return std::nullopt;
            }
            const auto it = _refs.find(ref);
            if (it == _refs.end()) {
                return std::nullopt;
            }
            return _possible_types(it->second, active);
        }
        if (schema.contains("const")) {
            return std::set<std::string>{_json_value_type(schema["const"])};
        }
        if (schema.contains("enum")) {
            if (!schema["enum"].is_array()) return std::nullopt;
            std::set<std::string> types;
            for (const auto & v : schema["enum"]) {
                types.insert(_json_value_type(v));
            }
            return types;
        }
        if (schema.contains("allOf")) {
            std::optional<std::set<std::string>> result;
            for (const auto & comp : schema["allOf"]) {
                auto t = _possible_types(comp, active);
                if (!t) return std::nullopt;
                if (!result) {
                    result = *t;
                } else {
                    std::set<std::string> inter;
                    std::set_intersection(result->begin(), result->end(), t->begin(), t->end(),
                                          std::inserter(inter, inter.end()));
                    result = inter;
                }
            }
            return result;
        }

        std::set<std::string> types;
        bool known = false;
        auto add_scalar = [&](const std::string & s) {
            types.insert(s);
            if (s == "number") types.insert("integer");
        };
        if (schema.contains("oneOf") || schema.contains("anyOf")) {
            const json & alts = schema.contains("oneOf") ? schema.at("oneOf") : schema.at("anyOf");
            if (!alts.is_array()) return std::nullopt;
            for (const auto & alt : alts) {
                auto t = _possible_types(alt, active);
                if (!t) return std::nullopt;
                types.insert(t->begin(), t->end());
                known = true;
            }
        }
        if (schema.contains("type")) {
            const json & t = schema["type"];
            if (t.is_string()) {
                add_scalar(t.get<std::string>());
                known = true;
            } else if (t.is_array()) {
                for (const auto & m : t) {
                    if (!m.is_string()) return std::nullopt;
                    add_scalar(m.get<std::string>());
                }
                known = true;
            } else {
                return std::nullopt;
            }
        }
        if (schema.contains("properties") || schema.contains("additionalProperties") || schema.contains("required")) {
            types.insert("object");
            known = true;
        }
        if (schema.contains("items") || schema.contains("prefixItems")) {
            types.insert("array");
            known = true;
        }
        if (schema.contains("pattern") || schema.contains("minLength") || schema.contains("maxLength")
            || schema.contains("format")) {
            types.insert("string");
            known = true;
        }
        if (schema.contains("minimum") || schema.contains("maximum")
            || schema.contains("exclusiveMinimum") || schema.contains("exclusiveMaximum")) {
            types.insert("integer");
            known = true;
        }
        if (!known) {
            return std::nullopt;
        }
        return types;
    }

    // Exact literal value set of a schema (const/enum, $ref-resolved), serialized with the
    // deterministic nlohmann::json dump (std::map key order); nullopt when unknown.
    std::optional<std::set<std::string>> _constant_values(const json & schema,
                                                          std::unordered_set<const json *> & active) const {
        struct CycleGuard {
            std::unordered_set<const json *> & set;
            const json * ptr;
            CycleGuard(decltype(set) s, const json * p) : set(s), ptr(p) { set.insert(p); }
            ~CycleGuard() { set.erase(ptr); }
        };

        if (!schema.is_object()) {
            return std::nullopt;
        }
        CycleGuard guard{active, &schema};
        if (schema.contains("$ref")) {
            const std::string ref = schema["$ref"].get<std::string>();
            if (_refs_being_resolved.count(ref)) {
                return std::nullopt;
            }
            const auto it = _refs.find(ref);
            if (it == _refs.end()) {
                return std::nullopt;
            }
            return _constant_values(it->second, active);
        }
        if (schema.contains("const")) {
            return std::set<std::string>{schema["const"].dump()};
        }
        if (schema.contains("enum") && schema["enum"].is_array() && !schema["enum"].empty()) {
            std::set<std::string> values;
            for (const auto & v : schema["enum"]) {
                values.insert(v.dump());
            }
            return values;
        }
        return std::nullopt;
    }

    // True when no value can satisfy both schemas. Provable either by disjoint static type
    // sets or by disjoint exact literal sets (const/enum).
    bool _schemas_disjoint(const json & a, const json & b,
                           std::unordered_set<const json *> & active) const {
        auto ta = _possible_types(a, active);
        auto tb = _possible_types(b, active);
        if (ta && tb) {
            std::set<std::string> inter;
            std::set_intersection(ta->begin(), ta->end(), tb->begin(), tb->end(),
                                  std::inserter(inter, inter.end()));
            if (inter.empty()) {
                return true;
            }
        }
        auto ca = _constant_values(a, active);
        auto cb = _constant_values(b, active);
        if (ca && cb) {
            for (const auto & v : *ca) {
                if (cb->count(v)) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    std::string visit(const json & schema, const std::string & name) {
        json schema_type = schema.contains("type") ? schema["type"] : json();
        std::string schema_format = schema.contains("format") && schema["format"].is_string()
            ? schema["format"].get<std::string>() : "";
        std::string rule_name = is_reserved_name(name) ? name + "-" : name.empty() ? "root" : name;

        if (schema.contains("$ref")) {
            return _add_rule(rule_name, _resolve_ref(schema["$ref"]));
        }
        if (schema.is_object()) {
            _check_supported_assertions(schema, schema_type);
        }
        if (schema.contains("oneOf") || schema.contains("anyOf")) {
            const bool is_one_of = schema.contains("oneOf");
            const json & alts = is_one_of ? schema.at("oneOf") : schema.at("anyOf");
            if (!alts.is_array() || alts.empty()) {
                _errors.push_back(std::string(is_one_of ? "\"oneOf\"" : "\"anyOf\"")
                    + " must be a non-empty array: " + schema.dump());
                return "";
            }
            std::vector<json> alt_schemas;
            for (const auto & alt : alts) {
                alt_schemas.push_back(alt);
            }
            if (is_one_of) {
                // oneOf requires exactly-one semantics. A union is only faithful when the
                // alternatives are provably disjoint; overlapping alternatives must fail
                // instead of silently broadening to "at least one".
                std::unordered_set<const json *> active;
                for (size_t i = 0; i < alt_schemas.size(); i++) {
                    for (size_t j = i + 1; j < alt_schemas.size(); j++) {
                        if (!_schemas_disjoint(alt_schemas[i], alt_schemas[j], active)) {
                            _errors.push_back("\"oneOf\" alternatives " + std::to_string(i) + " and "
                                + std::to_string(j) + " may both accept the same values; exact oneOf "
                                "(exactly-one) semantics are unsupported, so overlapping alternatives "
                                "are rejected. Use anyOf for a plain union or make the alternatives "
                                "disjoint: " + alt_schemas[i].dump() + " vs " + alt_schemas[j].dump());
                        }
                    }
                }
                if (!_errors.empty()) {
                    return "";
                }
            }
            return _add_rule(rule_name, _generate_union_rule(name, alt_schemas));
        }

        if (schema_type.is_array()) {
            std::vector<json> schema_types;
            for (const auto & t : schema_type) {
                json schema_copy(schema);
                schema_copy["type"] = t;
                schema_types.push_back(schema_copy);
            }
            return _add_rule(rule_name, _generate_union_rule(name, schema_types));
        }
        if (schema.contains("const")) {
            return _add_rule(rule_name, _generate_constant_rule(schema["const"]));
        }
        if (schema.contains("enum")) {
            std::vector<std::string> enum_values;
            for (const auto & v : schema["enum"]) {
                enum_values.push_back(_generate_constant_rule(v));
            }
            return _add_rule(rule_name, "(" + string_join(enum_values, " | ") + ")");
        }
        if ((schema_type.is_null() || schema_type == "object")
                && (schema.contains("properties") || schema.contains("required")
                    || (schema.contains("additionalProperties") && schema["additionalProperties"] != true))) {
            std::unordered_set<std::string> required;
            if (schema.contains("required") && schema["required"].is_array()) {
                for (const auto & item : schema["required"]) {
                    if (item.is_string()) {
                        required.insert(item.get<std::string>());
                    }
                }
            }
            std::vector<std::pair<std::string, json>> properties;
            if (schema.contains("properties")) {
                for (const auto & prop : schema["properties"].items()) {
                    properties.emplace_back(prop.key(), prop.value());
                }
            }
            return _add_rule(rule_name,
                _build_object_rule(
                    properties, required, name,
                    schema.contains("additionalProperties") ? schema["additionalProperties"] : json()));
        }
        if ((schema_type.is_null() || schema_type == "object" || schema_type == "string") && schema.contains("allOf")) {
            std::unordered_set<std::string> required;
            std::vector<std::pair<std::string, json>> properties;
            std::map<std::string, size_t> enum_values;
            const std::string& hybrid_name = name;
            std::function<void(const json &, bool)> add_component = [&](const json & comp_schema, bool is_required) {
                if (comp_schema.contains("$ref")) {
                    const std::string ref = comp_schema["$ref"].get<std::string>();
                    if (_refs_being_resolved.count(ref)) {
                        _errors.push_back("Cycle in allOf $ref: " + ref);
                        return;
                    }
                    const auto it = _refs.find(ref);
                    if (it == _refs.end()) {
                        _errors.push_back("Unresolved $ref in allOf component: " + ref);
                        return;
                    }
                    _refs_being_resolved.insert(ref);
                    add_component(it->second, is_required);
                    _refs_being_resolved.erase(ref);
                    return;
                }
                // Components are either object-shaped, exact literal constraints, or unions of
                // those; anything else would previously be dropped here, silently weakening the
                // intersection the schema asserts.
                for (const auto & kv : comp_schema.items()) {
                    static const std::unordered_set<std::string> ALLOWED_COMPONENT_KEYS = {
                        "$ref", "properties", "required", "enum", "anyOf", "type", "title", "description",
                    };
                    if (!ALLOWED_COMPONENT_KEYS.count(kv.key())
                        || (kv.key() == "type" && kv.value() != "object")) {
                        // Only the object "type" is mergeable here; any other type marker would
                        // have been silently dropped, weakening the asserted intersection.
                        _errors.push_back("Unsupported key \"" + kv.key() + "\" in allOf component; "
                            "dropping it would weaken the constraint: " + comp_schema.dump());
                        return;
                    }
                }
                if (comp_schema.contains("properties")) {
                    std::unordered_set<std::string> comp_required;
                    if (comp_schema.contains("required") && comp_schema["required"].is_array()) {
                        for (const auto & item : comp_schema["required"]) {
                            if (item.is_string()) {
                                comp_required.insert(item.get<std::string>());
                            }
                        }
                    }
                    for (const auto & prop : comp_schema["properties"].items()) {
                        properties.emplace_back(prop.key(), prop.value());
                        if (is_required || comp_required.count(prop.key())) {
                            required.insert(prop.key());
                        }
                    }
                    for (const auto & r : comp_required) {
                        if (!comp_schema["properties"].contains(r)) {
                            _errors.push_back("allOf component requires property \"" + r
                                + "\" without declaring it in its \"properties\": " + comp_schema.dump());
                        }
                    }
                } else if (comp_schema.contains("required") && !comp_schema["required"].empty()) {
                    _errors.push_back("allOf component has \"required\" without \"properties\": "
                        + comp_schema.dump());
                } else if (comp_schema.contains("enum")) {
                    for (const auto & v : comp_schema["enum"]) {
                        const auto rule = _generate_constant_rule(v);
                        if (enum_values.find(rule) == enum_values.end()) {
                            enum_values[rule] = 0;
                        }
                        enum_values[rule] += 1;
                    }
                }
            };
            // The parent's own properties/required participate in the intersection; previous
            // versions silently dropped them when allOf was present.
            json parent_component = json::object();
            if (schema.contains("properties")) {
                parent_component["properties"] = schema["properties"];
            }
            if (schema.contains("required")) {
                parent_component["required"] = schema["required"];
            }
            if (!parent_component.empty()) {
                add_component(parent_component, false);
            }
            for (const auto & t : schema["allOf"]) {
                if (t.contains("anyOf")) {
                    for (const auto & tt : t["anyOf"]) {
                        add_component(tt, false);
                    }
                } else {
                    add_component(t, true);
                }
            }
            if (!enum_values.empty()) {
                std::vector<std::string> enum_intersection;
                for (const auto & p : enum_values) {
                    if (p.second == schema["allOf"].size()) {
                        enum_intersection.push_back(p.first);
                    }
                }
                if (!enum_intersection.empty()) {
                    return _add_rule(rule_name, "(" + string_join(enum_intersection, " | ") + ")");
                }
            }
            return _add_rule(rule_name, _build_object_rule(properties, required, hybrid_name, json()));
        }
        if ((schema_type.is_null() || schema_type == "array") && (schema.contains("items") || schema.contains("prefixItems"))) {
            json items = schema.contains("items") ? schema["items"] : schema["prefixItems"];
            if (items.is_array()) {
                std::string rule = "\"[\" space ";
                for (size_t i = 0; i < items.size(); i++) {
                    if (i > 0) {
                        rule += " \",\" space ";
                    }
                    rule += visit(items[i], name + (name.empty() ? "" : "-") + "tuple-" + std::to_string(i));
                }
                rule += " space \"]\"";
                return _add_rule(rule_name, rule);
            }
            std::string item_rule_name = visit(items, name + (name.empty() ? "" : "-") + "item");
            int min_items = schema.contains("minItems") ? schema["minItems"].get<int>() : 0;
            json max_items_json = schema.contains("maxItems") ? schema["maxItems"] : json();
            int max_items = max_items_json.is_number_integer() ? max_items_json.get<int>() : std::numeric_limits<int>::max();

            return _add_rule(rule_name, "\"[\" space " + build_repetition(item_rule_name, min_items, max_items, "\",\" space") + " space \"]\"");
        }
        if (schema_type == "array" && (schema.contains("minItems") || schema.contains("maxItems"))) {
            // Length-bounded array without an item constraint: the array shape and its length
            // are enforced, element values stay unconstrained.
            std::string item_rule = _add_primitive("value", PRIMITIVE_RULES.at("value"));
            int min_items = schema.contains("minItems") ? schema["minItems"].get<int>() : 0;
            json max_items_json = schema.contains("maxItems") ? schema["maxItems"] : json();
            int max_items = max_items_json.is_number_integer() ? max_items_json.get<int>() : std::numeric_limits<int>::max();
            return _add_rule(rule_name, "\"[\" space " + build_repetition(item_rule, min_items, max_items, "\",\" space") + " space \"]\"");
        }
        if ((schema_type.is_null() || schema_type == "string") && schema.contains("pattern")) {
            return _visit_pattern(schema["pattern"], rule_name);
        }
        if ((schema_type.is_null() || schema_type == "string") && std::regex_match(schema_format, std::regex("^uuid[1-5]?$"))) {
            return _add_primitive(rule_name == "root" ? "root" : schema_format, PRIMITIVE_RULES.at("uuid"));
        }
        if ((schema_type.is_null() || schema_type == "string") && STRING_FORMAT_RULES.find(schema_format + "-string") != STRING_FORMAT_RULES.end()) {
            auto prim_name = schema_format + "-string";
            return _add_rule(rule_name, _add_primitive(prim_name, STRING_FORMAT_RULES.at(prim_name)));
        }
        if (schema_type == "string" && (schema.contains("minLength") || schema.contains("maxLength"))) {
            std::string char_rule = _add_primitive("char", PRIMITIVE_RULES.at("char"));
            int min_len = schema.contains("minLength") ? schema["minLength"].get<int>() : 0;
            int max_len = schema.contains("maxLength") ? schema["maxLength"].get<int>() : std::numeric_limits<int>::max();
            return _add_rule(rule_name, "\"\\\"\" " + build_repetition(char_rule, min_len, max_len) + " \"\\\"\"");
        }
        if (schema_type == "integer" && (schema.contains("minimum") || schema.contains("exclusiveMinimum") || schema.contains("maximum") || schema.contains("exclusiveMaximum"))) {
            int64_t min_value = std::numeric_limits<int64_t>::min();
            int64_t max_value = std::numeric_limits<int64_t>::max();
            if (schema.contains("minimum")) {
                min_value = schema["minimum"].get<int64_t>();
            } else if (schema.contains("exclusiveMinimum")) {
                min_value = schema["exclusiveMinimum"].get<int64_t>() + 1;
            }
            if (schema.contains("maximum")) {
                max_value = schema["maximum"].get<int64_t>();
            } else if (schema.contains("exclusiveMaximum")) {
                max_value = schema["exclusiveMaximum"].get<int64_t>() - 1;
            }
            std::stringstream out;
            out << "(";
            build_min_max_int(min_value, max_value, out);
            out << ")";
            return _add_rule(rule_name, out.str());
        }
        if (schema.empty() || schema_type == "object") {
            // Note: an empty schema is deliberately compiled to the conservative object
            // primitive (never the unconstrained value rule) so that an underspecified root
            // cannot open unconstrained generation.
            return _add_rule(rule_name, _add_primitive("object", PRIMITIVE_RULES.at("object")));
        }
        if (schema_type.is_null() && schema.is_object()) {
            // No type constraint and no recognized structural keywords (e.g. {"description": "..."}).
            // Per JSON Schema semantics this is equivalent to {} and accepts any value.
            return _add_rule(rule_name, _add_primitive("value", PRIMITIVE_RULES.at("value")));
        }
        if (!schema_type.is_string() || PRIMITIVE_RULES.find(schema_type.get<std::string>()) == PRIMITIVE_RULES.end()) {
            _errors.push_back("Unrecognized schema: " + schema.dump());
            return "";
        }
        return _add_primitive(rule_name == "root" ? "root" : schema_type.get<std::string>(), PRIMITIVE_RULES.at(schema_type.get<std::string>()));
    }
    void check_errors() {
        if (!_errors.empty()) {
            throw std::invalid_argument("JSON schema conversion failed:\n" + string_join(_errors, "\n"));
        }
    }

    [[nodiscard]] const std::vector<std::string> & warnings() const { return _warnings; }

    std::string format_grammar() {
        std::stringstream ss;
        for (const auto & kv : _rules) {
            ss << kv.first << " ::= " << kv.second << '\n';
        }
        return ss.str();
    }
};


json_schema_grammar json_schema_to_grammar(const nlohmann::json & schema, const std::string & root_name) {
    common_schema_converter converter([](const std::string &) { return json(); }, false);
    auto copy = schema;
    converter.resolve_refs(copy, "");
    // The root name namespaces every schema-derived rule (root_name, root_name-<prop>, ...),
    // so several converted schemas can be merged into one grammar text as long as each uses a
    // distinct root name. Shared primitive rules (space, char, string, ...) keep their fixed
    // bodies across all conversions.
    converter.visit(copy, root_name);
    converter.check_errors();
    return {converter.format_grammar(), converter.warnings()};
}

