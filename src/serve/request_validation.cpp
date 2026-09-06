#include "serve/request_validation.h"
#include "ninfer/ops/grammar.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace ninfer::serve {

[[noreturn]] void bad_request(std::string message, std::string param, std::string code) {
    ApiError error;
    error.status  = 400;
    error.type    = "invalid_request_error";
    error.message = std::move(message);
    error.param   = std::move(param);
    error.code    = std::move(code);
    throw ApiException(std::move(error));
}

std::optional<int> optional_int(const RequestJson& object, const char* key) {
    if (!object.contains(key) || object.at(key).is_null()) { return std::nullopt; }
    const RequestJson& value = object.at(key);
    if (!value.is_number_integer()) { bad_request(std::string(key) + " must be an integer", key); }
    if (value.is_number_unsigned()) {
        const std::uint64_t converted = value.get<std::uint64_t>();
        if (converted > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            bad_request(std::string(key) + " is out of range", key);
        }
        return static_cast<int>(converted);
    }
    const std::int64_t converted = value.get<std::int64_t>();
    if (converted < std::numeric_limits<int>::min() ||
        converted > std::numeric_limits<int>::max()) {
        bad_request(std::string(key) + " is out of range", key);
    }
    return static_cast<int>(converted);
}

std::optional<double> optional_number(const RequestJson& object, const char* key) {
    if (!object.contains(key) || object.at(key).is_null()) { return std::nullopt; }
    if (!object.at(key).is_number()) { bad_request(std::string(key) + " must be a number", key); }
    const double value = object.at(key).get<double>();
    if (!std::isfinite(value)) { bad_request(std::string(key) + " must be finite", key); }
    return value;
}

bool optional_bool(const RequestJson& object, const char* key, bool fallback) {
    if (!object.contains(key) || object.at(key).is_null()) { return fallback; }
    if (!object.at(key).is_boolean()) { bad_request(std::string(key) + " must be a boolean", key); }
    return object.at(key).get<bool>();
}

bool valid_tool_name(std::string_view name, std::size_t maximum_length) noexcept {
    if (name.empty() || name.size() > maximum_length) { return false; }
    for (const unsigned char character : name) {
        if (std::isalnum(character) == 0 && character != '_' && character != '-') { return false; }
    }
    return true;
}

namespace {

ninfer::OutputConstraint schema_constraint(const RequestJson& schema, std::string_view param) {
    if (!schema.is_object()) {
        bad_request("JSON Schema must be an object", std::string(param));
    }
    std::string text = schema.dump();
    try {
        (void)ops::Grammar::schema_to_gbnf(text);
    } catch (const std::exception& error) {
        bad_request(error.what(), std::string(param), "invalid_json_schema");
    }
    return {.kind = ninfer::OutputConstraint::JsonSchema, .text = std::move(text)};
}

ninfer::OutputConstraint gbnf_constraint(const RequestJson& value, std::string_view param) {
    if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
        bad_request("grammar must be a non-empty GBNF string", std::string(param));
    }
    std::string text = value.get<std::string>();
    try {
        ops::Grammar::validate_gbnf(text);
    } catch (const std::exception& error) {
        bad_request(error.what(), std::string(param), "invalid_grammar");
    }
    return {.kind = ninfer::OutputConstraint::Gbnf, .text = std::move(text)};
}

} // namespace

