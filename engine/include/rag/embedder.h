#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <mutex>

struct llama_model;
struct llama_context;
struct llama_vocab;

namespace fastllm {

class Embedder {
public:
    virtual ~Embedder() = default;

    virtual bool load_model(const std::string& model_path) = 0;
    virtual std::vector<float> embed(const std::string& text) = 0;
    virtual size_t embedding_dim() const = 0;
    virtual bool is_loaded() const = 0;
};

class LlamaEmbedder : public Embedder {
public:
    LlamaEmbedder();
    ~LlamaEmbedder() override;

    bool load_model(const std::string& model_path) override;
    bool init_from_model(llama_model* shared_model);
    std::vector<float> embed(const std::string& text) override;
    size_t embedding_dim() const override { return dim_; }
    bool is_loaded() const override { return model_ != nullptr; }

private:
    bool create_context();
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    size_t dim_ = 0;
    bool owning_model_ = true;
    mutable std::mutex mutex_;
};

}
