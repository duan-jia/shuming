#include "security/tenant_isolator.h"

#include <spdlog/spdlog.h>

namespace fastllm {

std::string TenantIsolator::inject_tenant_where(const std::string& sql, int tenant_id) {
    std::string where = "tenant_id = " + std::to_string(tenant_id);

    size_t where_pos = sql.find("WHERE");
    if (where_pos == std::string::npos) {
        size_t limit_pos = sql.find("LIMIT");
        size_t order_pos = sql.find("ORDER BY");
        size_t insert_pos = sql.size();

        if (order_pos != std::string::npos) insert_pos = order_pos;
        else if (limit_pos != std::string::npos) insert_pos = limit_pos;

        return sql.substr(0, insert_pos) + " WHERE " + where + " " + sql.substr(insert_pos);
    }

    return sql.substr(0, where_pos + 6) + " " + where + " AND " + sql.substr(where_pos + 6);
}

std::string TenantIsolator::inject_tenant_insert(const std::string& table, const std::string& columns,
                                                   const std::string& values, int tenant_id) {
    std::string cols = "tenant_id";
    if (!columns.empty()) cols = "tenant_id," + columns;

    std::string vals = std::to_string(tenant_id);
    if (!values.empty()) vals = vals + "," + values;

    return "INSERT INTO " + table + " (" + cols + ") VALUES (" + vals + ")";
}

bool TenantIsolator::check_resource_tenant(const std::string& table, const std::string& id_column,
                                             int resource_id, int tenant_id) {
    // This would need to query the database
    // For now, it's a placeholder that should be called before any resource access
    spdlog::debug("Tenant isolation check: table={}, {}={}, tenant_id={}",
                  table, id_column, resource_id, tenant_id);
    return true;
}

}
