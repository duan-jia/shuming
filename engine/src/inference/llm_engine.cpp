#include "inference/llm_engine.h"
#include "inference/reasoning_parser.h"

#include <spdlog/spdlog.h>
#include <llama.h>
#include <algorithm>

namespace fastllm {

LlmEngine::LlmEngine() {
    static bool backend_initialized = false;
    if (!backend_initialized) {
        llama_backend_init();
        backend_initialized = true;
        spdlog::info("llama.cpp backend initialized");
    }
}

LlmEngine::~LlmEngine() {
    unload_model();
}

bool LlmEngine::load_model(const std::string& model_path, const InferParams& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto mparams = llama_model_default_params();
    mparams.n_gpu_layers = params.n_gpu_layers;

    spdlog::info("Loading model from: {}", model_path);
    model_ = llama_model_load_from_file(model_path.c_str(), mparams);
    if (!model_) {
        spdlog::error("Failed to load model: {}", model_path);
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);

    auto cparams = llama_context_default_params();
    cparams.n_ctx = params.n_ctx;
    cparams.n_batch = params.n_batch;
    cparams.n_ubatch = params.n_batch;
    cparams.n_threads = params.n_threads;
    cparams.n_threads_batch = params.n_threads;

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) {
        spdlog::error("Failed to create context");
        llama_model_free(model_);
        model_ = nullptr;
        return false;
    }

    default_params_ = params;
    model_name_ = model_path;
    spdlog::info("Model loaded: {} (ctx={}, threads={}, gpu_layers={})",
                 model_path, params.n_ctx, params.n_threads, params.n_gpu_layers);
    return true;
}

void LlmEngine::unload_model() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
    lru_order_.clear();
    prefix_ref_count_.clear();
    total_kv_bytes_ = 0;
    if (ctx_) { llama_free(ctx_); ctx_ = nullptr; }
    if (model_) { llama_model_free(model_); model_ = nullptr; }
    vocab_ = nullptr;
    model_name_.clear();
}

std::vector<int> LlmEngine::tokenize(const std::string& text, bool add_bos) {
    if (!vocab_) return {};
    int max_tokens = static_cast<int>(text.size()) + 128;
    std::vector<int> tokens(max_tokens);
    int n = llama_tokenize(vocab_, text.c_str(), static_cast<int>(text.size()),
                           reinterpret_cast<llama_token*>(tokens.data()),
                           max_tokens, add_bos, false);
    if (n < 0) {
        max_tokens = -n;
        tokens.resize(max_tokens);
        n = llama_tokenize(vocab_, text.c_str(), static_cast<int>(text.size()),
                           reinterpret_cast<llama_token*>(tokens.data()),
                           max_tokens, add_bos, false);
    }
    if (n < 0) {
        spdlog::error("tokenize failed: n={}", n);
        return {};
    }
    tokens.resize(static_cast<size_t>(n));
    return tokens;
}

std::string LlmEngine::token_to_piece(int token) {
    if (!vocab_) return "";
    char buf[256];
    int n = llama_token_to_piece(vocab_, static_cast<llama_token>(token),
                                  buf, sizeof(buf), 0, true);
    if (n < 0) return "";
    return std::string(buf, n);
}

int LlmEngine::count_tokens(const std::string& text) {
    return tokenize(text, false).size();
}

void LlmEngine::stream_complete(const std::string& prompt, const InferParams& params,
                                 std::function<void(const std::string&)> on_token,
                                 std::function<void(const std::string&)> on_reasoning,
                                 std::function<void(const InferResult&)> on_done) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!model_ || !ctx_ || !vocab_) {
        if (on_done) on_done({});
        return;
    }

    std::string full_prompt;
    if (!params.system_prompt.empty()) {
        full_prompt = params.system_prompt + "\n\n";
    }
    full_prompt += prompt;

    auto tokens = tokenize(full_prompt, true);
    if (tokens.empty()) {
        spdlog::error("Tokenization returned empty");
        if (on_done) on_done({});
        return;
    }

    int n_prompt_tokens = tokens.size();
    spdlog::debug("Session '{}': prompt tokens={}", params.session_id, n_prompt_tokens);

    auto sparams = llama_sampler_chain_default_params();
    llama_sampler* smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
        params.repeat_last_n, params.repeat_penalty, 0.0f, 0.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(params.top_k));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(params.top_p, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(params.temperature));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    llama_batch batch = llama_batch_get_one(
        reinterpret_cast<llama_token*>(tokens.data()), n_prompt_tokens);
    if (llama_decode(ctx_, batch) != 0) {
        spdlog::error("Failed to decode prompt");
        llama_sampler_free(smpl);
        if (on_done) on_done({});
        return;
    }
    spdlog::debug("Prompt decoded successfully");

    if (!params.session_id.empty()) {
        update_kv_cache_tracking(params.session_id, n_prompt_tokens);
    }

    ReasoningParser reasoning_parser;
    int n_completion = 0;

    for (int i = 0; i < params.max_tokens; ++i) {
        llama_token new_token = llama_sampler_sample(smpl, ctx_, -1);

        if (llama_vocab_is_eog(vocab_, new_token)) break;

        std::string piece = token_to_piece(static_cast<int>(new_token));
        auto parsed = reasoning_parser.feed(piece);

        if (parsed.state == ParseState::THINKING && on_reasoning) {
            on_reasoning(parsed.content);
        } else if (parsed.state == ParseState::ANSWERING && on_token) {
            on_token(parsed.content);
        }

        llama_sampler_accept(smpl, new_token);
        n_completion++;

        batch = llama_batch_get_one(&new_token, 1);
        if (llama_decode(ctx_, batch) != 0) {
            spdlog::warn("Decode failed at token {}", i);
            break;
        }
    }

    InferResult result;
    result.prompt_tokens = n_prompt_tokens;
    result.completion_tokens = n_completion;
    result.reasoning = reasoning_parser.reasoning();
    result.text = reasoning_parser.answer();

    llama_sampler_free(smpl);

    if (!params.session_id.empty()) {
        update_kv_cache_tracking(params.session_id, n_prompt_tokens + n_completion);
    }

    spdlog::debug("Session '{}': complete (prompt={}, completion={})",
                  params.session_id, n_prompt_tokens, n_completion);

    if (on_done) on_done(result);
}

