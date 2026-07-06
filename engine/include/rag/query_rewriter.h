#pragma once

#include <string>
#include <vector>

namespace fastllm {

struct QueryRewriteResult {
    std::string original_query;
    std::string search_query;
    std::vector<std::string> aliases;
    bool rewritten = false;
    std::string failure_reason;

    std::string combined_query() const;
};

class QueryRewriter {
public:
    QueryRewriter(bool enabled,
                  std::string endpoint,
                  std::string model,
                  std::string api_key,
                  int timeout_seconds = 10);

    QueryRewriteResult rewrite(const std::string& query) const;
    bool enabled() const { return enabled_; }

private:
    bool enabled_ = false;
    std::string endpoint_;
    std::string model_;
    std::string api_key_;
    int timeout_seconds_ = 10;
};

} // namespace fastllm
