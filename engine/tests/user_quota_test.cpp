#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "persistence/billing_service.h"
#include "persistence/tenant_repository.h"
#include "persistence/user_repository.h"

using namespace fastllm;

void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

std::string env(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

int main() {
    if (!std::getenv("MYSQL_TEST_HOST")) {
        std::cout << "SKIP: requires an isolated MySQL database with sql/init.sql\n";
        return 77;
    }
    MysqlPool pool(env("MYSQL_TEST_HOST", "127.0.0.1"), std::stoi(env("MYSQL_TEST_PORT", "3306")),
                   env("MYSQL_TEST_USER", "root"), env("MYSQL_TEST_PASSWORD", ""),
                   env("MYSQL_TEST_DATABASE", "llm_platform"), 4);
    expect(pool.initialize(), "pool initializes");
    TenantRepository tenants(pool);
    UserRepository users(pool);
    BillingService billing(pool);
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    int tenant = 0, user = 0, other = 0;
    expect(tenants.create_tenant("quota-test-" + suffix, 5000000000LL, tenant), "BIGINT tenant quota");
    expect(users.create_user(tenant, "quota-test-" + suffix, "disabled", "user", user, 3000000000LL), "create with override");
    expect(users.create_user(tenant, "quota-other-" + suffix, "disabled", "user", other), "create inheriting");
    expect(billing.check_quota(tenant, user).token_limit == 3000000000LL, "user override above INT_MAX");
    expect(billing.check_quota(tenant, other).token_limit == 5000000000LL, "tenant inheritance above INT_MAX");
    expect(billing.charge(tenant, user, "", "quota-test-" + suffix, 100, 50).success, "settle usage");
    expect(users.set_daily_token_quota(user, 100) == UserLookupStatus::Found, "lower quota");
    auto usage = billing.check_quota(tenant, user);
    expect(!usage.success && usage.remaining == 0 && usage.tokens_used == 150, "lowering preserves usage and clamps remaining");
    expect(users.set_daily_token_quota(user, std::nullopt) == UserLookupStatus::Found, "restore inheritance");
    usage = billing.check_quota(tenant, user);
    expect(usage.token_limit == 5000000000LL && usage.tokens_used == 150, "inheritance updates today without resetting usage");
    expect(users.set_daily_token_quota(user, kMaxDailyTokenQuota) == UserLookupStatus::Found, "maximum safe quota");
    auto conn = pool.acquire();
    expect(conn->execute("UPDATE user_token_usage_daily SET usage_date=DATE_SUB(CURDATE(),INTERVAL 1 DAY) WHERE user_id=" + std::to_string(user)), "simulate date rollover");
    pool.release(conn);
    usage = billing.check_quota(tenant, user);
    expect(usage.token_limit == kMaxDailyTokenQuota && usage.tokens_used == 0, "next day retains override");
    expect(users.set_daily_token_quota(user, 0) == UserLookupStatus::Unavailable, "reject zero");
    expect(users.set_daily_token_quota(user, -1) == UserLookupStatus::Unavailable, "reject negative");
    expect(users.set_daily_token_quota(user, kMaxDailyTokenQuota + 1) == UserLookupStatus::Unavailable, "reject unsafe JSON integer");
    expect(users.set_daily_token_quota(2147483647, 1) == UserLookupStatus::NotFound, "unknown user");
    std::atomic<bool> ok{true};
    std::thread charges([&] {
        for (int i = 0; i < 20; ++i) {
            if (!billing.charge(tenant, user, "", "concurrent-" + suffix + "-" + std::to_string(i), 10, 5).success) ok = false;
        }
    });
    std::thread edits([&] {
        for (int i = 0; i < 20; ++i) {
            if (users.set_daily_token_quota(user, 10000000 + i) != UserLookupStatus::Found) ok = false;
        }
    });
    charges.join(); edits.join();
    usage = billing.check_quota(tenant, user);
    expect(ok && usage.tokens_used == 300 && usage.token_limit == 10000019, "concurrent edits and billing preserve every charge");
    expect(billing.check_quota(tenant, other).token_limit == 5000000000LL, "other user unaffected");
    const auto summaries = users.list_users();
    bool found = false;
    for (const auto& summary : summaries) {
        if (summary.id == user) found = summary.daily_token_quota == 10000019
            && summary.today_tokens_used == 300 && summary.effective_daily_token_quota == 10000019;
    }
    expect(found, "management summary exposes effective quota and usage");
    conn = pool.acquire();
    expect(conn->execute("DELETE FROM tenants WHERE id=" + std::to_string(tenant)), "remove test tenant and cascade fixtures");
    pool.release(conn);
    std::cout << "PASS: inheritance, overrides, rollover, bounds, usage preservation, concurrent billing\n";
}
