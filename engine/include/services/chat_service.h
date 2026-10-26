#pragma once

#include <memory>
#include <string>

#include "app/app_context.h"
#include "routes/auth_required.h"
#include "agent/agent_runtime.h"
#include "server/http_protocol.h"
#include "server/router.h"
#include "server/sse_connection.h"

namespace fastllm {

class ChatService {
public:
    ChatService(AppContext& ctx, ToolService& tools, AgentOptions options, bool allow_user_web_search, AuthRequired authenticate);
    void cancel(const HttpRequest& req, HttpResponse& response);

    void stream(const HttpRequest& req, const RouteParams& params, std::shared_ptr<SseContext> sse_ctx);

private:
    struct ActiveRequests;
    std::shared_ptr<ActiveRequests> active_;
    AppContext& ctx_;
    ToolService& tools_;
    AgentOptions options_;
    bool allow_user_web_search_;
    AuthRequired authenticate_;

    static std::string generate_uuid();
    static void send_sse_error(const std::shared_ptr<SseContext>& sse_ctx, const std::string& message);
};

}
