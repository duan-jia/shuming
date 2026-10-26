#include "rag/chunker.h"
#include "rag/bm25_index.h"
#include "rag/vector_index.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>

using namespace fastllm;

static void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    Chunker chunker;
    auto chunks = chunker.chunk(1, "# RDMA 的做法\n\nA general document title must remain intact.\n", "research");
    expect(!chunks.empty(), "expected chunk");
    expect(chunks.front().metadata.title == "RDMA 的做法", "title must not use recipe suffix rules");
    expect(chunks.front().metadata.category == "research", "category must be preserved");

    const auto path = std::filesystem::temp_directory_path() /
        ("shuming-metadata-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    try {
        // Historical BM25 state remains readable; subsequent saves use title.
        nlohmann::json old = nlohmann::json::array({{
            {"doc_id", 7}, {"chunk_index", 0}, {"tenant_id", 3}, {"visibility", "group"},
            {"content", "rdma"}, {"term_count", 1}, {"category", "research"},
            {"dish", "Historical title"}, {"difficulty", "ignored"}, {"term_freq", {{"r", 1}}}
        }});
        { std::ofstream stream(path); stream << old; }
        Bm25Index index;
        expect(index.load(path.string()), "legacy load failed");
        auto results = index.search("rdma", 1, 3);
        expect(results.size() == 1 && results[0].metadata.title == "Historical title", "legacy title lost");
        expect(index.save(path.string()), "save failed");
        nlohmann::json saved;
        { std::ifstream stream(path); stream >> saved; }
        expect(saved[0]["title"] == "Historical title", "new title missing");
        expect(!saved[0].contains("dish") && !saved[0].contains("difficulty"), "recipe keys still written");
        Bm25Index reloaded;
        expect(reloaded.load(path.string()), "new format load failed");
        auto again = reloaded.search("rdma", 1, 3);
        expect(again.size() == 1 && again[0].metadata.title == "Historical title", "round trip lost title");
        expect(reloaded.search("rdma", 1, 4).empty(), "tenant isolation changed");
        reloaded.synchronize_scopes({{7, {3, "public"}}});
        expect(!reloaded.search("rdma", 1, 4).empty(), "public migration did not update cached scope");
        reloaded.synchronize_scopes({{7, {5, "group"}}});
        expect(reloaded.search("rdma", 1, 4).empty(), "restricted scope leaked through old cache");
        expect(!reloaded.search("rdma", 1, 5).empty(), "new group lost access");
        reloaded.synchronize_scopes({});
        expect(reloaded.search("rdma", 1, 5).empty(), "deleted document remained in cached index");
    } catch (...) {
        std::filesystem::remove(path);
        throw;
    }
    std::filesystem::remove(path);
    std::cout << "RAG title metadata and legacy index compatibility passed\n";
}
