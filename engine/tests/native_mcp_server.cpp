// Isolated real-HTTP test fixture: real muduo/JWT/MCP/provider client, fake RAG.
#include <cstdlib>
#include <iostream>
#include <chrono>
#include <thread>
#include <muduo/net/EventLoop.h>
#include "auth/jwt_handler.h"
#include "agent/agent_runtime.h"
#include "inference/remote_engine.h"
#include "mcp/mcp_service.h"
#include "server/http_server.h"

using namespace fastllm;

class FixtureKnowledge : public KnowledgeSearchBackend {
public:
    bool is_ready() const override { return true; }
    QueryRewriteInfo rewrite_query(const std::string& query) const override {
        QueryRewriteInfo result;
        result.original_query = query;
        result.search_query = query;
        return result;
    }
    std::vector<Evidence> retrieve_chunks(const QueryRewriteInfo& query, int, int tenant) override {
        if (query.original_query == "slow-knowledge") std::this_thread::sleep_for(std::chrono::milliseconds(600));
        Evidence item;
        item.document_id = tenant;
        item.filename = "tenant-" + std::to_string(tenant) + ".md";
        item.content = "Tenant evidence";
        item.chunk_index = 0;
        return {item};
    }
    std::vector<Evidence> retrieve_parents(const QueryRewriteInfo& query, int k, int tenant) override {
        auto items = retrieve_chunks(query, k, tenant);
        items[0].mode = KnowledgeSearchMode::PARENTS;
        items[0].matched_chunks = 1;
        return items;
    }
};

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 2;
    JwtHandler jwt("native-mcp-fixture-only");
    FixtureKnowledge backend;
    KnowledgeSearchService knowledge(backend);
    WebSearchOptions options;
    options.enabled = true;
    options.api_key = "fixture-provider-key";
    options.endpoint = argv[2];
    options.fetch_endpoint = std::string(argv[2]).substr(0, std::string(argv[2]).rfind('/')) + "/fetch";
    options.timeout_seconds = 1;
    options.max_concurrency = 1;
    WebSearchService web(options);
    KnowledgeFetchOptions fetch_options;
    fetch_options.pdf_directory = std::getenv("FIXTURE_PDF_DIR") ? std::getenv("FIXTURE_PDF_DIR") : ".tmp/fetch-fixture";
    fetch_options.signing_secret = "fixture-download-secret";
    fetch_options.public_base_url = "http://127.0.0.1:" + std::string(argv[1]);
    fetch_options.link_ttl_seconds = 2;
    KnowledgeFetchService documents([](int id, int tenant, DocumentDetails& document) {
        if (id != tenant && id != 99) return false;
        document.id = id;
        document.source_filename = "fixture paper.pdf";
        document.source_storage_name = "00000000-0000-0000-0000-000000000001.pdf";
        if (id == 99) document.source_storage_name = "../outside.pdf";
        return true;
    }, fetch_options);
    ToolService tools(knowledge, web, documents);
    RateLimiter limiter;
    limiter.add_rule("search_web", 30, 60);
    limiter.add_rule("fetch_web", 30, 60);
    AuthRequired auth = [&jwt](const HttpRequest& req, HttpResponse& resp, UserInfo& user) {
        auto token = req.header("Authorization");
        if (token.rfind("Bearer ", 0) != 0) { resp.json_error(401, "Unauthorized"); return false; }
        auto claims = jwt.verify_token(token.substr(7));
        if (!claims.valid) { resp.json_error(401, "Unauthorized"); return false; }
        user = {claims.user_id, claims.tenant_id, claims.username, claims.role == "admin" ? Role::ADMIN : Role::USER};
        return true;
    };
    McpOptions mcp_options;
    mcp_options.allowed_origins = {"https://allowed.example"};
    McpService mcp(tools, auth, limiter, mcp_options);
    WorkerPool agent_workers(2, 4);
    std::unique_ptr<RemoteEngine> model;
    std::unique_ptr<RemoteEngine> text_model;
    if (argc == 4) model = std::make_unique<RemoteEngine>(argv[3], "fixture-model", "fixture-model-key", 32768, true);
    if (argc == 4) text_model = std::make_unique<RemoteEngine>(argv[3], "fixture-model", "fixture-model-key");
    muduo::net::EventLoop loop;
    HttpServer server(&loop, muduo::net::InetAddress(std::atoi(argv[1]), true), "native-mcp-fixture", 1, 2, 2);
    for (const auto& endpoint : std::vector<std::pair<std::string, ToolSet>>{
             {"/mcp", ToolSet::ALL}, {"/mcp/knowledge", ToolSet::KNOWLEDGE}, {"/mcp/web", ToolSet::WEB}}) {
        server.router().worker_endpoint(endpoint.first, [&mcp, set = endpoint.second](const HttpRequest& req, RouteParams, HttpResponse& resp) {
            mcp.handle(req, resp, set);
        });
    }
    server.router().worker_endpoint("/api/knowledge/download", [&documents](const HttpRequest& req, RouteParams, HttpResponse& resp) {
        documents.download(req, resp);
    });
    if (model) server.router().sse("/api/chat/stream", [&](const HttpRequest& req, RouteParams, std::shared_ptr<SseContext> sse) {
        HttpResponse response;
        UserInfo user{};
        if (!auth(req, response, user)) { sse->send_event("error", "{\"error\":\"Unauthorized\"}"); sse->close(); return; }
        const auto body = nlohmann::json::parse(req.body);
        if (!agent_workers.submit([&, body, sse, user] {
            AgentOptions limits;
            limits.max_turns = body.value("max_turns", 6);
            limits.max_total_tokens = body.value("max_total_tokens", limits.max_total_tokens);
            limits.timeout_seconds = body.value("timeout_seconds", 5);
            InferParams params;
            params.max_tokens = body.value("max_tokens", limits.max_output_tokens);
            params.should_stop = [sse] { return !sse->is_active(); };
            CallerContext caller;
            caller.tenant_id = user.tenant_id;
            caller.principal_id = user.id;
            auto event = [sse](const nlohmann::json& value) { sse->send_event("message", value.dump()); };
            AgentRuntime runtime(body.value("image_inputs", true) ? *model : *text_model, tools, limiter, caller, limits, true, user.role == Role::ADMIN, event,
                [body] { return body.value("remaining_quota", 1000000LL); });
            sse->send_event("message", "{\"type\":\"start\"}");
            runtime.stream_complete(body.value("message", "test"), params,
                [event](const std::string& text) { event({{"type", "answer"}, {"content", text}}); },
                [event](const std::string& text) { event({{"type", "reasoning"}, {"content", text}}); },
                [sse](const InferResult& result) {
                    nlohmann::json output = {{"type", result.error.empty() ? "done" : "error"},
                        {"usage", {{"prompt_tokens", result.prompt_tokens}, {"completion_tokens", result.completion_tokens}, {"estimated", result.usage_estimated}}},
                        {"stop_reason", result.stop_reason}, {"finish_reason", result.finish_reason}, {"truncated", result.finish_reason == "length"}};
                    if (!result.error.empty()) output["error"] = result.error;
                    sse->send_event(result.error.empty() ? "message" : "error", output.dump());
                    sse->close();
                });
        })) { sse->send_event("error", "{\"error\":\"busy\"}"); sse->close(); }
    });
    server.router().get("/healthz", [](const HttpRequest&, RouteParams, HttpResponse& resp) { resp.json("{}"); });
    std::cout << "ADMIN_TOKEN=" << jwt.generate_token(1, 7, "fixture", "admin") << '\n';
    std::cout << "USER_TOKEN=" << jwt.generate_token(2, 8, "fixture-user", "user") << std::endl;
    server.start();
    loop.loop();
}
