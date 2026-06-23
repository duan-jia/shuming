#include "server/http_protocol.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

namespace fastllm {

std::string HttpRequest::method_string() const {
    switch (method) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE_METHOD: return "DELETE";
        case HttpMethod::OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

std::string HttpRequest::header(const std::string& key, const std::string& def) const {
    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
    for (const auto& [k, v] : headers) {
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
        if (lk == lower_key) return v;
    }
    return def;
}

std::string HttpRequest::query(const std::string& key, const std::string& def) const {
    auto it = query_params.find(key);
    return it != query_params.end() ? it->second : def;
}

void HttpResponse::set_status(int code, const std::string& msg) {
    status_code = code;
    status_message = msg;
}

void HttpResponse::set_header(const std::string& key, const std::string& value) {
    headers[key] = value;
}

void HttpResponse::set_body(const std::string& b) {
    body = b;
}

void HttpResponse::json(const std::string& b) {
    headers["Content-Type"] = "application/json";
    body = b;
}

void HttpResponse::json_error(int status, const std::string& msg) {
    status_code = status;
    headers["Content-Type"] = "application/json";
    nlohmann::json j;
    j["error"] = msg;
    j["status"] = status;
    body = j.dump();
}

std::string HttpResponse::serialize() const {
    std::ostringstream oss;
    oss << (version == HttpVersion::HTTP11 ? "HTTP/1.1 " : "HTTP/1.0 ");
    oss << status_code << " " << status_message << "\r\n";

    bool has_content_length = false;
    bool has_content_type = false;
    for (const auto& [k, v] : headers) {
        oss << k << ": " << v << "\r\n";
        if (k == "Content-Length") has_content_length = true;
        if (k == "Content-Type") has_content_type = true;
    }

    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    oss << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";

    if (!has_content_type) {
        oss << "Content-Type: application/octet-stream\r\n";
    }
    if (!has_content_length) {
        oss << "Content-Length: " << body.size() << "\r\n";
    }
    if (close_connection) {
        oss << "Connection: close\r\n";
    }

    oss << "Server: fast-llm-reasoner\r\n";
    oss << "\r\n";
    oss << body;
    return oss.str();
}

void HttpParser::reset() {
    request_ = HttpRequest{};
    state_ = EXPECT_REQUEST_LINE;
    content_length_ = 0;
    consumed_ = 0;
    partial_line_.clear();
    chunked_ = false;
    chunk_remaining_ = 0;
    chunk_size_buf_.clear();
}

bool HttpParser::parse_request_line(const std::string& line) {
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    std::string method_str = line.substr(0, sp1);
    std::string uri = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string ver_str = line.substr(sp2 + 1);

    if (method_str == "GET") request_.method = HttpMethod::GET;
    else if (method_str == "POST") request_.method = HttpMethod::POST;
    else if (method_str == "PUT") request_.method = HttpMethod::PUT;
    else if (method_str == "DELETE") request_.method = HttpMethod::DELETE_METHOD;
    else if (method_str == "OPTIONS") request_.method = HttpMethod::OPTIONS;
    else request_.method = HttpMethod::UNKNOWN;

    size_t qpos = uri.find('?');
    if (qpos != std::string::npos) {
        request_.path = uri.substr(0, qpos);
        request_.query_string = uri.substr(qpos + 1);
    } else {
        request_.path = uri;
    }

    if (ver_str == "HTTP/1.1") request_.version = HttpVersion::HTTP11;
    else if (ver_str == "HTTP/1.0") request_.version = HttpVersion::HTTP10;

    return true;
}

bool HttpParser::parse_header_line(const std::string& line) {
    if (line.empty()) return true;
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;

    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value[0] == ' ') value.erase(0, 1);

    request_.headers[key] = value;

    std::string key_lower = key;
    std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(), ::tolower);

    if (key_lower == "content-length") {
        content_length_ = std::stoul(value);
    }
    if (key_lower == "transfer-encoding" && value.find("chunked") != std::string::npos) {
        chunked_ = true;
    }

