#include "security/rbac_manager.h"

namespace fastllm {

RbacManager::RbacManager() {
    init_permissions();
}

void RbacManager::init_permissions() {
    // ADMIN: everything
    auto& admin = role_permissions_[Role::ADMIN];
    admin.insert(perm_key("*", "*"));

    // USER: basic usage
    auto& user = role_permissions_[Role::USER];
    user.insert(perm_key("chat", "read"));
    user.insert(perm_key("chat", "write"));
    user.insert(perm_key("kb", "read"));
    user.insert(perm_key("kb", "write"));
}

bool RbacManager::check_permission(Role role, const std::string& resource, const std::string& action) {
    auto it = role_permissions_.find(role);
    if (it == role_permissions_.end()) return false;

    const auto& perms = it->second;
    if (perms.count(perm_key("*", "*"))) return true;
    if (perms.count(perm_key(resource, action))) return true;
    if (perms.count(perm_key(resource, "*"))) return true;

    return false;
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

std::string RbacManager::perm_key(const std::string& resource, const std::string& action) {
    return resource + ":" + action;
}

}
