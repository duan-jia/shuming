#include <cstdlib>
#include <iostream>
#include <string>

#include <muduo/net/EventLoop.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "app/app_bootstrap.h"
#include "routes/admin_routes.h"
#include "routes/auth_routes.h"
#include "routes/chat_routes.h"
#include "routes/document_routes.h"
#include "routes/session_routes.h"
#include "server/http_server.h"
#include "service/config_manager.h"
#include "services/admin_service.h"
#include "services/auth_service.h"
#include "services/chat_service.h"
#include "services/document_service.h"

using namespace fastllm;

namespace {

void apply_cli_args(int argc, char* argv[], AppOptions& options, std::string& config_path) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            options.port = std::atoi(argv[++i]);
        } else if (arg == "--model" && i + 1 < argc) {
            options.model_path = argv[++i];
        } else if (arg == "--embedding-model" && i + 1 < argc) {
            options.embedding_model_path = argv[++i];
        } else if (arg == "--static" && i + 1 < argc) {
            options.static_dir = argv[++i];
        } else if (arg == "--rag-dir" && i + 1 < argc) {
            options.rag_dir = argv[++i];
        }
    }
}

void register_health_route(HttpServer& server, AppContext& ctx) {
    server.router().get("/api/health", [&ctx](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        nlohmann::json j;
        j["status"] = "ok";
        j["service"] = "fast-llm-reasoner";
        j["mysql"] = "connected";
        j["model_loaded"] = ctx.model_mgr->get_engine("") != nullptr;
        resp.json(j.dump());
    });
}

}

int main(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::debug);

    std::string config_path = "config.json";
    AppOptions options;
    apply_cli_args(argc, argv, options, config_path);

    ConfigManager config;
    if (!config.load(config_path)) {
        spdlog::warn("Could not load config.json ({}), using defaults", config.last_error());
    }

    if (options.port == 0) {
        options.port = config.get_int("server.port", 8080);
    }
    if (options.static_dir.empty()) {
        options.static_dir = config.get_str("server.static_dir", "");
    }

    AppContext ctx;
    if (!AppBootstrap::initialize(ctx, config, options)) {
        return 1;
    }

    AuthService auth(ctx);
    ChatService chat(ctx);
    DocumentService documents(ctx);
    AdminService admin(ctx);

    muduo::net::EventLoop loop;
    muduo::net::InetAddress addr(options.port);
    HttpServer server(&loop, addr, "fast-llm-reasoner", config.get_int("http_threads", 2));
    server.set_static_dir(options.static_dir);

    AuthRequired require_auth = [&auth](const HttpRequest& req, HttpResponse& resp, UserInfo& user) {
        return auth.require_auth(req, resp, user);
    };

    register_health_route(server, ctx);
    register_auth_routes(server, auth);
    register_chat_routes(server, chat);
    register_session_routes(server, *ctx.session_service, require_auth);
    register_document_routes(server, documents, require_auth);
    register_admin_routes(server, admin, require_auth);

    spdlog::info("Starting fast-llm-reasoner on port {}", options.port);
    server.start();
    loop.loop();

    return 0;
}
