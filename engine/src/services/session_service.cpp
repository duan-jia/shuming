#include "services/session_service.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <random>

namespace fastllm {

SessionService::SessionService(SessionRepository& sessions) : sessions_(sessions) {}

std::string SessionService::generate_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    uint32_t d[4] = {dist(gen), dist(gen), dist(gen), dist(gen)};
    char buf[37];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
        d[0],
        (d[1] >> 16) & 0xFFFF,
        d[1] & 0x0FFF | 0x4000,
        (d[2] >> 16) & 0x3FFF | 0x8000,
        d[2] & 0xFFFF,
        d[3]);
    return std::string(buf);
}

void SessionService::list(const UserInfo& user, HttpResponse& resp) {
    nlohmann::json j;
    j["sessions"] = sessions_.list_user_sessions(user.tenant_id, user.id);
    resp.json(j.dump());
}

void SessionService::create(const HttpRequest& req, const UserInfo& user, HttpResponse& resp) {
    try {
        auto body = nlohmann::json::parse(req.body);
        std::string title = body.value("title", "New Chat");
        std::string model = body.value("model", "qwen3b-q4");
        std::string session_id = generate_uuid();

        if (!sessions_.create_session(session_id, user.tenant_id, user.id, title, model)) {
            resp.json_error(500, "Failed to create session");
            return;
        }

        nlohmann::json j;
        j["id"] = session_id;
        j["title"] = title;
        j["model"] = model;
        j["status"] = "created";
        resp.json(j.dump());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

void SessionService::detail(const RouteParams& params, const UserInfo& user, HttpResponse& resp) {
    std::string session_id = params.get("id");
    if (session_id.empty()) {
        resp.json_error(400, "Session ID required");
        return;
    }

    nlohmann::json j;
    if (!sessions_.get_session(session_id, user.tenant_id, user.id, j)) {
        resp.json_error(404, "Session not found");
        return;
    }

    j["messages"] = sessions_.list_messages(session_id);
    resp.json(j.dump());
}

void SessionService::remove(const RouteParams& params, const UserInfo& user, HttpResponse& resp) {
    std::string session_id = params.get("id");
    if (session_id.empty()) {
        resp.json_error(400, "Session ID required");
        return;
    }

    sessions_.delete_session(session_id, user.tenant_id, user.id);
    resp.json(R"({"status":"deleted"})");
}

void SessionService::messages(const RouteParams& params, const UserInfo& user, HttpResponse& resp) {
    std::string session_id = params.get("id");
    if (session_id.empty()) {
        resp.json_error(400, "Session ID required");
        return;
    }

    nlohmann::json session;
    if (!sessions_.get_session(session_id, user.tenant_id, user.id, session)) {
        resp.json_error(404, "Session not found");
        return;
    }

    nlohmann::json j;
    j["messages"] = sessions_.list_messages(session_id);
    resp.json(j.dump());
}

}
