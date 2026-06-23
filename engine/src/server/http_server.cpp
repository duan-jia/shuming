#include "server/http_server.h"

#include <spdlog/spdlog.h>
#include <boost/any.hpp>
#include <muduo/net/Buffer.h>
#include <fstream>
#include <sstream>

namespace {

std::string g_static_dir;

std::string read_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "";
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::string content_type_for(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size()-5) == ".html") return "text/html; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".css") return "text/css; charset=utf-8";
    if (path.size() >= 3 && path.substr(path.size()-3) == ".js") return "application/javascript; charset=utf-8";
    if (path.size() >= 5 && path.substr(path.size()-5) == ".json") return "application/json; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".png") return "image/png";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".ico") return "image/x-icon";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

}

namespace fastllm {

void HttpServer::set_static_dir(const std::string& dir) { g_static_dir = dir; }

HttpServer::HttpServer(muduo::net::EventLoop* loop,
                       const muduo::net::InetAddress& addr,
                       const std::string& name,
                       int num_threads)
    : server_(loop, addr, name) {
    server_.setConnectionCallback(
        [this](const muduo::net::TcpConnectionPtr& conn) {
            on_connection(conn);
        });
    server_.setMessageCallback(
        [this](const muduo::net::TcpConnectionPtr& conn,
               muduo::net::Buffer* buf,
               muduo::Timestamp time) {
            on_message(conn, buf, time);
        });
    server_.setThreadNum(num_threads);
}

void HttpServer::start() {
    spdlog::info("HTTP server starting");
    server_.start();
}

void HttpServer::on_connection(const muduo::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        spdlog::debug("Connection from {}", conn->peerAddress().toIpPort());
        conn->setContext(std::make_shared<HttpParser>());
    } else {
        spdlog::debug("Connection closed: {}", conn->peerAddress().toIpPort());
    }
}

void HttpServer::on_message(const muduo::net::TcpConnectionPtr& conn,
                             muduo::net::Buffer* buf,
                             muduo::Timestamp) {
    boost::any* ctx = conn->getMutableContext();
    auto* parser_ptr = boost::any_cast<std::shared_ptr<HttpParser>>(ctx);
    if (!parser_ptr || !*parser_ptr) return;
    auto& parser = *parser_ptr;

    auto state = parser->feed(buf->peek(), buf->readableBytes());
    size_t to_consume = std::min(parser->consumed(), buf->readableBytes());
    buf->retrieve(to_consume);

    if (state == HttpParser::COMPLETE) {
        handle_request(conn, parser->request());
        parser->reset();
    } else if (state == HttpParser::ERROR) {
        HttpResponse resp;
        resp.json_error(400, "Bad Request");
        conn->send(resp.serialize());
        conn->shutdown();
    }
}

void HttpServer::handle_request(const muduo::net::TcpConnectionPtr& conn,
                                 const HttpRequest& req) {
    spdlog::info("{} {} from {}", req.method_string(), req.path,
                 conn->peerAddress().toIpPort());

    auto accept = req.header("Accept", "");
    auto content_type = req.header("Content-Type", "");
    bool want_sse = (accept.find("text/event-stream") != std::string::npos) ||
                    (content_type.find("text/event-stream") != std::string::npos) ||
                    (req.path.find("/stream") != std::string::npos);

    if (want_sse) {
        handle_sse_request(conn, req);
        return;
    }

    HttpResponse resp;
    resp.close_connection = (req.version == HttpVersion::HTTP10 &&
                             req.header("Connection", "") != "keep-alive");

    if (req.method == HttpMethod::OPTIONS) {
        resp.set_status(204, "No Content");
        resp.set_header("Access-Control-Max-Age", "86400");
        conn->send(resp.serialize());
        return;
    }

    if (req.method == HttpMethod::GET && g_static_dir.size() > 0 &&
        req.path.substr(0, 5) != "/api/") {
        std::string file_path = req.path;
        if (file_path == "/") file_path = "/index.html";
        std::string full_path = g_static_dir + file_path;
        std::string content = read_file(full_path);
        if (!content.empty()) {
            resp.set_status(200, "OK");
            resp.set_header("Content-Type", content_type_for(file_path));
            resp.set_body(content);
            conn->send(resp.serialize());
            if (resp.close_connection) conn->shutdown();
            return;
        }
    }

    router_.dispatch(req, resp);

    conn->send(resp.serialize());

    if (resp.close_connection) {
        conn->shutdown();
    }
}

void HttpServer::handle_sse_request(const muduo::net::TcpConnectionPtr& conn,
                                     const HttpRequest& req) {
    spdlog::info("SSE connection from {}", conn->peerAddress().toIpPort());

    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/event-stream\r\n"
                         "Cache-Control: no-cache\r\n"
                         "Connection: keep-alive\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Server: fast-llm-reasoner\r\n"
                         "\r\n";
    conn->send(header);

    auto sse = sse_mgr_.create(conn);
    auto sse_ctx = std::make_shared<SseContext>();
    sse_ctx->send_event = [sse](const std::string& event_type, const std::string& data) {
        sse->send_event(event_type, data);
    };
    sse_ctx->close = [sse]() {
        sse->send_done();
    };
    sse_ctx->is_active = [sse]() {
        return sse->is_active();
    };

    sse->set_close_callback([this](const std::string& id) {
        sse_mgr_.remove(id);
    });

    if (!router_.dispatch_sse(req, sse_ctx)) {
        sse->send_event("error", R"({"error":"No SSE handler"})");
        sse->send_done();
    }
}

}
