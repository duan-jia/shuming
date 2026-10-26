#include "agent/source_files.h"
#include "services/knowledge_fetch_service.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace fastllm;
using Json = nlohmann::json;
void check(bool ok) { if (!ok) throw std::runtime_error("source files test failed"); }
int main() {
    char path[] = "/tmp/shuming-sources-XXXXXX";
    check(mkdtemp(path));
    const std::string storage = "12345678-1234-1234-1234-123456789abc.pdf";
    std::ofstream(std::string(path) + "/" + storage) << "%PDF-1.7\nfixture";
    KnowledgeFetchOptions options;
    options.pdf_directory = path;
    options.signing_secret = "test-only-secret";
    bool is_public = false;
    KnowledgeFetchService files([&](int id, int tenant, DocumentDetails& doc) {
        if (tenant != 7 || id < 1 || id > 3) return false;
        doc.visibility = is_public ? "public" : "group";
        doc.source_filename = "paper.pdf";
        doc.source_storage_name = id == 1 ? storage : id == 2 ? "22345678-1234-1234-1234-123456789abc.pdf" : "../outside.pdf";
        return true;
    }, options);
    CallerContext caller;
    caller.tenant_id = 7;
    const auto ref = files.download_reference(1, caller);
    check(ref == Json({{"doc_id", 1}, {"filename", "paper.pdf"}}));
    check(files.download_reference(2, caller).is_null());
    check(files.download_reference(3, caller).is_null());
    caller.public_only = true;
    check(files.download_reference(1, caller).is_null());
    is_public = true;
    check(files.download_reference(1, caller) == ref);
    caller.public_only = false;
    caller.tenant_id = 8;
    check(files.download_reference(1, caller).is_null());
    caller.tenant_id = 7;
    auto refs = Json::array();
    auto resolve = [&](int id) { return files.download_reference(id, caller); };
    collect_source_files(refs, "search_knowledge", R"({"results":[{"doc_id":1},{"doc_id":1},{"doc_id":2},{"doc_id":3},{"doc_id":-1},{"doc_id":"1"},{"doc_id":2147483648}]})", resolve);
    check(refs == Json::array({ref}));
    collect_source_files(refs, "fetch_knowledge", R"({"doc_id":1})", resolve);
    check(refs.size() == 1);
    refs = Json::array();
    for (auto content : {"broken", R"({"error":"timeout","doc_id":1})", R"({"error":"over budget"})", R"({"results":[]})"})
        collect_source_files(refs, "search_knowledge", content, resolve);
    collect_source_files(refs, "fetch_web", R"({"doc_id":1})", resolve);
    check(refs.empty());
    collect_source_files(refs, "fetch_knowledge", R"({"doc_id":1,"download_url":"https://evil.invalid"})", resolve);
    check(refs == Json::array({ref}));
    auto many = Json::array();
    for (int id = 1; id <= 50; ++id) many.push_back({{"doc_id", id}});
    refs = Json::array();
    collect_source_files(refs, "search_knowledge", Json({{"results", many}}).dump(),
        [](int id) { return Json{{"doc_id", id}, {"filename", "paper.pdf"}}; });
    check(refs.size() == 40);
    std::ofstream(std::string(path) + "/" + storage) << "not a pdf";
    check(files.download_reference(1, caller).is_null());
    std::filesystem::remove_all(path);
    std::cout << "source collection, deduplication, access, source availability and URL rejection passed\n";
}
