#pragma once

#include <functional>
#include <vector>
#include "routes/auth_required.h"
#include "security/rate_limiter.h"
#include "services/tool_service.h"

namespace fastllm {

struct McpOptions {
    std::vector<std::string> allowed_origins;
    bool allow_user_web_search = false;
};

// Stateless Streamable HTTP: JSON responses; no server-initiated SSE stream.
// One backend, two scoped MCP endpoints, plus a combined /mcp endpoint.
class McpService {
public:
    McpService(ToolService& tools, AuthRequired authenticate, RateLimiter& limiter, McpOptions options);
    void handle(const HttpRequest& request, HttpResponse& response, ToolSet set);

private:
    ToolService& tools_;
    AuthRequired authenticate_;
    RateLimiter& limiter_;
    McpOptions options_;
};

} // namespace fastllm
