#include "rag/embedder.h"

#include <spdlog/spdlog.h>
#include <llama.h>
#include <algorithm>
#include <numeric>

namespace fastllm {

LlamaEmbedder::LlamaEmbedder() = default;

LlamaEmbedder::~LlamaEmbedder() {
    if (ctx_) { llama_free(ctx_); ctx_ = nullptr; }
    if (model_ && owning_model_) { llama_model_free(model_); model_ = nullptr; }
}

bool LlamaEmbedder::load_model(const std::string& model_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;

    model_ = llama_model_load_from_file(model_path.c_str(), mparams);
    if (!model_) {
        spdlog::error("Failed to load embedding model: {}", model_path);
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);
    return create_context();
}

bool LlamaEmbedder::init_from_model(llama_model* shared_model) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!shared_model) {
        spdlog::error("init_from_model: null model");
        return false;
    }
    model_ = shared_model;
    owning_model_ = false;
    vocab_ = llama_model_get_vocab(model_);
    return create_context();
}

bool LlamaEmbedder::create_context() {
    auto cparams = llama_context_default_params();
    cparams.n_ctx = 512;
    cparams.n_batch = 512;
    cparams.n_threads = 2;
    cparams.embeddings = true;

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) {
        spdlog::error("Failed to create embedding context");
        if (owning_model_) {
            llama_model_free(model_);
        }
        model_ = nullptr;
        return false;
    }

    dim_ = llama_n_embd(model_);

    auto ptype = llama_pooling_type(ctx_);
    spdlog::info("Embedding model loaded: dim={}, pooling={}", dim_, static_cast<int>(ptype));
    return true;
}

std::vector<float> LlamaEmbedder::embed(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!model_ || !ctx_ || !vocab_) return {};

    std::vector<llama_token> tokens(256);
    int n = llama_tokenize(vocab_, text.c_str(), static_cast<int>(text.size()),
                           tokens.data(), tokens.size(), true, false);
    if (n < 0) {
        tokens.resize(-n);
        n = llama_tokenize(vocab_, text.c_str(), static_cast<int>(text.size()),
                           tokens.data(), tokens.size(), true, false);
    }
    tokens.resize(n > 0 ? n : 0);
    if (tokens.empty()) return {};

    int ctx_size = llama_n_ctx(ctx_);
    if (static_cast<int>(tokens.size()) >= ctx_size) {
        tokens.resize(ctx_size - 1);
    }

    llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());

    if (llama_decode(ctx_, batch) != 0) {
        spdlog::error("Embedding decode failed");
        return {};
    }

    llama_memory_clear(llama_get_memory(ctx_), true);

    auto pooling = llama_pooling_type(ctx_);
    const float* embd = nullptr;

    if (pooling == LLAMA_POOLING_TYPE_NONE || pooling == LLAMA_POOLING_TYPE_UNSPECIFIED) {
        embd = llama_get_embeddings(ctx_);
        if (!embd) return {};
        embd += (tokens.size() - 1) * dim_;
    } else {
        embd = llama_get_embeddings_seq(ctx_, 0);
        if (!embd) return {};
    }

    std::vector<float> result(embd, embd + dim_);

    float norm = 0.0f;
    for (float v : result) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0) {
        for (float& v : result) v /= norm;
    }

    return result;
}

}
