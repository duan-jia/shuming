#pragma once

#include <memory>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include <muduo/net/TcpConnection.h>

namespace fastllm {

struct SseContext {
    std::function<void(const std::string& event_type, const std::string& data)> send_event;
    std::function<void()> close;
    std::function<bool()> is_active;
    std::string session_id;
    int user_id = 0;
    int tenant_id = 0;
    std::string role;
};

class SseConnection : public std::enable_shared_from_this<SseConnection> {
public:
    using Ptr = std::shared_ptr<SseConnection>;
    using CloseCallback = std::function<void(const std::string&)>;

    explicit SseConnection(const muduo::net::TcpConnectionPtr& conn);

    void send_event(const std::string& event_type, const std::string& data);
    void send_event(const std::string& data);
    void send_done();
    void close();

    void set_close_callback(CloseCallback cb) { close_cb_ = std::move(cb); }
    const std::string& id() const { return id_; }
    bool is_active() const { return active_; }

private:
    void do_send(const std::string& msg);

    std::string id_;
    muduo::net::TcpConnectionPtr conn_;
    CloseCallback close_cb_;
    std::atomic<bool> active_{true};
};

class SseConnectionManager {
public:
    SseConnection::Ptr create(const muduo::net::TcpConnectionPtr& conn);
    void remove(const std::string& id);
    SseConnection::Ptr get(const std::string& id);

private:
    std::unordered_map<std::string, SseConnection::Ptr> connections_;
    std::mutex mutex_;
};

}
