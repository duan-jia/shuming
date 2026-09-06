#pragma once

#include <string>
#include <functional>
#include <vector>

namespace fastllm {

struct ChatMessage {
    std::string role;
    std::string content;
};

struct InferParams {
    int n_ctx = 2048;
    int n_batch = 512;
    int n_threads = 4;
    int n_gpu_layers = -1;
    int max_tokens = 512;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float repeat_penalty = 1.1f;
    int repeat_last_n = 64;
    std::string system_prompt;
    std::string session_id;
    std::vector<ChatMessage> history;
};

struct InferResult {
    std::string text;
    std::string reasoning;
    std::string error;
    int prompt_tokens = 0;
    int completion_tokens = 0;
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

    virtual int count_tokens(const std::string& text) {
        return static_cast<int>(text.size()) / 4;
    }
};

}
