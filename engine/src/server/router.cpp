#include "server/router.h"

#include <spdlog/spdlog.h>

namespace fastllm {

void Router::get(const std::string& pattern, Handler handler) {
    auto [regex, names] = compile_pattern(pattern);
    routes_.push_back({"GET", pattern, regex, names, false, std::move(handler), nullptr});
}

void Router::post(const std::string& pattern, Handler handler) {
    auto [regex, names] = compile_pattern(pattern);
    routes_.push_back({"POST", pattern, regex, names, false, std::move(handler), nullptr});
}

void Router::del(const std::string& pattern, Handler handler) {
    auto [regex, names] = compile_pattern(pattern);
    routes_.push_back({"DELETE", pattern, regex, names, false, std::move(handler), nullptr});
}

void Router::sse(const std::string& pattern, SseHandler handler) {
    auto [regex, names] = compile_pattern(pattern);
    routes_.push_back({"POST", pattern, regex, names, true, nullptr, std::move(handler)});
}

void Router::worker_endpoint(const std::string& path, Handler handler) {
    worker_endpoints_.emplace(path, std::move(handler));
}

bool Router::requires_worker(const HttpRequest& req) const {
    return worker_endpoints_.find(req.path) != worker_endpoints_.end();
}

bool Router::requires_sse(const HttpRequest& req) const {
    for (const auto& route : routes_) {
        if (route.is_sse && route.method == req.method_string()
            && std::regex_match(req.path, route.compiled)) return true;
    }
    return false;
}

bool Router::dispatch(const HttpRequest& req, HttpResponse& resp) {
    auto endpoint = worker_endpoints_.find(req.path);
    if (endpoint != worker_endpoints_.end()) {
        endpoint->second(req, {}, resp);
        return true;
    }
    std::string method = req.method_string();

    for (auto& route : routes_) {
        if (route.is_sse) continue;
        if (route.method != method) continue;

        std::smatch match;
        if (!std::regex_match(req.path, match, route.compiled)) continue;

        RouteParams params;
        for (size_t i = 0; i < route.param_names.size() && i + 1 < match.size(); ++i) {
            params.params[route.param_names[i]] = match[i + 1].str();
        }

        spdlog::debug("Route matched: {} {} -> {}", method, req.path, route.pattern);
        route.handler(req, params, resp);
        return true;
    }

    resp.json_error(404, "Not Found: " + method + " " + req.path);
    return false;
}

bool Router::dispatch_sse(const HttpRequest& req, std::shared_ptr<SseContext> sse_ctx) {
    std::string method = req.method_string();

    for (auto& route : routes_) {
        if (!route.is_sse) continue;
        if (route.method != method) continue;

        std::smatch match;
        if (!std::regex_match(req.path, match, route.compiled)) continue;

        RouteParams params;
        for (size_t i = 0; i < route.param_names.size() && i + 1 < match.size(); ++i) {
            params.params[route.param_names[i]] = match[i + 1].str();
        }

        spdlog::debug("SSE route matched: {} {} -> {}", method, req.path, route.pattern);
        route.sse_handler(req, params, sse_ctx);
        return true;
    }

    return false;
}

std::pair<std::regex, std::vector<std::string>> Router::compile_pattern(const std::string& pattern) {
    std::string regex_str = "^";
    std::vector<std::string> param_names;

    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == ':') {
            regex_str += "([^/]+)";
            std::string name;
            ++i;
            while (i < pattern.size() && pattern[i] != '/') {
                name += pattern[i++];
            }
            param_names.push_back(name);
            if (i < pattern.size()) --i;
        } else {
            regex_str += pattern[i];
        }
    }
    regex_str += "$";

    return {std::regex(regex_str), param_names};
}

}
