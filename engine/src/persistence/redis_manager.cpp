#include "persistence/redis_manager.h"

#include <spdlog/spdlog.h>
#include <cstdarg>

namespace fastllm {

RedisConnection::RedisConnection() = default;

RedisConnection::~RedisConnection() {
    close();
}

bool RedisConnection::connect(const std::string& host, int port, int db) {
    close();

    struct timeval timeout = {1, 500000};
    ctx_ = redisConnectWithTimeout(host.c_str(), port, timeout);
    if (!ctx_ || ctx_->err) {
        spdlog::error("Redis connect failed: {}", ctx_ ? ctx_->errstr : "unknown error");
        if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
        return false;
    }

    if (db > 0) {
        auto reply = static_cast<redisReply*>(redisCommand(ctx_, "SELECT %d", db));
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            spdlog::error("Redis SELECT %d failed", db);
            freeReplyObject(reply);
            redisFree(ctx_);
            ctx_ = nullptr;
            return false;
        }
        freeReplyObject(reply);
    }

    spdlog::info("Redis connected to {}:{}", host, port);
    return true;
}

void RedisConnection::close() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

redisReply* RedisConnection::command(const char* format, ...) {
    if (!ctx_) return nullptr;

    va_list ap;
    va_start(ap, format);
    auto reply = static_cast<redisReply*>(redisvCommand(ctx_, format, ap));
    va_end(ap);
    return reply;
}

bool RedisConnection::set(const std::string& key, const std::string& value, int ttl_sec) {
    redisReply* reply;
    if (ttl_sec > 0) {
        reply = static_cast<redisReply*>(redisCommand(ctx_, "SET %s %s EX %d", key.c_str(), value.c_str(), ttl_sec));
    } else {
        reply = static_cast<redisReply*>(redisCommand(ctx_, "SET %s %s", key.c_str(), value.c_str()));
    }

    bool ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply) freeReplyObject(reply);
    return ok;
}

std::optional<std::string> RedisConnection::get(const std::string& key) {
    auto reply = static_cast<redisReply*>(redisCommand(ctx_, "GET %s", key.c_str()));
    if (!reply) return std::nullopt;

    std::optional<std::string> result;
    if (reply->type == REDIS_REPLY_STRING) {
        result = std::string(reply->str, reply->len);
    }
    freeReplyObject(reply);
    return result;
}

bool RedisConnection::del(const std::string& key) {
    auto reply = static_cast<redisReply*>(redisCommand(ctx_, "DEL %s", key.c_str()));
    bool ok = reply && reply->type == REDIS_REPLY_INTEGER;
    if (reply) freeReplyObject(reply);
    return ok;
}

bool RedisConnection::expire(const std::string& key, int ttl_sec) {
    auto reply = static_cast<redisReply*>(redisCommand(ctx_, "EXPIRE %s %d", key.c_str(), ttl_sec));
    bool ok = reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
    if (reply) freeReplyObject(reply);
    return ok;
}

bool RedisConnection::exists(const std::string& key) {
    auto reply = static_cast<redisReply*>(redisCommand(ctx_, "EXISTS %s", key.c_str()));
    bool ok = reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
    if (reply) freeReplyObject(reply);
    return ok;
}

RedisManager::RedisManager(const std::string& host, int port, int db)
    : host_(host), port_(port), db_(db) {}

RedisManager::~RedisManager() = default;

bool RedisManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    conn_ = std::make_unique<RedisConnection>();
    return conn_->connect(host_, port_, db_);
}

bool RedisManager::set(const std::string& key, const std::string& value, int ttl_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    return conn_ ? conn_->set(key, value, ttl_sec) : false;
}

std::optional<std::string> RedisManager::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return conn_ ? conn_->get(key) : std::nullopt;
}

bool RedisManager::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return conn_ ? conn_->del(key) : false;
}

bool RedisManager::expire(const std::string& key, int ttl_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    return conn_ ? conn_->expire(key, ttl_sec) : false;
}

bool RedisManager::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return conn_ ? conn_->exists(key) : false;
}

redisReply* RedisManager::command(const char* format, ...) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || !conn_->valid()) return nullptr;

    va_list ap;
    va_start(ap, format);
    auto reply = static_cast<redisReply*>(redisvCommand(conn_->raw(), format, ap));
    va_end(ap);
    return reply;
}

std::optional<long long> RedisManager::incr(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || !conn_->valid()) return std::nullopt;

    auto reply = static_cast<redisReply*>(redisCommand(conn_->raw(), "INCR %s", key.c_str()));
    if (!reply || reply->type != REDIS_REPLY_INTEGER) {
        if (reply) freeReplyObject(reply);
        return std::nullopt;
    }
    long long val = reply->integer;
    freeReplyObject(reply);
    return val;
}

std::optional<long long> RedisManager::decr(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || !conn_->valid()) return std::nullopt;

    auto reply = static_cast<redisReply*>(redisCommand(conn_->raw(), "DECR %s", key.c_str()));
    if (!reply || reply->type != REDIS_REPLY_INTEGER) {
        if (reply) freeReplyObject(reply);
        return std::nullopt;
    }
    long long val = reply->integer;
    freeReplyObject(reply);
    return val;
}

bool RedisManager::eval(const std::string& script,
                         const std::vector<std::string>& keys,
                         const std::vector<std::string>& args,
                         long long& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!conn_ || !conn_->valid()) return false;

    std::vector<const char*> argv;
    std::vector<size_t> argvl;

    std::string cmd = "EVALSHA";
    argv.push_back(cmd.c_str()); argvl.push_back(cmd.size());
    argv.push_back(script.c_str()); argvl.push_back(script.size());

    std::string numkeys = std::to_string(keys.size());
    argv.push_back(numkeys.c_str()); argvl.push_back(numkeys.size());

    for (const auto& k : keys) {
        argv.push_back(k.c_str()); argvl.push_back(k.size());
    }
    for (const auto& a : args) {
        argv.push_back(a.c_str()); argvl.push_back(a.size());
    }

    auto reply = static_cast<redisReply*>(redisCommandArgv(conn_->raw(), argv.size(), argv.data(), argvl.data()));
    if (!reply) return false;

    if (reply->type == REDIS_REPLY_ERROR) {
        // Fall back to EVAL if EVALSHA fails (script not cached)
        if (strstr(reply->str, "NOSCRIPT") != nullptr) {
            freeReplyObject(reply);
            cmd = "EVAL";
            argv[0] = cmd.c_str(); argvl[0] = cmd.size();
            reply = static_cast<redisReply*>(redisCommandArgv(conn_->raw(), argv.size(), argv.data(), argvl.data()));
            if (!reply) return false;
        }
    }

    if (reply->type == REDIS_REPLY_INTEGER) {
        result = reply->integer;
        freeReplyObject(reply);
        return true;
    }

    freeReplyObject(reply);
    return false;
}

}
