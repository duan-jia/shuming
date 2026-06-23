#pragma once

#include <string>
#include <vector>
#include <optional>

namespace fastllm {

class TenantIsolator {
public:
    static std::string inject_tenant_where(const std::string& sql, int tenant_id);
    static std::string inject_tenant_insert(const std::string& table, const std::string& columns,
                                             const std::string& values, int tenant_id);

    static bool check_resource_tenant(const std::string& table, const std::string& id_column,
                                       int resource_id, int tenant_id);

    struct TenantContext {
        int tenant_id;
        int user_id;
        std::string role;
    };
};

}
