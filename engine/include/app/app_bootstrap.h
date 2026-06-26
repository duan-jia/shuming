#pragma once

#include <string>

#include "app/app_context.h"
#include "service/config_manager.h"

namespace fastllm {

struct AppOptions {
    int port = 0;
    std::string model_path;
    std::string embedding_model_path;
    std::string static_dir;
    std::string rag_dir;
};

class AppBootstrap {
public:
    static bool initialize(AppContext& ctx, const ConfigManager& config, const AppOptions& options);

private:
    static bool ensure_admin(AppContext& ctx, const ConfigManager& config);
};

}
