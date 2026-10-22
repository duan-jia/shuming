#include "server/sse_connection.h"

#include <spdlog/spdlog.h>
#include <muduo/net/EventLoop.h>

namespace fastllm {

static std::atomic<uint64_t> sse_id_counter{0};

SseConnection::SseConnection(const muduo::net::TcpConnectionPtr& conn)
    : conn_(conn) {
    id_ = "sse-" + std::to_string(sse_id_counter.fetch_add(1));
    spdlog::debug("SSE connection created: {}", id_);
}

void SseConnection::send_event(const std::string& event_type, const std::string& data) {
    if (!active_) return;
    std::string msg = "event: " + event_type + "\ndata: " + data + "\n\n";
    do_send(msg);
}

void SseConnection::send_event(const std::string& data) {
    if (!active_) return;
    std::string msg = "data: " + data + "\n\n";
    do_send(msg);
}

void SseConnection::send_done() {
    if (!active_.exchange(false)) return;
    do_send("data: [DONE]\n\n");
    if (conn_) conn_->getLoop()->queueInLoop([conn = conn_] {
        if (conn->connected()) conn->shutdown();
    });
    if (close_cb_) close_cb_(id_);
}

void SseConnection::close() {
    if (!active_.exchange(false)) return;
    if (conn_) conn_->getLoop()->queueInLoop([conn = conn_] {
        if (conn->connected()) conn->shutdown();
    });
    if (close_cb_) close_cb_(id_);
}

void SseConnection::do_send(const std::string& msg) {
    if (conn_) conn_->getLoop()->queueInLoop([conn = conn_, msg] {
        if (conn->connected()) conn->send(msg);
    });
}

SseConnection::Ptr SseConnectionManager::create(const muduo::net::TcpConnectionPtr& conn) {
    auto sse = std::make_shared<SseConnection>(conn);
    std::lock_guard<std::mutex> lock(mutex_);
    connections_[sse->id()] = sse;
    return sse;
}

void SseConnectionManager::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(id);
}

SseConnection::Ptr SseConnectionManager::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(id);
    return it != connections_.end() ? it->second : nullptr;
}

}
