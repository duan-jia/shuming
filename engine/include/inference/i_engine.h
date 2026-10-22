#pragma once

#include <string>
#include <chrono>
#include <functional>
#include <vector>
#include <nlohmann/json.hpp>

namespace fastllm {

struct ChatMessage {
    std::string role;
    std::string content;
    std::string reasoning;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

struct InferParams {
    int n_ctx = 2048;
    int n_batch = 512;
    int n_threads = 4;
    int n_gpu_layers = -1;
    int max_tokens = 512; // Remote requests omit the output limit when zero.
    int context_output_reserve = 0;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float repeat_penalty = 1.1f;
    int repeat_last_n = 64;
    std::string system_prompt;
    std::string session_id;
    std::vector<ChatMessage> history;
    nlohmann::json tools = nlohmann::json::array();
    nlohmann::json tool_messages = nlohmann::json::array();
    std::function<bool()> should_stop;
    int timeout_seconds = 300;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
};

struct InferResult {
    std::string text;
    std::string reasoning;
    std::string error;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason;
    std::string stop_reason;
    bool usage_estimated = false;
};

class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;

    virtual bool is_loaded() const = 0;
    virtual std::string model_name() const = 0;

    virtual void stream_complete(
        const std::string& prompt,
        const InferParams& params,
        std::function<void(const std::string&)> on_token,
        std::function<void(const std::string&)> on_reasoning = nullptr,
        std::function<void(const InferResult&)> on_done = nullptr) = 0;

    virtual bool supports_tools() const { return false; }
    virtual int estimate_input_tokens(const std::string& prompt, const InferParams& params) {
        int tokens = count_tokens(prompt) + count_tokens(params.system_prompt) + 8;
        for (const auto& message : params.history) tokens += count_tokens(message.content) + 4;
        return tokens;
    }

    virtual int count_tokens(const std::string& text) {
        return static_cast<int>(text.size()) / 4;
    }
};

}
