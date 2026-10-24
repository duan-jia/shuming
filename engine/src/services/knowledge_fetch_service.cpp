#include "services/knowledge_fetch_service.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <climits>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <poll.h>
#include <regex>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>

extern char** environ;

namespace fastllm {
namespace {
using Json = nlohmann::json;
struct File {
    int fd;
    ~File() { if (fd >= 0) close(fd); }
};
long long integer(const std::string& text) {
    if (text.empty() || text.size() > 18 || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return -1;
    try { return std::stoll(text); } catch (...) { return -1; }
}
std::string encoded_filename(const std::string& name) {
    std::ostringstream out;
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') out << c;
        else out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return out.str();
}
}

KnowledgeFetchService::KnowledgeFetchService(Lookup lookup, KnowledgeFetchOptions options)
    : lookup_(std::move(lookup)), options_(std::move(options)) {
    if (!lookup_ || options_.signing_secret.empty() || options_.link_ttl_seconds < 1
        || options_.link_ttl_seconds > 3600 || options_.timeout_seconds < 1 || options_.max_concurrency < 1)
        throw std::invalid_argument("Invalid knowledge fetch configuration");
    while (!options_.public_base_url.empty() && options_.public_base_url.back() == '/') options_.public_base_url.pop_back();
    if (!options_.public_base_url.empty() && options_.public_base_url.rfind("https://", 0) != 0
        && options_.public_base_url.rfind("http://127.0.0.1:", 0) != 0)
        throw std::invalid_argument("knowledge_fetch.public_base_url requires HTTPS");
}

Json KnowledgeFetchService::input_schema() {
    return {{"type", "object"}, {"additionalProperties", false}, {"required", {"doc_id"}},
        {"properties", {
            {"doc_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", INT_MAX}}},
            {"representation", {{"type", "string"}, {"enum", {"source", "text", "pages"}}, {"default", "text"}}},
            {"start_page", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100000}, {"default", 1}}},
            {"page_count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}, {"default", 3}}},
            {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000000}, {"default", 0},
                {"description", "Unicode character offset within the SAME selected page range; follow next_offset before next_page."}}}
        }}};
}

int KnowledgeFetchService::open_pdf(const DocumentDetails& document) const {
    static const std::regex storage(R"(^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}(--[^/\\\x00-\x1f]{1,160})?\.pdf$)");
    if (document.source_filename.empty() || !std::regex_match(document.source_storage_name, storage))
        throw std::runtime_error("Document has no source PDF");
    File directory{open(options_.pdf_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.fd < 0) throw std::runtime_error("PDF storage unavailable");
    File file{openat(directory.fd, document.source_storage_name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    struct stat info{};
    if (file.fd < 0 || fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_size < 5 || static_cast<size_t>(info.st_size) > options_.max_pdf_bytes)
        throw std::runtime_error("Source PDF unavailable or exceeds size limit");
    char magic[5];
    if (pread(file.fd, magic, 5, 0) != 5 || std::string(magic, 5) != "%PDF-")
        throw std::runtime_error("Invalid source PDF");
    const auto fd = fcntl(file.fd, F_DUPFD_CLOEXEC, 10);
    if (fd < 0) throw std::runtime_error("Cannot open source PDF");
    return fd;
}

std::string KnowledgeFetchService::signature(int id, int tenant, long long expiry, const std::string& storage) const {
    const auto payload = "shuming-pdf-v1\n" + std::to_string(id) + "\n" + std::to_string(tenant) + "\n" + std::to_string(expiry) + "\n" + storage;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), options_.signing_secret.data(), options_.signing_secret.size(),
              reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest, &length))
        throw std::runtime_error("Cannot sign download");
    std::ostringstream out;
    for (unsigned int i = 0; i < length; ++i) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return out.str();
}

