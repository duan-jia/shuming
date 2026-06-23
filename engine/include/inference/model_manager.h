#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "inference/i_engine.h"

namespace fastllm {

class ModelManager {
public:
    ModelManager() = default;

    bool load_model(const std::string& model_name, const std::string& model_path,
                    const InferParams& params);
    void unload_model(const std::string& model_name);
    void add_engine(const std::string& name, std::unique_ptr<IInferenceEngine> engine);

    IInferenceEngine* get_engine(const std::string& model_name);
    std::vector<std::string> loaded_models() const;

private:
    std::unordered_map<std::string, std::unique_ptr<IInferenceEngine>> engines_;
    std::string active_engine_;
};

}
