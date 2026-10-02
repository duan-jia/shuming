#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "services/knowledge_search_service.h"
#include "services/rag_prompt_formatter.h"

namespace {

using namespace fastllm;

class FakeBackend : public KnowledgeSearchBackend {
public:
    bool ready = true;
    int last_tenant_id = -1;
    int last_top_k = 0;
    KnowledgeSearchMode last_mode = KnowledgeSearchMode::CHUNKS;
    QueryRewriteInfo rewrite;
    std::vector<Evidence> chunks;
    std::vector<Evidence> parents;

    bool is_ready() const override { return ready; }

    QueryRewriteInfo rewrite_query(const std::string& query) const override {
        QueryRewriteInfo result = rewrite;
        if (result.original_query.empty()) result.original_query = query;
        if (result.search_query.empty()) result.search_query = query;
        return result;
    }

    std::vector<Evidence> retrieve_chunks(
        const QueryRewriteInfo&, int top_k, int tenant_id) override {
        last_tenant_id = tenant_id;
        last_top_k = top_k;
        last_mode = KnowledgeSearchMode::CHUNKS;
        return chunks;
    }

    std::vector<Evidence> retrieve_parents(
        const QueryRewriteInfo&, int top_k, int tenant_id) override {
        last_tenant_id = tenant_id;
        last_top_k = top_k;
        last_mode = KnowledgeSearchMode::PARENTS;
        return parents;
    }
};

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

template <typename Fn>
void expect_error(Fn fn, KnowledgeSearchErrorCode code, const std::string& name) {
    try {
        fn();
    } catch (const KnowledgeSearchError& e) {
        expect(e.code() == code, name + " returned wrong error code");
        return;
    }
    fail(name + " did not throw");
}

CallerContext caller_for(int tenant_id) {
    CallerContext caller;
    caller.tenant_id = tenant_id;
    caller.principal_id = 42;
    caller.principal_type = "user";
    caller.trace_id = "trace-1";
    return caller;
}

void test_chunk_search_uses_caller_tenant() {
    FakeBackend backend;
    backend.rewrite.original_query = "original";
    backend.rewrite.search_query = "rewritten";
    backend.rewrite.rewritten = true;
    backend.rewrite.aliases = {"alias"};

    Evidence item;
    item.mode = KnowledgeSearchMode::CHUNKS;
    item.document_id = 7;
    item.filename = "guide.md";
    item.content = "content";
    item.score = 0.25f;
    item.chunk_index = 3;
    backend.chunks.push_back(item);

    KnowledgeSearchService service(backend);
    SearchKnowledgeRequest request;
    request.query = "original";
    request.top_k = 4;
    request.mode = KnowledgeSearchMode::CHUNKS;
    auto result = service.execute(request, caller_for(19));

    expect(backend.last_tenant_id == 19, "tenant must come from caller context");
    expect(backend.last_top_k == 4, "top_k was not forwarded");
    expect(backend.last_mode == KnowledgeSearchMode::CHUNKS, "chunk mode not used");
    expect(result.evidence.size() == 1, "chunk evidence missing");
    expect(result.evidence[0].document_id == 7, "chunk evidence changed");
    expect(result.rewrite.search_query == "rewritten", "rewrite result changed");
}

void test_parent_search() {
    FakeBackend backend;
    Evidence item;
    item.mode = KnowledgeSearchMode::PARENTS;
    item.document_id = 8;
    item.content = "parent content";
    item.score = 0.5f;
    item.matched_chunks = 2;
    backend.parents.push_back(item);

    KnowledgeSearchService service(backend);
    SearchKnowledgeRequest request;
    request.query = "question";
    request.mode = KnowledgeSearchMode::PARENTS;
    auto result = service.execute(request, caller_for(3));

    expect(backend.last_mode == KnowledgeSearchMode::PARENTS, "parent mode not used");
    expect(result.evidence[0].matched_chunks == 2, "parent evidence changed");
}

void test_validation_and_unavailable() {
    FakeBackend backend;
    KnowledgeSearchService service(backend);
    auto caller = caller_for(1);

    SearchKnowledgeRequest request;
    request.query = "   ";
    expect_error([&] { service.execute(request, caller); },
        KnowledgeSearchErrorCode::INVALID_ARGUMENT, "blank query");

    request.query = "query";
    request.top_k = 0;
    expect_error([&] { service.execute(request, caller); },
        KnowledgeSearchErrorCode::INVALID_ARGUMENT, "zero top_k");

    request.top_k = 101;
    expect_error([&] { service.execute(request, caller); },
        KnowledgeSearchErrorCode::INVALID_ARGUMENT, "large top_k");

    request.top_k = 5;
    expect_error([&] { service.execute(request, caller_for(-1)); },
        KnowledgeSearchErrorCode::INVALID_ARGUMENT, "invalid tenant");
    expect_error([&] { service.execute(request, caller_for(0)); },
        KnowledgeSearchErrorCode::INVALID_ARGUMENT, "zero tenant");

    backend.ready = false;
    expect_error([&] { service.execute(request, caller); },
        KnowledgeSearchErrorCode::UNAVAILABLE, "unavailable backend");

    expect(parse_knowledge_search_mode("chunks") == KnowledgeSearchMode::CHUNKS,
        "chunks mode parse failed");
    expect(parse_knowledge_search_mode("parents") == KnowledgeSearchMode::PARENTS,
        "parents mode parse failed");
    expect_error([&] { parse_knowledge_search_mode("documents"); },
        KnowledgeSearchErrorCode::INVALID_ARGUMENT, "invalid mode");
}

void test_prompt_formatting() {
    SearchKnowledgeResult empty;
    empty.query = "question";
    expect(RagPromptFormatter::format(empty, "base prompt") == "base prompt",
        "empty evidence should use base prompt");

    SearchKnowledgeResult result;
    result.query = "question";
    result.rewrite.original_query = "question";
    result.rewrite.search_query = "search terms";
    result.rewrite.aliases = {"synonym"};
    result.rewrite.rewritten = true;

    Evidence chunk;
    chunk.mode = KnowledgeSearchMode::CHUNKS;
    chunk.document_id = 9;
    chunk.filename = "notes.md";
    chunk.content = "useful text";
    chunk.chunk_index = 4;
    chunk.score = 0.125f;
    result.evidence.push_back(chunk);

    auto prompt = RagPromptFormatter::format(result, "base prompt");
    expect(prompt.find("notes.md (chunk 4, relevance: 0.125)") != std::string::npos,
        "chunk prompt metadata missing");
    expect(prompt.find("search terms\nAliases: synonym\nOriginal: question")
        != std::string::npos, "combined rewrite query missing");

    result.evidence.clear();
    Evidence parent;
    parent.mode = KnowledgeSearchMode::PARENTS;
    parent.document_id = 10;
    parent.content = "parent text";
    parent.matched_chunks = 3;
    parent.score = 0.75f;
    result.evidence.push_back(parent);
    prompt = RagPromptFormatter::format(result, "base prompt");
    expect(prompt.find("document #10 (3 matching sections, best score: 0.750)")
        != std::string::npos, "parent prompt metadata missing");
}

} // namespace

int main() {
    test_chunk_search_uses_caller_tenant();
    test_parent_search();
    test_validation_and_unavailable();
    test_prompt_formatting();
    std::cout << "knowledge search service tests passed\n";
    return 0;
}
