#pragma once

#include <atomic>
#include <functional>
#include <string>
#include "persistence/document_repository.h"
#include "services/knowledge_search_types.h"
#include "services/tool_result.h"
#include "server/http_protocol.h"

namespace fastllm {

struct KnowledgeFetchOptions {
    std::string pdf_directory = "data/pdfs";
    std::string reader_script = "scripts/read_pdf.py";
    std::string python = "python3";
    std::string public_base_url;
    std::string signing_secret;
    int link_ttl_seconds = 300;
    int timeout_seconds = 20;
    int max_concurrency = 2;
    size_t max_pdf_bytes = 50 * 1024 * 1024;
};

class KnowledgeFetchService {
public:
    // Lookup always applies the same visibility rules as the document detail API.
    using Lookup = std::function<bool(int, int, DocumentDetails&)>;
    KnowledgeFetchService(Lookup lookup, KnowledgeFetchOptions options);
    static nlohmann::json input_schema();
    ToolResult execute(const nlohmann::json& args, const CallerContext& caller,
                       std::function<bool()> should_stop = nullptr);
    // Stable reference only: no signed URL or storage path. Null if unavailable.
    nlohmann::json download_reference(int id, const CallerContext& caller);
    void download(const HttpRequest& request, HttpResponse& response);

private:
    Lookup lookup_;
    KnowledgeFetchOptions options_;
    std::atomic<int> active_{0};
    int open_pdf(const DocumentDetails& document) const;
    std::string signature(int id, int tenant, long long expiry, const std::string& storage) const;
    nlohmann::json read_pdf(int fd, const nlohmann::json& args, std::function<bool()> should_stop) const;
};

} // namespace fastllm
