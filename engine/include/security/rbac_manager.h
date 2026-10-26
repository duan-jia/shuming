#pragma once

#include <string>

namespace fastllm {

enum class Role {
    ADMIN,
    USER,
    GUEST,
    UNKNOWN
};

struct UserInfo {
    int id;
    int tenant_id;
    std::string username;
    Role role;
};

enum class Permission {
    CHAT_WRITE,
    SESSION_READ,
    SESSION_CREATE,
    SESSION_DELETE,
    DOCUMENT_READ,
    DOCUMENT_UPLOAD,
    DOCUMENT_DELETE,
    DOCUMENT_APPROVE,
    PUBLIC_LIBRARY_WRITE,
    KNOWLEDGE_SEARCH,
    MCP_ACCESS,
    WEB_USE,
    USER_MANAGE,
    TENANT_MANAGE
};

class RbacManager {
public:
    static bool check_permission(Role role, Permission permission);
    // Unknown routes are denied, including for administrators. MCP owns its
    // transport method errors; REST routes match their permitted methods.
    static bool check_route_permission(Role role, const std::string& method, const std::string& path);
    static bool can_use_web(Role role, bool allow_user_web_search);
    static bool can_upload_document(const UserInfo& user, const std::string& visibility);
    static bool can_delete_document(const UserInfo& user, int tenant_id, const std::string& visibility);
    static bool can_approve_document(const UserInfo& user, int tenant_id, const std::string& visibility);

    static std::string role_to_string(Role role);
    static Role string_to_role(const std::string& str);

};

}
