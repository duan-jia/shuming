#include "inference/model_manager.h"
#include "inference/llm_engine.h"

#include <spdlog/spdlog.h>

namespace fastllm {

bool ModelManager::load_model(const std::string& model_name, const std::string& model_path,
                               const InferParams& params) {
    auto engine = std::make_unique<LlmEngine>();

    if (!engine->load_model(model_path, params)) {
        spdlog::error("Failed to load model: {}", model_name);
        return false;
    }

    engines_[model_name] = std::move(engine);
    active_engine_ = model_name;
    spdlog::info("Model '{}' loaded and set as active", model_name);
    return true;
}

void ModelManager::unload_model(const std::string& model_name) {
    auto it = engines_.find(model_name);
    if (it == engines_.end()) return;
    engines_.erase(it);
    if (active_engine_ == model_name) {
        active_engine_ = engines_.empty() ? "" : engines_.begin()->first;
    }
}

void ModelManager::add_engine(const std::string& name, std::unique_ptr<IInferenceEngine> engine) {
    engines_[name] = std::move(engine);
    if (active_engine_.empty()) active_engine_ = name;
    spdlog::info("Engine '{}' registered ({})", name, engines_[name]->model_name());
}

IInferenceEngine* ModelManager::get_engine(const std::string& model_name) {
    auto it = engines_.find(model_name);
    if (it != engines_.end()) {
        return it->second.get();
    }
    if (model_name.empty() && !active_engine_.empty()) {
        return engines_[active_engine_].get();
    }
    return nullptr;
}

std::vector<std::string> ModelManager::loaded_models() const {
    std::vector<std::string> result;
    for (const auto& [name, _] : engines_) {
        result.push_back(name);
    }
    return result;
}

}
