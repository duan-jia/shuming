#pragma once

#include <string>
#include <vector>

#include "persistence/mysql_pool.h"

namespace fastllm {

struct TenantSummary {
    int id = 0;
    std::string name;
    int token_limit_daily = 0;
    std::string status = "active";
    std::string created_at;
};

class TenantRepository {
public:
    explicit TenantRepository(MysqlPool& mysql);

    bool create_tenant(const std::string& name, int token_limit_daily, int& tenant_id);
    std::vector<TenantSummary> list_tenants();

private:
    MysqlPool& mysql_;

    static std::string escape_sql(MYSQL* mysql, const std::string& input);
};

}
