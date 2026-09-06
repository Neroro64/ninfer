#include "targets/qwen3_6/impl/frontend/strict_tool_grammar.h"

#include <ninfer/ops/grammar.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ninfer::targets::qwen3_6::frontend_internal {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kToolOpen  = "<tool_call>";
constexpr std::string_view kParamOpen = "<parameter=";
constexpr std::string_view kParamClose = "</parameter>";

// Composed rule names stay inside the GBNF identifier alphabet [a-zA-Z0-9-].
constexpr std::string_view kWsRule           = "n6-ws";
constexpr std::string_view kFreeTextRule     = "n6-ftrun";
constexpr std::string_view kFreeCharRule     = "n6-ftchar";
constexpr std::string_view kTailRule         = "n6-tail";
constexpr std::string_view kCallRule         = "n6-call";
constexpr std::string_view kContentRule      = "n6-content";
constexpr std::string_view kFreeParamRule    = "n6-pf";
constexpr std::string_view kFreeNameRule     = "n6-pn";
constexpr std::string_view kFreeNameCharRule = "n6-pnc";
constexpr std::string_view kFreeValueRule    = "n6-rv";
constexpr std::string_view kFreeValueCharRule = "n6-rvc";

constexpr std::string_view kReservedRulePrefix = "n6-";

bool gbnf_name_byte(char byte) {
    const auto upper = static_cast<char>(byte & ~0x20U);
    return (upper >= 'A' && upper <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-';
}

bool reserved_rule_name(std::string_view name) {
    return name.size() > kReservedRulePrefix.size() &&
           name.starts_with(kReservedRulePrefix) &&
           std::all_of(name.begin(), name.end(), gbnf_name_byte);
}

std::string quote_literal(std::string_view bytes) {
    std::string literal;
    literal.reserve(bytes.size() + 2);
    literal.push_back('"');
    for (const char byte : bytes) {
        if (byte == '"' || byte == '\\') { literal.push_back('\\'); }
        literal.push_back(byte);
    }
    literal.push_back('"');
    return literal;
}

std::string complement_class(char excluded) {
    std::string body = "[^";
    if (excluded == ']' || excluded == '\\' || excluded == '^') { body.push_back('\\'); }
    body.push_back(excluded);
    body.push_back(']');
    return body;
}

// Alternating sequences that consume any text except a literal `marker`: the k-th alternative
// matches the first k marker characters and then one character that breaks the marker. The
// engine matches codepoints, so multi-byte text passes through the complement classes.
std::string marker_exclusion_body(std::string_view marker) {
    std::string consumed;
    std::string body;
    for (std::size_t index = 0; index < marker.size(); ++index) {
        if (index > 0) {
            body += " | ";
            body += quote_literal(consumed);
            body.push_back(' ');
        }
        body += complement_class(marker[index]);
        consumed.push_back(marker[index]);
    }
    return body;
}
bool references_start_rule(std::string_view body, std::string_view name) {
    std::size_t begin = 0;
    while ((begin = body.find(name, begin)) != std::string_view::npos) {
        const bool left_loose  = begin == 0 || !gbnf_name_byte(body[begin - 1]);
        const std::size_t after = begin + name.size();
        const bool right_loose = after == body.size() || !gbnf_name_byte(body[after]);
        if (left_loose && right_loose) { return true; }
        ++begin;
    }
    return false;

}

class RuleSet {
public:
    void define(std::string_view name, std::string body) {
        for (const auto& [existing, existing_body] : rules_) {
            if (existing == name) {
                if (existing_body != body) {
                    throw std::logic_error("composed grammar rule '" + std::string(name) +
                                           "' conflicts with an earlier definition");
                }
                return;
            }
        }
        rules_.emplace_back(std::string(name), std::move(body));
    }

    // Merges a converted document of "name ::= body" lines. When a rule named "root" exists and
    // differs from start_rule it is renamed, which admits plain GBNF constraint documents. The
    // start rule must exist afterwards.
    void merge_document(std::string_view converted, std::string_view start_rule) {
        bool saw_start     = false;
        bool renamed_root  = false;
        std::size_t begin  = 0;
        while (begin < converted.size()) {
            std::size_t end = converted.find('\n', begin);
            if (end == std::string_view::npos) { end = converted.size(); }
            const std::string_view line = converted.substr(begin, end - begin);
            begin = end + 1;
            if (line.empty()) { continue; }
            const std::size_t separator = line.find(" ::= ");
            if (separator == std::string_view::npos || separator == 0) {
                throw std::invalid_argument(
                    "output grammar conversion produced an unparsable rule line");
            }
            std::string name(line.substr(0, separator));
            std::string body(line.substr(separator + 5));
            if (name == start_rule) {
                saw_start = true;
            } else if (name == "root") {
                if (renamed_root) {
                    throw std::invalid_argument(
                        "output grammar constraint defines more than one root rule");
                }
                if (references_start_rule(body, "root")) {
                    throw std::invalid_argument(
                        "output grammar constraint helper rules must not reference the root "
                        "rule");
                }
                name    = std::string(start_rule);
                renamed_root = true;
                saw_start    = true;
            }
            try {
                define(name, std::move(body));
            } catch (const std::logic_error&) {
                throw std::invalid_argument("output grammar constraint redefines the reserved "
                                            "rule '" +
                                            name + "'");
            }
        }
        if (!saw_start) {
            throw std::invalid_argument("output grammar constraint must define the '" +
                                        std::string(start_rule) + "' start rule");
        }
    }

    [[nodiscard]] std::string render(std::string_view root_body) const {
        std::string document;
        document += "root ::= ";
        document += root_body;
        document.push_back('\n');
        for (const auto& [name, body] : rules_) {
            document += name;
            document += " ::= ";
            document += body;
            document.push_back('\n');
        }
        return document;
    }

private:
    std::vector<std::pair<std::string, std::string>> rules_;
};

void validate_strict_tool_name(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("strict tool function requires a non-empty name");
    }
    if (name.size() > 128) {
        throw std::invalid_argument("strict tool name exceeds 128 bytes");
    }
    const bool well_formed =
        std::all_of(name.begin(), name.end(), [](char byte) {
            const auto upper = static_cast<char>(byte & ~0x20U);
            return (upper >= 'A' && upper <= 'Z') || (byte >= '0' && byte <= '9') ||
                   byte == '_' || byte == '-';
        });
    if (!well_formed) {
        throw std::invalid_argument("strict tool name must contain only [A-Za-z0-9_-]");
    }
}

