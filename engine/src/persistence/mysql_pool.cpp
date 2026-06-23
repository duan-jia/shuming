#include "persistence/mysql_pool.h"

#include <spdlog/spdlog.h>
#include <cstring>

namespace fastllm {

MysqlConnection::MysqlConnection() {
    memset(&mysql_, 0, sizeof(mysql_));
}

MysqlConnection::~MysqlConnection() {
    close();
}

bool MysqlConnection::connect(const std::string& host, int port,
                               const std::string& user, const std::string& password,
                               const std::string& database) {
    if (connected_) close();

    mysql_init(&mysql_);
    mysql_options(&mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    unsigned int connect_timeout = 2;
    mysql_options(&mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);

    MYSQL* result = mysql_real_connect(&mysql_, host.c_str(), user.c_str(),
                                        password.c_str(), database.c_str(),
                                        port, nullptr, CLIENT_MULTI_STATEMENTS);
    if (!result) {
        spdlog::error("MySQL connect failed: {}", mysql_error(&mysql_));
        return false;
    }

    connected_ = true;
    spdlog::info("MySQL connected to {}:{}", host, port);
    return true;
}

void MysqlConnection::close() {
    if (connected_) {
        mysql_close(&mysql_);
        connected_ = false;
    }
}

bool MysqlConnection::execute(const std::string& sql) {
    if (!connected_) return false;
    if (mysql_query(&mysql_, sql.c_str()) != 0) {
        spdlog::error("MySQL execute error: {}", mysql_error(&mysql_));
        return false;
    }
    return true;
}

MYSQL_RES* MysqlConnection::query(const std::string& sql) {
    if (!connected_) return nullptr;
    if (mysql_query(&mysql_, sql.c_str()) != 0) {
        spdlog::error("MySQL query error: {}", mysql_error(&mysql_));
        return nullptr;
    }
    return mysql_store_result(&mysql_);
}

MysqlPool::MysqlPool(const std::string& host, int port,
                     const std::string& user, const std::string& password,
                     const std::string& database, size_t pool_size)
    : host_(host), port_(port), user_(user), password_(password),
      database_(database), pool_size_(pool_size) {}

MysqlPool::~MysqlPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pool_.empty()) {
        pool_.pop();
    }
}

bool MysqlPool::initialize() {
    size_t connected = 0;
    for (size_t i = 0; i < pool_size_; ++i) {
        auto conn = std::make_shared<MysqlConnection>();
        if (conn->connect(host_, port_, user_, password_, database_)) {
            pool_.push(conn);
            connected++;
        } else {
            spdlog::warn("MySQL connection {}/{} failed", i + 1, pool_size_);
        }
    }
    if (connected > 0) {
        spdlog::info("MySQL pool initialized with {} connections", connected);
        initialized_ = true;
        return true;
    }
    spdlog::critical("MySQL pool initialization failed, all connections refused");
    initialized_ = false;
    return false;
}

MysqlPool::ConnPtr MysqlPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (pool_.empty()) {
        auto conn = std::make_shared<MysqlConnection>();
        if (conn->connect(host_, port_, user_, password_, database_)) {
            return conn;
        }
        return nullptr;
    }

    auto conn = pool_.front();
    pool_.pop();

    if (!conn->valid()) {
        if (!conn->connect(host_, port_, user_, password_, database_)) {
            return nullptr;
        }
    }

    return conn;
}

void MysqlPool::release(ConnPtr conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.push(conn);
    }
    cv_.notify_one();
}

}
