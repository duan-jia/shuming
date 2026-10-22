#include "mcp/mcp_service.h"

#include <algorithm>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace fastllm {
namespace {
using Json = nlohmann::json;

bool supported_version(const std::string& version) {
    return version == "2025-03-26" || version == "2025-06-18" || version == "2025-11-25";
}

void rpc_error(HttpResponse& response, const Json& id, int code, const std::string& message, int status = 200) {
    response.set_status(status, status == 200 ? "OK" : "Bad Request");
    response.json(Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}.dump());
}

} // namespace

McpService::McpService(ToolService& tools, AuthRequired authenticate, RateLimiter& limiter, McpOptions options)
    : tools_(tools), authenticate_(std::move(authenticate)), limiter_(limiter), options_(std::move(options)) {}

void McpService::handle(const HttpRequest& req, HttpResponse& response, ToolSet set) {
    response.set_header("Cache-Control", "no-store");
    const auto origin = req.header("Origin");
    if (!origin.empty()) {
        if (std::find(options_.allowed_origins.begin(), options_.allowed_origins.end(), origin) == options_.allowed_origins.end()) {
            response.json_error(403, "Origin is not allowed");
            return;
        }
        response.set_header("Access-Control-Allow-Origin", origin);
        response.set_header("Vary", "Origin");
    }
    if (req.method == HttpMethod::OPTIONS) {
        response.set_status(204, "No Content");
        response.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, MCP-Protocol-Version, MCP-Session-Id, X-Request-Id");
        response.set_header("Access-Control-Allow-Methods", "POST, GET, DELETE, OPTIONS");
        return;
    }
    UserInfo user{};
    if (!authenticate_(req, response, user)) {
        response.set_header("WWW-Authenticate", "Bearer realm=\"shuming-mcp\"");
        return;
    }
    if (user.id <= 0 || user.tenant_id <= 0) {
        response.json_error(403, "Valid tenant identity required");
        return;
    }
    const auto version = req.header("MCP-Protocol-Version", "2025-03-26");
    if (!supported_version(version)) {
        response.json_error(400, "Unsupported MCP protocol version");
        return;
    }
    if (req.method != HttpMethod::POST) {
        response.set_status(405, "Method Not Allowed");
        response.set_header("Allow", "POST, OPTIONS");
        return;
    }
    if (req.header("Content-Type").find("application/json") == std::string::npos) {
        response.json_error(415, "Content-Type must be application/json");
        return;
    }
    const auto accept = req.header("Accept");
    if (accept.find("application/json") == std::string::npos || accept.find("text/event-stream") == std::string::npos) {
        response.json_error(406, "Accept must include application/json and text/event-stream");
        return;
    }
    if (req.body.size() > 64 * 1024) {
        response.json_error(413, "MCP request is too large");
        return;
    }
    const auto message = Json::parse(req.body, nullptr, false);
    if (message.is_discarded()) {
        rpc_error(response, nullptr, -32700, "Parse error", 400);
        return;
    }
    if (!message.is_object() || message.value("jsonrpc", Json()) != "2.0"
        || !message.contains("method") || !message["method"].is_string()
        || (message.contains("id") && !message["id"].is_string() && !message["id"].is_number_integer())
        || (message.contains("params") && !message["params"].is_object())) {
        rpc_error(response, nullptr, -32600, "Invalid Request", 400);
        return;
    }
    const auto method = message["method"].get<std::string>();
    if (!message.contains("id")) {
        // Stateless transport: no session state or unsolicited server messages.
        response.set_status(202, "Accepted");
        return;
    }
    const auto id = message["id"];
    const auto params = message.value("params", Json::object());
    const bool allow_web = user.role == Role::ADMIN || options_.allow_user_web_search;
    Json result;
    try {
        if (method == "initialize") {
            if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string()
                || !params.contains("capabilities") || !params["capabilities"].is_object()
                || !params.contains("clientInfo") || !params["clientInfo"].is_object()
                || !params["clientInfo"].contains("name") || !params["clientInfo"]["name"].is_string()
                || !params["clientInfo"].contains("version") || !params["clientInfo"]["version"].is_string()) {
                throw std::invalid_argument("Invalid initialization parameters");
            }
            const auto requested = params["protocolVersion"].get<std::string>();
            result = {{"protocolVersion", supported_version(requested) ? requested : "2025-11-25"},
                      {"capabilities", {{"tools", Json::object()}}},
                      {"serverInfo", {{"name", set == ToolSet::KNOWLEDGE ? "shuming-knowledge" : set == ToolSet::WEB ? "shuming-web-search" : "shuming-tools"}, {"version", "1.0.0"}}}};
        } else if (method == "ping") {
            result = Json::object();
        } else if (method == "tools/list") {
            result = {{"tools", tools_.list(set, allow_web)}};
        } else if (method == "tools/call") {
            if (!params.contains("name") || !params["name"].is_string()) throw std::invalid_argument("Tool name required");
            const auto name = params["name"].get<std::string>();
            bool available = false;
            for (const auto& tool : tools_.list(set, allow_web)) if (tool["name"] == name) available = true;
            if (!available) throw std::invalid_argument("Tool is unavailable or not permitted on this endpoint");
            const auto arguments = params.value("arguments", Json::object());
            if (!arguments.is_object()) throw std::invalid_argument("arguments must be an object");
            if (!limiter_.allow(user.id, name)) {
                result = {{"isError", true}, {"content", {{{"type", "text"}, {"text", "Tool rate limit exceeded; retry later"}}}}};
            } else {
                CallerContext caller;
                caller.principal_id = user.id;
                caller.tenant_id = user.tenant_id;
                caller.principal_type = "user";
                caller.trace_id = req.header("X-Request-Id");
                try {
                    auto evidence = tools_.execute(name, arguments, caller);
                    result = {{"content", {{{"type", "text"}, {"text", evidence.dump()}}}}, {"structuredContent", evidence}, {"isError", false}};
                } catch (const std::invalid_argument&) {
                    throw;
                } catch (const std::exception& error) {
                    spdlog::warn("MCP tool {} failed for tenant {}: {}", name, user.tenant_id, error.what());
                    result = {{"isError", true}, {"content", {{{"type", "text"}, {"text", "Tool execution failed; check service configuration or retry later"}}}}};
                }
            }
        } else {
            rpc_error(response, id, -32601, "Method not found");
            return;
        }
        response.json(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}.dump());
    } catch (const std::invalid_argument& error) {
        rpc_error(response, id, -32602, error.what());
    } catch (const nlohmann::json::exception&) {
        rpc_error(response, id, -32602, "Invalid parameters");
    } catch (const std::exception& error) {
        spdlog::error("MCP request failed: {}", error.what());
        rpc_error(response, id, -32603, "Internal error");
    }
}

} // namespace fastllm
