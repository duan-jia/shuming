#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <regex>
#include <memory>

#include "server/http_protocol.h"

namespace fastllm {

struct RouteParams {
    std::unordered_map<std::string, std::string> params;
    std::string get(const std::string& key, const std::string& def = "") const {
        auto it = params.find(key);
        return it != params.end() ? it->second : def;
    }
};

struct SseContext;

class HttpResponse;

using Handler = std::function<void(const HttpRequest&, RouteParams, HttpResponse&)>;

using SseHandler = std::function<void(const HttpRequest&, RouteParams, std::shared_ptr<SseContext>)>;

struct Route {
    std::string method;
    std::string pattern;
    std::regex compiled;
    std::vector<std::string> param_names;
    bool is_sse = false;
    Handler handler;
    SseHandler sse_handler;
};

class Router {
public:
    void get(const std::string& pattern, Handler handler);
    void post(const std::string& pattern, Handler handler);
    void del(const std::string& pattern, Handler handler);

    void sse(const std::string& pattern, SseHandler handler);

    bool dispatch(const HttpRequest& req, HttpResponse& resp);
    bool dispatch_sse(const HttpRequest& req, std::shared_ptr<SseContext> sse_ctx);

private:
    std::vector<Route> routes_;
    static std::pair<std::regex, std::vector<std::string>> compile_pattern(const std::string& pattern);
};

}
