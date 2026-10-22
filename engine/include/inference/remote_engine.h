#pragma once

#include <string>
#include "inference/i_engine.h"

namespace fastllm {

class RemoteEngine : public IInferenceEngine {
public:
    RemoteEngine(const std::string& endpoint, const std::string& model,
                 const std::string& api_key, int context_window_tokens = 32768);
    ~RemoteEngine() override = default;

    bool is_loaded() const override { return true; }
    std::string model_name() const override { return model_; }
    bool supports_tools() const override { return true; }
    int estimate_input_tokens(const std::string& prompt, const InferParams& params) override;

    void stream_complete(const std::string& prompt, const InferParams& params,
                         std::function<void(const std::string&)> on_token,
                         std::function<void(const std::string&)> on_reasoning = nullptr,
                         std::function<void(const InferResult&)> on_done = nullptr) override;

private:
    std::string endpoint_;
    std::string model_;
    std::string api_key_;
    int context_window_tokens_;
};

}
