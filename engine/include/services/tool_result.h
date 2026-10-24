#pragma once

#include <nlohmann/json.hpp>

namespace fastllm {

// Binary content is kept out of JSON text and text-token accounting.
struct ToolResult {
    nlohmann::json data;
    nlohmann::json attachments = nlohmann::json::array();
};

} // namespace fastllm
