#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <list>
#include <unordered_map>

#include "inference/i_engine.h"

struct llama_model;
struct llama_context;
struct llama_sampler;
struct llama_vocab;
struct llama_batch;

namespace fastllm {

struct SessionState {
    std::string session_id;
    std::string system_prompt;
    size_t prefix_hash = 0;
    size_t kv_tokens = 0;
    bool kv_active = false;
    std::list<std::string>::iterator lru_iter;
};

class LlmEngine : public IInferenceEngine {
public:
    LlmEngine();
    ~LlmEngine();

    bool load_model(const std::string& model_path, const InferParams& params);
    bool is_loaded() const override { return model_ != nullptr; }
    std::string model_name() const override { return model_name_; }
    void unload_model();

    void stream_complete(const std::string& prompt, const InferParams& params,
                         std::function<void(const std::string&)> on_token,
                         std::function<void(const std::string&)> on_reasoning = nullptr,
                         std::function<void(const InferResult&)> on_done = nullptr);

    int count_tokens(const std::string& text) override;

    void activate_session(const std::string& session_id, const std::string& system_prompt);
    void deactivate_session(const std::string& session_id);
    bool has_session_cache(const std::string& session_id) const;
    size_t session_kv_tokens(const std::string& session_id) const;
    void touch_session(const std::string& session_id);

    void set_max_kv_cache_bytes(size_t bytes);
    size_t kv_cache_usage() const;
    llama_model* get_llama_model() const { return model_; }
    size_t kv_cache_max() const;

private:
    std::vector<int> tokenize(const std::string& text, bool add_bos = true);
    std::string token_to_piece(int token);
    void update_kv_cache_tracking(const std::string& session_id, size_t num_tokens);
    void evict_if_needed();

    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    InferParams default_params_;
    std::string model_name_;
    std::mutex mutex_;

    std::unordered_map<std::string, SessionState> sessions_;
    std::list<std::string> lru_order_;
    size_t total_kv_bytes_ = 0;
    size_t max_kv_bytes_ = 4ULL * 1024 * 1024 * 1024;
    std::unordered_map<size_t, int> prefix_ref_count_;
};

}
