#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <memory>
#include <vector>
#include <optional>
#include <mutex>

namespace fastllm {

class RedisConnection {
public:
    RedisConnection();
    ~RedisConnection();

    bool connect(const std::string& host, int port, int db = 0);
    void close();

    redisReply* command(const char* format, ...);
    bool set(const std::string& key, const std::string& value, int ttl_sec = 0);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool expire(const std::string& key, int ttl_sec);
    bool exists(const std::string& key);

    redisContext* raw() { return ctx_; }
    bool valid() const { return ctx_ != nullptr; }

private:
    redisContext* ctx_ = nullptr;
};

class RedisManager {
public:
    RedisManager(const std::string& host, int port, int db = 0);
    ~RedisManager();

    bool initialize();

    bool set(const std::string& key, const std::string& value, int ttl_sec = 0);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool expire(const std::string& key, int ttl_sec);
    bool exists(const std::string& key);

    redisReply* command(const char* format, ...);
    std::optional<long long> incr(const std::string& key);
    std::optional<long long> decr(const std::string& key);

    bool eval(const std::string& script,
              const std::vector<std::string>& keys,
              const std::vector<std::string>& args,
              long long& result);

private:
    std::string host_;
    int port_;
    int db_;
    std::unique_ptr<RedisConnection> conn_;
    std::mutex mutex_;
};

}
