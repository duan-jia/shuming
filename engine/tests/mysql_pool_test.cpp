#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "persistence/mysql_pool.h"
#include "persistence/mysql_transaction.h"

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string env(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

void expect_query(fastllm::MysqlConnection& conn) {
    auto* result = conn.query("SELECT 42");
    expect(result != nullptr, "query succeeds on acquired connection");
    auto* row = mysql_fetch_row(result);
    expect(row && row[0] && std::string(row[0]) == "42", "query result is correct");
    mysql_free_result(result);
}

}  // namespace

int main() {
    if (!std::getenv("MYSQL_TEST_HOST")) {
        std::cout << "SKIP: set MYSQL_TEST_HOST to an isolated MySQL test server\n";
        return 77;
    }

    const auto host = env("MYSQL_TEST_HOST", "127.0.0.1");
    const int port = std::stoi(env("MYSQL_TEST_PORT", "3306"));
    const auto user = env("MYSQL_TEST_USER", "root");
    const auto password = env("MYSQL_TEST_PASSWORD", "");
    const auto database = env("MYSQL_TEST_DATABASE", "mysql");

    fastllm::MysqlConnection control;
    expect(control.connect(host, port, user, password, database), "control connection opens");

    fastllm::MysqlPool pool(host, port, user, password, database, 1);
    expect(pool.initialize(), "pool initializes");
    auto conn = pool.acquire();
    expect(conn && conn->valid(), "pool provides a valid connection");
    expect_query(*conn);
    const auto initial_id = mysql_thread_id(conn->raw());
    pool.release(conn);
    conn.reset();

    conn = pool.acquire();
    expect(conn && mysql_thread_id(conn->raw()) == initial_id, "live connection is reused");

    // Reproduce production: MySQL expires an idle pooled connection. The very
    // first request after the timeout must succeed, without a sacrificial retry.
    expect(conn->execute("SET SESSION wait_timeout = 1"), "set short idle timeout");
    pool.release(conn);
    conn.reset();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    conn = pool.acquire();
    expect(conn && conn->valid(), "idle connection is recovered before checkout");
    expect(mysql_thread_id(conn->raw()) != initial_id, "expired connection is replaced");
    expect_query(*conn);

    // Also cover a server-side disconnect that is unrelated to idle timeout.
    const auto killed_id = mysql_thread_id(conn->raw());
    pool.release(conn);
    conn.reset();
    expect(control.execute("KILL CONNECTION " + std::to_string(killed_id)), "kill idle connection");
    conn = pool.acquire();
    expect(conn && mysql_thread_id(conn->raw()) != killed_id, "killed idle connection is replaced");
    expect_query(*conn);

    // SQL errors must not cause automatic query replay or unnecessary reconnects.
    const auto live_id = mysql_thread_id(conn->raw());
    expect(conn->query("SELECT FROM") == nullptr, "invalid SQL fails");
    pool.release(conn);
    conn.reset();
    conn = pool.acquire();
    expect(conn && mysql_thread_id(conn->raw()) == live_id, "SQL error preserves live connection");
    expect_query(*conn);

    // A connection lost inside a transaction must fail rather than reconnect
    // and silently commit a different (empty or partial) transaction.
    {
        fastllm::MysqlTransaction transaction(*conn);
        expect(transaction.active(), "transaction starts");
        const auto transaction_id = mysql_thread_id(conn->raw());
        expect(control.execute("KILL CONNECTION " + std::to_string(transaction_id)), "kill active connection");
        expect(!transaction.commit(), "interrupted transaction cannot commit");
    }
    pool.release(conn);
    conn.reset();
    conn = pool.acquire();
    expect(conn && conn->valid(), "pool recovers after interrupted transaction");
    expect_query(*conn);
    pool.release(conn);
    conn.reset();

    // An explicitly closed or null connection must not poison the pool.
    conn = pool.acquire();
    conn->close();
    pool.release(conn);
    conn.reset();
    pool.release(nullptr);
    conn = pool.acquire();
    expect(conn && conn->valid(), "closed connections are replaced");
    expect_query(*conn);
    pool.release(conn);
    conn.reset();

    // Failed mysql_real_connect() still owns client resources. Exercise cleanup
    // and repeated connect attempts on the same object, including destruction.
    fastllm::MysqlConnection retry;
    expect(!retry.connect(host, port, user, password, "__fastllm_nonexistent_test_database__"),
           "connection to missing database fails");
    expect(!retry.valid() && !retry.ping(), "failed connection is invalid");
    expect(retry.connect(host, port, user, password, database), "failed connection can recover");
    expect_query(retry);
    retry.close();
    retry.close();
    expect(!retry.valid() && !retry.ping(), "close is idempotent");

    fastllm::MysqlPool unavailable(host, port, user, password,
                                 "__fastllm_nonexistent_test_database__", 1);
    expect(!unavailable.initialize(), "unavailable pool initialization fails");
    expect(!unavailable.acquire(), "failed reconnect returns no connection");

    std::cout << "PASS: MySQL pool recovers stale connections and preserves transaction failures\n";
}