    return true;
}

void HttpParser::parse_query_string() {
    if (request_.query_string.empty()) return;
    std::istringstream iss(request_.query_string);
    std::string pair;
    while (std::getline(iss, pair, '&')) {
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            request_.query_params[pair.substr(0, eq)] = pair.substr(eq + 1);
        } else {
            request_.query_params[pair] = "";
        }
    }
}

HttpParser::ParseState HttpParser::feed(const char* data, size_t len) {
    consumed_ = 0;
    size_t pos = 0;

    while (pos < len) {
        if (state_ == EXPECT_REQUEST_LINE || state_ == EXPECT_HEADERS) {
            const char* crlf = static_cast<const char*>(memmem(data + pos, len - pos, "\r\n", 2));
            if (!crlf) {
                partial_line_.append(data + pos, len - pos);
                consumed_ = len;
                return state_;
            }

            std::string line;
            if (!partial_line_.empty()) {
                line = partial_line_ + std::string(data + pos, crlf - (data + pos));
                partial_line_.clear();
            } else {
                line = std::string(data + pos, crlf - (data + pos));
            }

            size_t line_consumed = (crlf - (data + pos)) + 2;
            pos += line_consumed;
            consumed_ += line_consumed;

            if (state_ == EXPECT_REQUEST_LINE) {
                if (!parse_request_line(line)) {
                    state_ = ERROR;
                    return state_;
                }
                state_ = EXPECT_HEADERS;
            } else {
                if (line.empty()) {
                    parse_query_string();
                    if (content_length_ > 0) {
                        state_ = EXPECT_BODY;
                    } else if (chunked_) {
                        state_ = EXPECT_CHUNK_SIZE;
                    } else {
                        state_ = COMPLETE;
                        return state_;
                    }
                } else {
                    if (!parse_header_line(line)) {
                        state_ = ERROR;
                        return state_;
                    }
                }
            }
        } else if (state_ == EXPECT_BODY) {
            size_t need = content_length_ - request_.body.size();
            size_t available = len - pos;
            size_t to_read = std::min(need, available);
            request_.body.append(data + pos, to_read);
            pos += to_read;
            consumed_ += to_read;

            if (request_.body.size() >= content_length_) {
                state_ = COMPLETE;
                return state_;
            }
            return state_;
        } else if (state_ == EXPECT_CHUNK_SIZE) {
            for (; pos + 1 < len; ++pos) {
                chunk_size_buf_ += data[pos];
                if (data[pos] == '\r' && data[pos + 1] == '\n') {
                    pos += 2;
                    consumed_ = pos;
                    if (!chunk_size_buf_.empty() && chunk_size_buf_.back() == '\r') {
                        chunk_size_buf_.pop_back();
                    }
                    chunk_remaining_ = std::stoul(chunk_size_buf_, nullptr, 16);
                    chunk_size_buf_.clear();
            if (chunk_remaining_ == 0) {
                state_ = COMPLETE;
                return state_;
                    }
                    state_ = EXPECT_CHUNK_DATA;
                    break;
                }
            }
            if (state_ != EXPECT_CHUNK_DATA) {
                consumed_ = pos;
                return state_;
            }
        } else if (state_ == EXPECT_CHUNK_DATA) {
            size_t available = len - pos;
            size_t to_read = std::min(chunk_remaining_, available);
            request_.body.append(data + pos, to_read);
            pos += to_read;
            consumed_ += to_read;
            chunk_remaining_ -= to_read;
            if (chunk_remaining_ == 0) {
                state_ = EXPECT_CHUNK_CRLF;
            }
        } else if (state_ == EXPECT_CHUNK_CRLF) {
            if (pos + 1 < len && data[pos] == '\r' && data[pos + 1] == '\n') {
                pos += 2;
                consumed_ += 2;
                state_ = EXPECT_CHUNK_SIZE;
            } else if (pos < len) {
                consumed_ = len;
                return state_;
            }
        } else {
            break;
        }
    }

    return state_;
}

}