void LlmEngine::activate_session(const std::string& session_id, const std::string& system_prompt) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second.kv_active = true;
        lru_order_.erase(it->second.lru_iter);
        lru_order_.push_front(session_id);
        it->second.lru_iter = std::prev(lru_order_.end());
        return;
    }

    SessionState state;
    state.session_id = session_id;
    state.system_prompt = system_prompt;
    state.prefix_hash = std::hash<std::string>{}(system_prompt);
    state.kv_active = true;

    lru_order_.push_front(session_id);
    state.lru_iter = lru_order_.begin();
    sessions_[session_id] = state;

    prefix_ref_count_[state.prefix_hash]++;
    spdlog::debug("Session '{}' activated, prefix_hash={}", session_id, state.prefix_hash);
}

void LlmEngine::deactivate_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;

    auto& state = it->second;
    if (state.kv_active && state.kv_tokens > 0) {
        total_kv_bytes_ -= state.kv_tokens * 2 * sizeof(float);
    }

    prefix_ref_count_[state.prefix_hash]--;
    if (prefix_ref_count_[state.prefix_hash] <= 0) {
        prefix_ref_count_.erase(state.prefix_hash);
    }

    lru_order_.erase(state.lru_iter);
    sessions_.erase(it);
    spdlog::debug("Session '{}' deactivated", session_id);
}

bool LlmEngine::has_session_cache(const std::string& session_id) const {
    auto it = sessions_.find(session_id);
    return it != sessions_.end() && it->second.kv_active;
}

size_t LlmEngine::session_kv_tokens(const std::string& session_id) const {
    auto it = sessions_.find(session_id);
    return it != sessions_.end() ? it->second.kv_tokens : 0;
}

void LlmEngine::touch_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        lru_order_.erase(it->second.lru_iter);
        lru_order_.push_front(session_id);
        it->second.lru_iter = lru_order_.begin();
    }
}

void LlmEngine::set_max_kv_cache_bytes(size_t bytes) {
    max_kv_bytes_ = bytes;
}

size_t LlmEngine::kv_cache_usage() const {
    return total_kv_bytes_;
}

size_t LlmEngine::kv_cache_max() const {
    return max_kv_bytes_;
}

void LlmEngine::update_kv_cache_tracking(const std::string& session_id, size_t num_tokens) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;

    auto& state = it->second;
    if (state.kv_active && state.kv_tokens > 0) {
        total_kv_bytes_ -= state.kv_tokens * 2 * sizeof(float);
    }

    state.kv_tokens = num_tokens;

    if (state.kv_active) {
        total_kv_bytes_ += num_tokens * 2 * sizeof(float);
    }

    lru_order_.erase(state.lru_iter);
    lru_order_.push_front(session_id);
    state.lru_iter = lru_order_.begin();

    evict_if_needed();
}

void LlmEngine::evict_if_needed() {
    while (total_kv_bytes_ > max_kv_bytes_ && lru_order_.size() > 1) {
        std::string victim = lru_order_.back();
        auto it = sessions_.find(victim);
        if (it != sessions_.end() && it->second.kv_active && it->second.kv_tokens > 0) {
            total_kv_bytes_ -= it->second.kv_tokens * 2 * sizeof(float);
            it->second.kv_active = false;
            it->second.kv_tokens = 0;
            spdlog::info("KV Cache: evicted session '{}' (LRU)", victim);
        }
        lru_order_.pop_back();
    }
}

}
