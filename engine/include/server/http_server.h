#pragma once

#include <string>
#include <memory>

#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>

#include "server/http_protocol.h"
#include "server/router.h"
#include "server/worker_pool.h"
#include "server/sse_connection.h"

namespace fastllm {

class HttpServer {
public:
    HttpServer(muduo::net::EventLoop* loop,
               const muduo::net::InetAddress& addr,
               const std::string& name,
               int num_threads = 1, int worker_threads = 4, int worker_queue_limit = 64);

    Router& router() { return router_; }
    SseConnectionManager& sse_manager() { return sse_mgr_; }

    void set_static_dir(const std::string& dir);
    void start();

private:
    void on_connection(const muduo::net::TcpConnectionPtr& conn);
    void on_message(const muduo::net::TcpConnectionPtr& conn,
                    muduo::net::Buffer* buf,
                    muduo::Timestamp time);

    void handle_request(const muduo::net::TcpConnectionPtr& conn,
                        const HttpRequest& req);

    void handle_sse_request(const muduo::net::TcpConnectionPtr& conn,
                            const HttpRequest& req);

    muduo::net::TcpServer server_;
    Router router_;
    SseConnectionManager sse_mgr_;
    WorkerPool workers_;
};

}
