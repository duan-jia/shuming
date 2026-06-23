#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fastllm {

enum class Role {
    ADMIN,
    USER
};

struct UserInfo {
    int id;
    int tenant_id;
    std::string username;
    Role role;
};

class RbacManager {
public:
    RbacManager();

    bool check_permission(Role role, const std::string& resource, const std::string& action);

    static std::string role_to_string(Role role);
    static Role string_to_role(const std::string& str);

private:
    using Permission = std::pair<std::string, std::string>;
    using PermissionSet = std::unordered_set<std::string>;

    std::unordered_map<Role, PermissionSet> role_permissions_;

    void init_permissions();
    static std::string perm_key(const std::string& resource, const std::string& action);
};

}
