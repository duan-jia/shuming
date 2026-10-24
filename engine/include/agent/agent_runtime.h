#pragma once

#include "inference/i_engine.h"
#include "services/tool_service.h"
#include "security/rate_limiter.h"

namespace fastllm {

struct AgentOptions {
    bool enabled = true;
    int max_turns = 6;
    int max_output_tokens = 0; // Zero leaves the provider output limit unspecified.
    int max_total_tokens = 0; // Zero disables the per-request cumulative token budget.
    int timeout_seconds = 90;
    int max_tool_calls_per_turn = 4;
    int max_tool_result_bytes = 24000;
};

// A per-request inference wrapper: scheduler, streaming and settlement remain
// shared with normal chat, while tool rounds use the same in-process services as MCP.
class AgentRuntime : public IInferenceEngine {
public:
    AgentRuntime(IInferenceEngine& model, ToolService& tools, RateLimiter& limiter,
                 CallerContext caller, AgentOptions options, bool allow_knowledge, bool allow_web,
                 std::function<void(const nlohmann::json&)> on_event,
                 std::function<long long()> remaining_quota);
    bool is_loaded() const override { return model_.is_loaded(); }
    std::string model_name() const override { return model_.model_name(); }
    void stream_complete(const std::string& prompt, const InferParams& params,
                         std::function<void(const std::string&)> on_token,
                         std::function<void(const std::string&)> on_reasoning = nullptr,
                         std::function<void(const InferResult&)> on_done = nullptr) override;
private:
    IInferenceEngine& model_;
    ToolService& tools_;
    RateLimiter& limiter_;
    CallerContext caller_;
    AgentOptions options_;
    bool allow_knowledge_;
    bool allow_web_;
    std::function<void(const nlohmann::json&)> on_event_;
    std::function<long long()> remaining_quota_;
};

} // namespace fastllm
