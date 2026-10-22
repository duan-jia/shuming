#include <cstdlib>
#include <iostream>
#include <string>

#include <muduo/net/EventLoop.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "app/app_bootstrap.h"
#include "mcp/mcp_service.h"
#include "services/tool_service.h"
#include "services/web_search_service.h"
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
    DocumentService documents(ctx);
    AdminService admin(ctx);

    WebSearchOptions web_options;
    web_options.enabled = config.get_bool("web_search.enabled", false);
    web_options.endpoint = config.get_str("web_search.endpoint", "https://api.deepseek.com/v1/tools/search");
    web_options.api_key = config.get_env_str("web_search.api_key", "DEEPSEEK_SEARCH_API_KEY");
    if (web_options.api_key.empty()) {
        web_options.api_key = config.get_env_str("remote_engine.api_key", "DEEPSEEK_API_KEY");
    }
    web_options.ca_bundle = config.get_str("web_search.ca_bundle", "/etc/ssl/certs/ca-certificates.crt");
    web_options.timeout_seconds = config.get_int("web_search.timeout_seconds", 15);
    web_options.max_concurrency = config.get_int("web_search.max_concurrency", 2);
    WebSearchService web_search(web_options);
    ToolService tools(*ctx.knowledge_search_service, web_search);
    McpOptions mcp_options;
    mcp_options.allow_user_web_search = config.get_bool("mcp.allow_user_web_search", false);
    if (config.raw().contains("mcp") && config.raw()["mcp"].contains("allowed_origins")) {
        mcp_options.allowed_origins = config.raw()["mcp"]["allowed_origins"].get<std::vector<std::string>>();
    }
    AuthRequired require_auth = [&auth](const HttpRequest& req, HttpResponse& resp, UserInfo& user) {
        return auth.require_auth(req, resp, user);
    };
    ctx.rate_limiter->add_rule("search_knowledge", config.get_int("mcp.knowledge_requests_per_minute", 60), 60);
    ctx.rate_limiter->add_rule("search_web", config.get_int("mcp.web_requests_per_minute", 20), 60);
    McpService mcp(tools, require_auth, *ctx.rate_limiter, mcp_options);
    AgentOptions agent_options;
    agent_options.enabled = config.get_bool("agent.enabled", true);
    agent_options.max_turns = config.get_int("agent.max_turns", 6);
    agent_options.max_output_tokens = config.get_int("agent.max_output_tokens", 0);
    agent_options.max_total_tokens = config.get_int("agent.max_total_tokens", 24000);
    agent_options.timeout_seconds = config.get_int("agent.timeout_seconds", 90);
    agent_options.max_tool_calls_per_turn = config.get_int("agent.max_tool_calls_per_turn", 4);
    agent_options.max_tool_result_bytes = config.get_int("agent.max_tool_result_bytes", 24000);
    ChatService chat(ctx, tools, agent_options, mcp_options.allow_user_web_search);

    muduo::net::EventLoop loop;
    muduo::net::InetAddress addr(options.port);
    HttpServer server(&loop, addr, "fast-llm-reasoner", config.get_int("http_threads", 2),
                      config.get_int("mcp.worker_threads", 4), config.get_int("mcp.queue_limit", 64));
    server.set_static_dir(options.static_dir);

    for (const auto& endpoint : std::vector<std::pair<std::string, ToolSet>>{
             {"/mcp", ToolSet::ALL}, {"/mcp/knowledge", ToolSet::KNOWLEDGE}, {"/mcp/web", ToolSet::WEB}}) {
        server.router().worker_endpoint(endpoint.first,
            [&mcp, set = endpoint.second](const HttpRequest& req, RouteParams, HttpResponse& resp) {
                mcp.handle(req, resp, set);
            });
    }

    register_health_route(server, ctx);
    register_auth_routes(server, auth);
    register_chat_routes(server, chat);
    register_session_routes(server, *ctx.session_service, require_auth);
    register_document_routes(server, documents, require_auth);
    register_admin_routes(server, admin, require_auth);

    spdlog::info("Starting fast-llm-reasoner on port {}", options.port);
    server.start();
    loop.loop();
    ctx.scheduler->stop();

    return 0;
}