void validate_strict_parameter_name(const std::string& tool_name, const std::string& name) {
    if (name.empty() || name.size() > 128) {
        throw std::invalid_argument("strict tool '" + tool_name +
                                    "' parameter names must contain 1 to 128 bytes");
    }
    const bool markup_safe = std::all_of(name.begin(), name.end(), [](char byte) {
        return byte != '<' && byte != '>' && byte != '"' &&
               byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n';
    });
    if (!markup_safe) {
        throw std::invalid_argument("strict tool '" + tool_name +
                                    "' parameter names must not contain markup or whitespace "
                                    "bytes");
    }
}

void validate_strict_parameters(const std::string& tool_name, const Json& schema) {
    for (const auto& [keyword, value] : schema.items()) {
        if (keyword == "type" || keyword == "properties" || keyword == "required" ||
            keyword == "additionalProperties") {
            continue;
        }
        throw std::invalid_argument("strict tool '" + tool_name +
                                    "' parameters cannot enforce the keyword '" + keyword +
                                    "' through the Qwen tool markup");
    }
    if (schema.contains("type") &&
        (!schema.at("type").is_string() || schema.at("type") != "object")) {
        throw std::invalid_argument("strict tool '" + tool_name +
                                    "' parameters must be an object schema");
    }
    if (schema.contains("additionalProperties") && schema.at("additionalProperties") != false) {
        throw std::invalid_argument("strict tool '" + tool_name +
                                    "' parameters must set additionalProperties to false");
    }
    if (schema.contains("properties") && !schema.at("properties").is_object()) {
        throw std::invalid_argument("strict tool '" + tool_name +
                                    "' parameters properties must be an object");
    }
    const Json empty_properties = Json::object();
    const Json& properties =
        schema.contains("properties") ? schema.at("properties") : empty_properties;
    for (const auto& [parameter_name, property] : properties.items()) {
        validate_strict_parameter_name(tool_name, parameter_name);
        if (!property.is_object()) {
            throw std::invalid_argument("strict tool '" + tool_name + "' parameter '" +
                                        parameter_name + "' must carry an object schema");
        }
    }
    if (schema.contains("required")) {
        const Json& required = schema.at("required");
        if (!required.is_array()) {
            throw std::invalid_argument("strict tool '" + tool_name +
                                        "' parameters required must be an array");
        }
        for (const Json& entry : required) {
            if (!entry.is_string()) {
                throw std::invalid_argument("strict tool '" + tool_name +
                                            "' parameters required must contain names");
            }
            const std::string& name = entry.get_ref<const std::string&>();
            if (!properties.contains(name)) {
                throw std::invalid_argument("strict tool '" + tool_name +
                                            "' requires the undeclared parameter '" + name +
                                            "'");
            }
        }
    }
}

