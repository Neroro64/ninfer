#pragma once

// Vendored from llama.cpp common/json-schema-to-grammar.h (MIT, see LICENSE and
// README.ninfer.md). Adaptations: llama.cpp's common_json value type is replaced by
// nlohmann::json; the common_schema_info probe and the build_grammar callback entry point are
// removed; json_schema_to_grammar reports degraded-construct warnings explicitly instead of
// printing them; NInfer strictness: constructs that would silently broaden the constraint
// (unsupported assertions, overlapping oneOf, ignored sibling keywords) fail conversion
// instead of being dropped, and the root rule name is selectable for composition.

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

struct json_schema_grammar {
    std::string gbnf;
    std::vector<std::string> warnings;
};

// Converts a JSON Schema into a GBNF grammar. Throws std::invalid_argument when the schema
// uses constructs the converter cannot express exactly; nothing is silently dropped. Remote
// $ref targets are not fetched and fail conversion.
//
// root_name: "" (default) names the start rule "root". A non-empty name namespaces every
// schema-derived rule (root_name, root_name-<property>, ...), which lets callers convert
// several schemas with distinct root names and merge the outputs into one grammar text.
// Shared primitive rules (space, char, string, object, array, value, number, integer,
// boolean, null, date/time forms) are emitted with fixed bodies on every conversion.
json_schema_grammar json_schema_to_grammar(const nlohmann::json & schema, const std::string & root_name = "");
