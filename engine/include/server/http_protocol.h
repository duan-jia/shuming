#pragma once

#include <string>
#include <unordered_map>
#include <functional>

namespace fastllm {

enum class HttpMethod { GET, POST, PUT, DELETE_METHOD, OPTIONS, UNKNOWN };
enum class HttpVersion { HTTP10, HTTP11 };

struct HttpRequest {
    HttpMethod method = HttpMethod::UNKNOWN;
    HttpVersion version = HttpVersion::HTTP11;
    std::string path;
    std::string query_string;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> query_params;
    std::string body;
    // Transport peer, set by HttpServer; never read from client forwarding headers.
    std::string peer_ip;

    std::string method_string() const;
    std::string header(const std::string& key, const std::string& def = "") const;
    std::string query(const std::string& key, const std::string& def = "") const;
};

struct HttpResponse {
    int status_code = 200;
    std::string status_message = "OK";
    HttpVersion version = HttpVersion::HTTP11;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool close_connection = false;

    void set_status(int code, const std::string& msg);
    void set_header(const std::string& key, const std::string& value);
    void set_body(const std::string& b);
    void json(const std::string& b);
    void json_error(int status, const std::string& msg);

    std::string serialize() const;
};

class HttpParser {
public:
    enum ParseState { EXPECT_REQUEST_LINE, EXPECT_HEADERS, EXPECT_BODY,
                      EXPECT_CHUNK_SIZE, EXPECT_CHUNK_DATA, EXPECT_CHUNK_CRLF,
                      COMPLETE, ERROR };

    ParseState feed(const char* data, size_t len);
    ParseState state() const { return state_; }
    const HttpRequest& request() const { return request_; }
    bool complete() const { return state_ == COMPLETE; }
    size_t consumed() const { return consumed_; }

    void reset();

private:
    bool parse_request_line(const std::string& line);
    bool parse_header_line(const std::string& line);
    void parse_query_string();
    size_t find_crlf(const char* data, size_t pos, size_t len) const;

    HttpRequest request_;
    ParseState state_ = EXPECT_REQUEST_LINE;
    size_t content_length_ = 0;
    size_t consumed_ = 0;
    std::string partial_line_;
    bool chunked_ = false;
    size_t chunk_remaining_ = 0;
    std::string chunk_size_buf_;
};

}
