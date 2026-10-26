#include "security/rbac_manager.h"

namespace fastllm {

bool RbacManager::check_permission(Role role, Permission permission) {
    if (role != Role::ADMIN && role != Role::USER && role != Role::GUEST) return false;
    switch (permission) {
        case Permission::CHAT_WRITE:
        case Permission::SESSION_READ:
        case Permission::SESSION_CREATE:
        case Permission::SESSION_DELETE:
            return true;
        case Permission::DOCUMENT_READ:
        case Permission::DOCUMENT_UPLOAD:
        case Permission::DOCUMENT_DELETE:
        case Permission::KNOWLEDGE_SEARCH:
        case Permission::MCP_ACCESS:
        case Permission::WEB_USE:
            return true;
        case Permission::PUBLIC_LIBRARY_WRITE:
        case Permission::DOCUMENT_APPROVE:
        case Permission::USER_MANAGE:
        case Permission::TENANT_MANAGE:
            return role == Role::ADMIN;
    }
    return false;
}

bool RbacManager::check_route_permission(Role role, const std::string& method, const std::string& path) {
    auto allows = [role](Permission permission) { return check_permission(role, permission); };
    if (path == "/api/chat/stream" || path == "/api/chat/cancel") {
        return method == "POST" && allows(Permission::CHAT_WRITE);
    }
    if (path == "/api/sessions") {
        if (method == "GET") return allows(Permission::SESSION_READ);
        return method == "POST" && allows(Permission::SESSION_CREATE);
    }
    const std::string sessions = "/api/sessions/";
    if (path.rfind(sessions, 0) == 0) {
        const auto tail = path.substr(sessions.size());
        const auto slash = tail.find('/');
        if (tail.empty() || slash == 0) return false;
        if (slash == std::string::npos) {
            if (method == "GET") return allows(Permission::SESSION_READ);
            return method == "DELETE" && allows(Permission::SESSION_DELETE);
        }
        return tail.substr(slash) == "/messages" && method == "GET" && allows(Permission::SESSION_READ);
    }
    if (path == "/api/documents") {
        if (method == "GET") return allows(Permission::DOCUMENT_READ);
        return method == "POST" && allows(Permission::DOCUMENT_UPLOAD);
    }
    if (path == "/api/documents/search") return method == "POST" && allows(Permission::KNOWLEDGE_SEARCH);
    if (path == "/api/documents/deduplicate") return method == "POST" && allows(Permission::DOCUMENT_UPLOAD);
    const std::string documents = "/api/documents/";
    if (path.rfind(documents, 0) == 0) {
        const auto tail = path.substr(documents.size());
        const auto slash = tail.find('/');
        const auto id = tail.substr(0, slash);
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) return false;
        if (slash == std::string::npos) {
            if (method == "GET") return allows(Permission::DOCUMENT_READ);
            return method == "DELETE" && allows(Permission::DOCUMENT_DELETE);
        }
        return tail.substr(slash) == "/approve" && method == "POST" && allows(Permission::DOCUMENT_APPROVE);
    }
    if (path == "/api/users" || path == "/api/tenants") {
        return (method == "GET" || method == "POST")
            && allows(path == "/api/users" ? Permission::USER_MANAGE : Permission::TENANT_MANAGE);
    }
    const std::string users = "/api/users/";
    if (path.rfind(users, 0) == 0) {
        const auto id = path.substr(users.size());
        return !id.empty() && id.find_first_not_of("0123456789") == std::string::npos
            && method == "DELETE" && allows(Permission::USER_MANAGE);
    }
    if (path == "/mcp" || path == "/mcp/knowledge" || path == "/mcp/web") {
        // The transport returns its own 405 for unsupported authenticated methods.
        return allows(Permission::MCP_ACCESS);
    }
    return false;
}

bool RbacManager::can_use_web(Role role, bool allow_user_web_search) {
    return check_permission(role, Permission::WEB_USE)
        && (role == Role::ADMIN || allow_user_web_search);
}

bool RbacManager::can_upload_document(const UserInfo& user, const std::string& visibility) {
    if (user.id <= 0 || user.tenant_id <= 0
        || !check_permission(user.role, Permission::DOCUMENT_UPLOAD)) return false;
    if (visibility == "public") return check_permission(user.role, Permission::PUBLIC_LIBRARY_WRITE);
    return visibility == "group";
}

bool RbacManager::can_delete_document(const UserInfo& user, int tenant_id, const std::string& visibility) {
    if (user.id <= 0 || tenant_id <= 0 || user.tenant_id <= 0
        || !check_permission(user.role, Permission::DOCUMENT_DELETE)) return false;
    if (visibility == "public") return check_permission(user.role, Permission::PUBLIC_LIBRARY_WRITE);
    return visibility == "group" && user.tenant_id == tenant_id;
}

bool RbacManager::can_approve_document(const UserInfo& user, int tenant_id, const std::string& visibility) {
    return check_permission(user.role, Permission::DOCUMENT_APPROVE)
        && can_delete_document(user, tenant_id, visibility);
}

std::string RbacManager::role_to_string(Role role) {
    switch (role) {
        case Role::ADMIN: return "admin";
        case Role::USER: return "user";
        case Role::GUEST: return "guest";
        default: return "unknown";
    }
}

Role RbacManager::string_to_role(const std::string& str) {
    if (str == "admin") return Role::ADMIN;
    if (str == "user") return Role::USER;
    if (str == "guest") return Role::GUEST;
    return Role::UNKNOWN;
}

}