Json parse_parameters(const ToolCallOutputContract::Tool& tool) {
    const Json schema = Json::parse(tool.parameters_json, nullptr, false);
    if (schema.is_discarded() || !schema.is_object()) {
        throw std::invalid_argument("strict tool '" + tool.name +
                                    "' has an unparsable parameters schema");
    }
    return schema;
}
// The converter omits the named start rule when a schema maps directly onto a canonical
// primitive rule; re-converting with the default start name restores an alias line, which the
// merge renames.
std::string convert_named_rules(std::string_view schema_json, std::string_view root_name) {
    std::string converted = ops::Grammar::schema_to_gbnf(schema_json, root_name);
    if (converted.find(std::string(root_name) + " ::= ") != std::string::npos) {
        return converted;
    }
    return ops::Grammar::schema_to_gbnf(schema_json);
}

// Defines the parameter-slot rules of one strict tool and returns the sequence body that emits
// them (empty string when the tool declares no parameters).
std::string strict_parameter_slots(RuleSet& rules, const ToolCallOutputContract::Tool& tool,
                                   std::size_t tool_index) {
    const Json schema      = parse_parameters(tool);
    const Json empty_properties = Json::object();
    const Json& properties =
        schema.contains("properties") ? schema.at("properties") : empty_properties;
    std::vector<std::string> required;
    if (schema.contains("required")) {
        required.reserve(schema.at("required").size());
        for (const Json& entry : schema.at("required")) {
            required.push_back(entry.get<std::string>());
        }
    }
    // nlohmann object iteration is key-sorted, so the markup order is a deterministic function
    // of the schema; any order yields the same parsed arguments object.
    std::string slots;
    std::size_t parameter_index = 0;
    for (const auto& [parameter_name, property] : properties.items()) {
        const std::string index = std::to_string(tool_index) + "-" + std::to_string(parameter_index);
        const std::string slot  = "n6-p-" + index;
        const std::string value = "n6-v-" + index;
        rules.merge_document(convert_named_rules(property.dump(), value), value);
        rules.define(slot, quote_literal(std::string(kParamOpen) + parameter_name + ">") +
                               " " + std::string(kWsRule) + " " + value + " " +
                               std::string(kWsRule) + " " + quote_literal(kParamClose));
        slots += slot;
        const bool mandatory =
            std::find(required.begin(), required.end(), parameter_name) != required.end();
        slots.push_back(mandatory ? ' ' : '?');
        slots += kWsRule;
        slots.push_back(' ');
        ++parameter_index;
    }
    while (!slots.empty() && slots.back() == ' ') { slots.pop_back(); }
    return slots;
}


std::string strict_call_body(RuleSet& rules, const ToolCallOutputContract::Tool& tool,
                             std::size_t tool_index) {
    const std::string slots = strict_parameter_slots(rules, tool, tool_index);
    std::string body = quote_literal(kToolOpen) + " " + std::string(kWsRule) + " " +
                       quote_literal("<function=" + tool.name + ">") + " " +
                       std::string(kWsRule) + " ";
    body += slots.empty() ? std::string(kWsRule) : slots;
    body += " ";
    body += quote_literal("</function>");
    body += " ";
    body += kWsRule;
    body += " ";
    body += quote_literal("</tool_call>");
    return body;
}

std::string nonstrict_call_body(const ToolCallOutputContract::Tool& tool) {
    std::string body = quote_literal(kToolOpen) + " " + std::string(kWsRule) + " " +
                       quote_literal("<function=" + tool.name + ">") + " " +
                       std::string(kWsRule) + " (" + std::string(kFreeParamRule) + " " +
                       std::string(kWsRule) + ")* ";
    body += kWsRule;
    body += " ";
    body += quote_literal("</function>");
    body += " ";
    body += kWsRule;
    body += " ";
    body += quote_literal("</tool_call>");
    return body;
}


} // namespace

