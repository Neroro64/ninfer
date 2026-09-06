#pragma once

#include "targets/qwen3_6/impl/frontend/tool_call_parser.h"

#include <string>
#include <string_view>

namespace ninfer::targets::qwen3_6 {
namespace frontend_internal {

// Strict tool enforcement is compiled against the Qwen XML markup the parser actually consumes,
// not against JSON bytes:
//   - every declared tool contributes one "<tool_call>" alternative with a fixed name literal,
//   - strict tools emit their parameters in converter (lexicographic) property order, required
//     ones mandatorily and optional ones optionally, so required and membership are exact,
//   - strict parameter values are JSON values (strings quoted) compiled through the schema
//     converter; the strict-only parser normalization decodes them back,
//   - non-strict tools keep their free-form surface (any parameter names, raw text values),
//   - free text is legal before the first tool call; between and after calls only format
//     whitespace is legal because the region parser restores anything else verbatim,
//   - an optional explicit output constraint contributes the content branch (alternative, not
//     intersection) and may combine with the tool-call branches.
// The composed document starts with "root ::= ...". Rule names with the "n6-" prefix are
// reserved by the composer.
[[nodiscard]] std::string compose_output_grammar(const ToolArgumentTypeContracts* contract,
                                                 std::string_view constraint_rules);

// Converts a JSON Schema document into GBNF rule text through the vendored converter. The start
// rule is named "n6-content" and derived rules share that namespace. Throws std::invalid_argument
// naming the keyword when the schema uses a construct the converter cannot express.
[[nodiscard]] std::string convert_output_constraint_schema(std::string_view schema_json);

// Rejects a strict tool definition whose schema cannot be expressed by the Qwen markup surface.
// The top level must be a plain object schema; parameter value schemas may use the full
// converter subset. Throws std::invalid_argument naming the offending construct.
void validate_strict_definition(std::string_view tool_definition_json);

// Contract-driven form for a tool already compiled into a prepared prompt (name and stored
// parameters JSON).
void validate_strict_contract_tool(const ToolArgumentTypeContracts::Tool& tool);

} // namespace frontend_internal

// Public surface: rejects a strict tool definition ({"type":"function","function":{...,
// "strict":true}}) whose schema cannot be enforced through the Qwen tool markup. Throws
// std::invalid_argument naming the offending construct.
void validate_strict_tool_json(std::string_view tool_definition_json);

} // namespace ninfer::targets::qwen3_6
