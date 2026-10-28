#include "persistence/tenant_repository.h"

#include <mysql/mysql.h>

namespace fastllm {

TenantRepository::TenantRepository(MysqlPool& mysql) : mysql_(mysql) {}

std::string TenantRepository::escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

bool TenantRepository::create_tenant(const std::string& name, long long token_limit_daily, int& tenant_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "INSERT INTO tenants (name, token_limit_daily) VALUES ('"
        + escape_sql(conn->raw(), name) + "',"
        + std::to_string(token_limit_daily) + ")";
    bool ok = conn->execute(sql);
    if (ok) {
        tenant_id = static_cast<int>(mysql_insert_id(conn->raw()));
    }
    mysql_.release(conn);
    return ok;
}

bool TenantRepository::ensure_guest_tenant(int& tenant_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;
    // Keep the legacy tenant ID so existing guest sessions remain isolated
    // in the same group after its display name changes.
    bool ok = conn->execute(std::string("UPDATE IGNORE tenants SET name='") + kGuestTenantName + "' WHERE name='__guest__'");
    if (ok) ok = conn->execute(std::string("INSERT IGNORE INTO tenants (name) VALUES ('") + kGuestTenantName + "')");
    auto* res = ok ? conn->query(std::string("SELECT id FROM tenants WHERE name='") + kGuestTenantName + "' AND status='active' LIMIT 1") : nullptr;
    auto* row = res ? mysql_fetch_row(res) : nullptr;
    if (row && row[0]) tenant_id = std::stoi(row[0]);
    if (res) mysql_free_result(res);
    mysql_.release(conn);
    return tenant_id > 0;
}

std::vector<TenantSummary> TenantRepository::list_tenants() {
    std::vector<TenantSummary> tenants;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return tenants;

    auto* res = conn->query("SELECT id, name, token_limit_daily, status, created_at FROM tenants ORDER BY id");
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            TenantSummary tenant;
            tenant.id = row[0] ? std::stoi(row[0]) : 0;
            tenant.name = row[1] ? row[1] : "";
            tenant.token_limit_daily = row[2] ? std::stoll(row[2]) : 0;
            tenant.status = row[3] ? row[3] : "active";
            tenant.created_at = row[4] ? row[4] : "";
            tenants.push_back(tenant);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return tenants;
}

}
