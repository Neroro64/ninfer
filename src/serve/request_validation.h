#pragma once

#include "serve/request.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ninfer::serve {

using RequestJson = nlohmann::json;

[[noreturn]] void bad_request(std::string message, std::string param = {}, std::string code = {});

std::optional<int> optional_int(const nlohmann::json& object, const char* key);
std::optional<double> optional_number(const nlohmann::json& object, const char* key);
bool optional_bool(const nlohmann::json& object, const char* key, bool fallback);

[[nodiscard]] bool valid_tool_name(std::string_view name, std::size_t maximum_length) noexcept;

// Both OpenAI surfaces share one executable constraint contract.
[[nodiscard]] std::optional<ninfer::OutputConstraint>
parse_output_format(const RequestJson& format, std::string_view param, bool responses);
void parse_constraint_extensions(const RequestJson& body, GenerationRequest& request);
void validate_strict_tool_schema(std::string_view schema, std::string_view param = "tools");

} // namespace ninfer::serve
