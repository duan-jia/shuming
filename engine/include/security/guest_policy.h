#pragma once

#include "security/rbac_manager.h"

namespace fastllm {
// Compatibility entry point; guest routes use the same policy as other roles.
inline bool guest_route_allowed(const std::string& method, const std::string& path) {
    return RbacManager::check_route_permission(Role::GUEST, method, path);
}
}