std::optional<ninfer::OutputConstraint>
parse_output_format(const RequestJson& format, std::string_view param, bool responses) {
    if (!format.is_object() || !format.contains("type") || !format.at("type").is_string()) {
        bad_request("output format must contain a string type", std::string(param));
    }
    const std::string type = format.at("type").get<std::string>();
    if (type == "text") { return std::nullopt; }
    if (type == "json_object") {
        return schema_constraint(RequestJson{{"type", "object"}}, param);
    }
    if (type != "json_schema") {
        bad_request("unsupported output format: " + type, std::string(param),
                    "response_format_not_supported");
    }
    const RequestJson* specification = &format;
    if (!responses) {
        if (!format.contains("json_schema") || !format.at("json_schema").is_object()) {
            bad_request("response_format.json_schema must be an object", std::string(param));
        }
        specification = &format.at("json_schema");
    }
    if (!specification->contains("name") || !specification->at("name").is_string() ||
        !valid_tool_name(specification->at("name").get_ref<const std::string&>(), 64)) {
        bad_request("JSON Schema name must match [A-Za-z0-9_-]{1,64}", std::string(param));
    }
    if (specification->contains("strict") && !specification->at("strict").is_null() &&
        !specification->at("strict").is_boolean()) {
        bad_request("JSON Schema strict must be a boolean", std::string(param));
    }
    if (specification->contains("description") &&
        !specification->at("description").is_null() &&
        !specification->at("description").is_string()) {
        bad_request("JSON Schema description must be a string", std::string(param));
    }
    if (!specification->contains("schema")) {
        bad_request("JSON Schema format requires schema", std::string(param));
    }
    return schema_constraint(specification->at("schema"), param);
}

void validate_strict_tool_schema(std::string_view schema, std::string_view param) {
    try {
        (void)ops::Grammar::schema_to_gbnf(schema);
    } catch (const std::exception& error) {
        bad_request(error.what(), std::string(param), "invalid_json_schema");
    }
}

void parse_constraint_extensions(const RequestJson& body, GenerationRequest& request) {
    auto install = [&](ninfer::OutputConstraint constraint, std::string_view param) {
        if (request.output_constraint) {
            bad_request("specify only one output constraint", std::string(param),
                        "conflicting_output_constraints");
        }
        request.output_constraint = std::move(constraint);
    };
    auto parse = [&](std::string_view kind, const RequestJson& value, std::string_view param) {
        if (kind == "json") {
            RequestJson schema = value;
            if (value.is_string()) {
                try {
                    schema = RequestJson::parse(value.get_ref<const std::string&>());
                } catch (const RequestJson::exception&) {
                    bad_request("invalid JSON Schema string", std::string(param));
                }
            }
            install(schema_constraint(schema, param), param);
        } else if (kind == "grammar") {
            // llama.cpp's empty grammar extension is neutral.
            if (value.is_string() && value.get_ref<const std::string&>().empty()) { return; }
            install(gbnf_constraint(value, param), param);
        } else if (kind == "choice") {
            if (!value.is_array() || value.empty()) {
                bad_request("choice must be a non-empty array of strings", std::string(param));
            }
            std::string grammar = "root ::= ";
            for (const auto& choice : value) {
                if (!choice.is_string()) {
                    bad_request("choice entries must be strings", std::string(param));
                }
                if (grammar != "root ::= ") { grammar += " | "; }
                grammar += choice.dump();
            }
            grammar += '\n';
            install(gbnf_constraint(grammar, param), param);
        } else {
            bad_request("regex constrained decoding is not supported", std::string(param),
                        "constrained_decoding_not_supported");
        }
    };
    for (const auto& [field, kind] :
         {std::pair{"grammar", "grammar"}, {"json_schema", "json"}, {"guided_json", "json"},
          {"guided_grammar", "grammar"}, {"guided_choice", "choice"}, {"guided_regex", "regex"}}) {
        if (body.contains(field) && !body.at(field).is_null()) {
            parse(kind, body.at(field), field);
        }
    }
    if (!body.contains("structured_outputs") || body.at("structured_outputs").is_null()) { return; }
    const auto& structured = body.at("structured_outputs");
    if (!structured.is_object()) {
        bad_request("structured_outputs must be an object", "structured_outputs");
    }
    for (const auto& [key, value] : structured.items()) {
        if (value.is_null()) { continue; }
        if (key != "json" && key != "grammar" && key != "choice" && key != "regex") {
            bad_request("unsupported structured_outputs member: " + key, "structured_outputs");
        }
        parse(key, value, "structured_outputs." + key);
    }
}

} // namespace ninfer::serve
