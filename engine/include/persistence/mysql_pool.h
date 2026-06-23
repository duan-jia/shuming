#pragma once

#include <mysql/mysql.h>
#include <memory>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace fastllm {

class MysqlConnection {
public:
    MysqlConnection();
    ~MysqlConnection();

    bool connect(const std::string& host, int port,
                 const std::string& user, const std::string& password,
                 const std::string& database);
    void close();

    MYSQL* raw() { return &mysql_; }
    bool valid() const { return connected_; }

    bool execute(const std::string& sql);
    MYSQL_RES* query(const std::string& sql);

private:
    MYSQL mysql_;
    bool connected_ = false;
};

class MysqlPool {
public:
    using ConnPtr = std::shared_ptr<MysqlConnection>;

    MysqlPool(const std::string& host, int port,
              const std::string& user, const std::string& password,
              const std::string& database, size_t pool_size = 4);
    ~MysqlPool();

    ConnPtr acquire();
    void release(ConnPtr conn);

    bool initialize();
    bool available() const { return initialized_; }

private:
    std::string host_;
    int port_;
    std::string user_;
    std::string password_;
    std::string database_;
    size_t pool_size_;

    std::queue<ConnPtr> pool_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool initialized_ = false;
};

class MysqlGuard {
public:
    MysqlGuard(MysqlPool& pool, MysqlPool::ConnPtr& conn)
        : pool_(pool), conn_(conn) {}
    ~MysqlGuard() {
        if (conn_) pool_.release(conn_);
    }

private:
    MysqlPool& pool_;
    MysqlPool::ConnPtr& conn_;
};

}
