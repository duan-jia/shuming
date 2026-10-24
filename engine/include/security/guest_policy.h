#pragma once
#include "security/login_limiter.h"

namespace fastllm {
// Public capability boundary; unknown/new routes are not implicitly guest-safe.
inline bool guest_route_allowed(const std::string& method, const std::string& path) {
    if (path == "/api/chat/stream" || path == "/api/chat/cancel") return method == "POST";
    if (path == "/api/sessions") return method == "GET" || method == "POST";
    if (path.rfind("/api/sessions/", 0) == 0) return method == "GET" || method == "DELETE";
    return false; // No document export, management, MCP, arbitrary tools, or admin APIs.
}
}