std::string compose_output_grammar(const ToolCallOutputContract* contract,
                                   std::string_view constraint_rules) {
    RuleSet rules;
    rules.define(kWsRule, "[ \t\r\n]*");

    const bool has_tools    = contract != nullptr && !contract->tools.empty();
    const bool has_content  = !constraint_rules.empty();
    if (!has_tools && !has_content) {
        throw std::invalid_argument(
            "an output constraint requires a content schema or at least one declared tool");
    }

    if (has_content) { rules.merge_document(constraint_rules, kContentRule); }

    std::string root_body;
    if (has_tools) {
        rules.define(kFreeCharRule, marker_exclusion_body(kToolOpen));
        rules.define(kFreeTextRule, std::string(kFreeCharRule) + "*");

        bool has_nonstrict = false;
        std::string alternatives;
        std::size_t index = 0;
        for (const ToolCallOutputContract::Tool& tool : contract->tools) {
            const std::string alternative = "n6-c-" + std::to_string(index);
            if (tool.strict) {
                rules.define(alternative, strict_call_body(rules, tool, index));
            } else {
                has_nonstrict = true;
                rules.define(alternative, nonstrict_call_body(tool));
            }
            if (!alternatives.empty()) { alternatives += " | "; }
            alternatives += alternative;
            ++index;
        }
        rules.define(kCallRule, alternatives);
        rules.define(kTailRule,
                     "(" + std::string(kCallRule) + " " + std::string(kWsRule) + "?)*");
        if (has_nonstrict) {
            rules.define(kFreeNameCharRule, "[^> \t\r\n]");
            rules.define(kFreeNameRule, std::string(kFreeNameCharRule) + "+");
            rules.define(kFreeValueCharRule, marker_exclusion_body(kParamClose));
            rules.define(kFreeValueRule, std::string(kFreeValueCharRule) + "*");
            rules.define(kFreeParamRule, quote_literal(kParamOpen) + " " +
                                             std::string(kFreeNameRule) + " " +
                                             quote_literal(">") + " " +
                                             std::string(kWsRule) + " " +
                                             std::string(kFreeValueRule) + " " +
                                             std::string(kWsRule) + " " +
                                             quote_literal(kParamClose));
        }

        root_body += std::string(kFreeTextRule) + "? ";
        if (has_content) { root_body += std::string(kContentRule) + "? "; }
        root_body += kTailRule;
    } else {
        // A pure content constraint starts immediately; leading whitespace tolerates the
        // canonical thinking-control suffix that trails its own close marker.
        root_body += std::string(kWsRule) + "* " + std::string(kContentRule);
    }
    return rules.render(root_body);
}

std::string convert_output_constraint_schema(std::string_view schema_json) {
    return convert_named_rules(schema_json, kContentRule);
}

void validate_strict_definition(std::string_view tool_definition_json) {
    const Json definition = Json::parse(tool_definition_json, nullptr, false);
    if (definition.is_discarded() || !definition.is_object()) {
        throw std::invalid_argument("strict tool definition must be a JSON object");
    }
    const auto function = definition.find("function");
    if (function == definition.end() || !function->is_object()) {
        throw std::invalid_argument("strict tool definition requires a function object");
    }
    const auto name = function->find("name");
    if (name == function->end() || !name->is_string()) {
        throw std::invalid_argument("strict tool function requires a string name");
    }
    const std::string& tool_name = name->get_ref<const std::string&>();
    validate_strict_tool_name(tool_name);
    const auto parameters = function->find("parameters");
    if (parameters != function->end()) {
        if (!parameters->is_object()) {
            throw std::invalid_argument("strict tool '" + tool_name +
                                        "' parameters must be an object schema");
        }
        validate_strict_parameters(tool_name, *parameters);
    }
}

void validate_strict_contract_tool(const ToolCallOutputContract::Tool& tool) {
    validate_strict_tool_name(tool.name);
    validate_strict_parameters(tool.name, parse_parameters(tool));
}

} // namespace ninfer::targets::qwen3_6::frontend_internal

namespace ninfer::targets::qwen3_6 {

void validate_strict_tool_json(std::string_view tool_definition_json) {
    frontend_internal::validate_strict_definition(tool_definition_json);
}

} // namespace ninfer::targets::qwen3_6
