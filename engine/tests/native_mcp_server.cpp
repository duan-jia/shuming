// Isolated real-HTTP test fixture: real muduo/JWT/MCP/provider client, fake RAG.
#include <cstdlib>
#include <iostream>
#include <chrono>
#include <thread>
#include <muduo/net/EventLoop.h>
#include "auth/jwt_handler.h"
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
    if (argc != 3) return 2;
    JwtHandler jwt("native-mcp-fixture-only");
    FixtureKnowledge backend;
    KnowledgeSearchService knowledge(backend);
    WebSearchOptions options;
    options.enabled = true;
    options.api_key = "fixture-provider-key";
    options.endpoint = argv[2];
    options.timeout_seconds = 1;
    options.max_concurrency = 1;
    WebSearchService web(options);
    ToolService tools(knowledge, web);
    RateLimiter limiter;
    limiter.add_rule("search_web", 30, 60);
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
    muduo::net::EventLoop loop;
    HttpServer server(&loop, muduo::net::InetAddress(std::atoi(argv[1]), true), "native-mcp-fixture", 1, 2, 2);
    for (const auto& endpoint : std::vector<std::pair<std::string, ToolSet>>{
             {"/mcp", ToolSet::ALL}, {"/mcp/knowledge", ToolSet::KNOWLEDGE}, {"/mcp/web", ToolSet::WEB}}) {
        server.router().worker_endpoint(endpoint.first, [&mcp, set = endpoint.second](const HttpRequest& req, RouteParams, HttpResponse& resp) {
            mcp.handle(req, resp, set);
        });
    }
    server.router().get("/healthz", [](const HttpRequest&, RouteParams, HttpResponse& resp) { resp.json("{}"); });
    std::cout << "ADMIN_TOKEN=" << jwt.generate_token(1, 7, "fixture", "admin") << '\n';
    std::cout << "USER_TOKEN=" << jwt.generate_token(2, 8, "fixture-user", "user") << std::endl;
    server.start();
    loop.loop();
}