Json KnowledgeFetchService::read_pdf(int fd, const Json& args, std::function<bool()> should_stop) const {
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) != 0) throw std::runtime_error("Cannot start PDF reader");
    File read_end{fcntl(pipes[0], F_DUPFD_CLOEXEC, 10)};
    File write_end{fcntl(pipes[1], F_DUPFD_CLOEXEC, 10)};
    close(pipes[0]); close(pipes[1]);
    if (read_end.fd < 0 || write_end.fd < 0) throw std::runtime_error("Cannot start PDF reader");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fd, 3);
    posix_spawn_file_actions_adddup2(&actions, write_end.fd, STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    // No shell interpolation. The authorized file is inherited as fd 3, so a
    // rename or symlink swap cannot redirect the reader to another local file.
    std::string payload = args.dump();
    char* argv[] = {const_cast<char*>(options_.python.c_str()), const_cast<char*>(options_.reader_script.c_str()), payload.data(), nullptr};
    pid_t pid;
    const auto error = posix_spawnp(&pid, options_.python.c_str(), &actions, &attributes, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (error) throw std::runtime_error("Cannot start PDF reader");
    struct Child {
        pid_t pid;
        ~Child() { if (pid > 0) { kill(-pid, SIGKILL); while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} } }
    } child{pid};
    close(write_end.fd); write_end.fd = -1;
    fcntl(read_end.fd, F_SETFL, O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options_.timeout_seconds);
    std::string output;
    bool eof = false;
    int status = 0;
    while (true) {
        if ((should_stop && should_stop()) || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("PDF reader cancelled or timed out");
        char bytes[8192];
        const auto count = read(read_end.fd, bytes, sizeof(bytes));
        if (count > 0) output.append(bytes, count);
        else if (count == 0) eof = true;
        else if (errno != EAGAIN && errno != EINTR) throw std::runtime_error("Cannot read PDF output");
        if (output.size() > 12 * 1024 * 1024) throw std::runtime_error("PDF result exceeds size limit");
        const auto finished = child.pid > 0 ? waitpid(pid, &status, WNOHANG) : 0;
        if (finished == pid) { child.pid = -1; if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw std::runtime_error("PDF processing failed"); }
        if (child.pid < 0 && eof) break;
        if (count <= 0) { pollfd pending{read_end.fd, POLLIN, 0}; poll(&pending, 1, 20); }
    }
    auto result = Json::parse(output, nullptr, false);
    if (!result.is_object()) throw std::runtime_error("Invalid PDF reader result");
    return result;
}

ToolResult KnowledgeFetchService::execute(const Json& args, const CallerContext& caller, std::function<bool()> should_stop) {
    if (!args.is_object() || !args.contains("doc_id") || !args["doc_id"].is_number_integer()
        || args["doc_id"] < 1 || args["doc_id"] > INT_MAX) throw std::invalid_argument("Valid doc_id required");
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "doc_id" && it.key() != "representation" && it.key() != "start_page" && it.key() != "page_count" && it.key() != "offset")
            throw std::invalid_argument("Unknown knowledge fetch argument");
    const auto schema = input_schema()["properties"];
    for (const auto* key : {"start_page", "page_count", "offset"})
        if (args.contains(key) && (!args[key].is_number_integer() || args[key] < schema[key]["minimum"] || args[key] > schema[key]["maximum"]))
            throw std::invalid_argument(std::string("Invalid ") + key);
    if (args.contains("representation") && (!args["representation"].is_string()
        || (args["representation"] != "source" && args["representation"] != "text" && args["representation"] != "pages")))
        throw std::invalid_argument("representation must be source, text or pages");
    if (caller.tenant_id <= 0) throw std::runtime_error("Valid tenant required");
    if (should_stop && should_stop()) throw std::runtime_error("Request cancelled");
    DocumentDetails document;
    const int id = args["doc_id"].get<int>();
    if (!lookup_(id, caller.tenant_id, document)) throw std::runtime_error("Document not found");
    File file{open_pdf(document)};
    const std::string representation = args.value("representation", "text");
    Json result = {{"doc_id", id}, {"filename", document.source_filename}, {"mime_type", "application/pdf"}, {"representation", representation}};
    if (representation == "source") {
        const auto expiry = std::time(nullptr) + options_.link_ttl_seconds;
        const auto path = "/api/knowledge/download?doc_id=" + std::to_string(id) + "&tenant_id=" + std::to_string(caller.tenant_id)
            + "&expires=" + std::to_string(expiry) + "&signature=" + signature(id, caller.tenant_id, expiry, document.source_storage_name);
        struct stat info{}; fstat(file.fd, &info);
        result["size_bytes"] = info.st_size;
        result["download_url"] = options_.public_base_url + path;
        result["expires_at"] = expiry;
        result["requires_auth"] = false;
        result["url_is_relative"] = options_.public_base_url.empty();
        return {std::move(result)};
    }
    const int previous = active_.fetch_add(1);
    struct Release { std::atomic<int>& active; ~Release() { --active; } } release{active_};
    if (previous >= options_.max_concurrency) throw std::runtime_error("PDF reader busy; retry later");
    auto body = read_pdf(file.fd, args, should_stop);
    Json attachments = body.value("images", Json::array());
    body.erase("images");
    result.update(body);
    return {std::move(result), std::move(attachments)};
}

void KnowledgeFetchService::download(const HttpRequest& request, HttpResponse& response) {
    response.set_header("Cache-Control", "private, no-store");
    response.set_header("Referrer-Policy", "no-referrer");
    if (request.method != HttpMethod::GET) { response.json_error(405, "GET required"); return; }
    const auto id = integer(request.query("doc_id"));
    const auto tenant = integer(request.query("tenant_id"));
    const auto expiry = integer(request.query("expires"));
    const auto supplied = request.query("signature");
    const auto now = std::time(nullptr);
    if (id < 1 || id > INT_MAX || tenant < 1 || tenant > INT_MAX || expiry < now || expiry > now + options_.link_ttl_seconds || supplied.size() != 64) {
        response.json_error(403, "Invalid or expired download link"); return;
    }
    DocumentDetails document;
    if (!lookup_(id, tenant, document)) { response.json_error(403, "Invalid or expired download link"); return; }
    const auto expected = signature(id, tenant, expiry, document.source_storage_name);
    if (CRYPTO_memcmp(supplied.data(), expected.data(), expected.size()) != 0) {
        response.json_error(403, "Invalid or expired download link"); return;
    }
    const int previous = active_.fetch_add(1);
    struct Release { std::atomic<int>& active; ~Release() { --active; } } release{active_};
    if (previous >= options_.max_concurrency) {
        response.json_error(503, "PDF service busy; retry later");
        response.set_header("Retry-After", "1");
        return;
    }
    try {
        File file{open_pdf(document)};
        std::string bytes;
        char buffer[65536];
        ssize_t count;
        while ((count = read(file.fd, buffer, sizeof(buffer))) > 0) {
            if (bytes.size() + count > options_.max_pdf_bytes) throw std::runtime_error("PDF too large");
            bytes.append(buffer, count);
        }
        if (count < 0) throw std::runtime_error("Cannot read PDF");
        response.set_header("Content-Type", "application/pdf");
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_header("Content-Disposition", "attachment; filename=\"document.pdf\"; filename*=UTF-8''" + encoded_filename(document.source_filename));
        response.set_body(bytes);
    } catch (const std::exception&) { response.json_error(404, "Source PDF unavailable"); }
}

} // namespace fastllm
