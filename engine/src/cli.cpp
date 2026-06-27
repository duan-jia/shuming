#include <iostream>
#include <csignal>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <numeric>

#include <spdlog/spdlog.h>

#include "inference/llm_engine.h"
#include "inference/model_manager.h"
#include "rag/document_store.h"
#include "rag/embedder.h"

using namespace fastllm;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_interrupt{false};

static void sigint_handler(int) {
    g_interrupt = true;
}

static std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return "";
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static void load_dir(RagService& rag, const std::string& dir) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".txt" && path.extension() != ".md") continue;
        auto content = read_file(path.string());
        if (content.empty()) continue;
        auto rel = std::filesystem::relative(path, dir);
        auto it = rel.begin();
        std::string category = (it != rel.end()) ? it->string() : "general";
        int doc_id = rag.add_document(1, path.filename().string(), content, category, "group", 0, false);
        if (doc_id >= 0)
            std::cout << "  loaded: " << category << "/" << path.filename() << " (id=" << doc_id << ")" << std::endl;
    }
    rag.save_all();
}

static void search_and_print(RagService& rag, const std::string& query, int top_k = 3) {
    if (!rag.is_ready()) {
        std::cout << "(RAG not available)" << std::endl;
        return;
    }
    auto results = rag.retrieve(query, top_k, 1);
    if (results.empty()) {
        std::cout << "(no matching documents)" << std::endl;
        return;
    }
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        std::cout << "  [" << (i+1) << "] " << r.metadata.category << "/"
                  << r.metadata.dish << " " << r.metadata.difficulty
                  << " score=" << r.score << " doc=" << r.doc_id << "\n  "
                  << r.content.substr(0, 120) << (r.content.size() > 120 ? "..." : "")
                  << std::endl;
    }
}

static void vec_and_print(RagService& rag, const std::string& query, int top_k = 3) {
    if (!rag.is_ready()) {
        std::cout << "(RAG not available)" << std::endl;
        return;
    }
    auto results = rag.vector_only_search(query, top_k, 1);
    if (results.empty()) {
        std::cout << "(no matching documents)" << std::endl;
        return;
    }
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        std::cout << "  [" << (i+1) << "] " << r.metadata.category << "/"
                  << r.metadata.dish << " " << r.metadata.difficulty
                  << " score=" << r.score << " doc=" << r.doc_id << std::endl;
    }
}

static void bm5_and_print(RagService& rag, const std::string& query, int top_k = 3) {
    auto results = rag.bm25_only_search(query, top_k);
    if (results.empty()) {
        std::cout << "(no matching documents)" << std::endl;
        return;
    }
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        std::cout << "  [" << (i+1) << "] " << r.metadata.category << "/"
                  << r.metadata.dish << " " << r.metadata.difficulty
                  << " score=" << r.score << " doc=" << r.doc_id << std::endl;
    }
}

static void parents_and_print(RagService& rag, const std::string& query, int top_k = 3) {
    auto results = rag.retrieve_parents(query, top_k, 1);
    if (results.empty()) {
        std::cout << "(no matching documents)" << std::endl;
        return;
    }
    for (size_t i = 0; i < results.size(); ++i) {
        auto& p = results[i];
        std::cout << "  [" << (i+1) << "] " << p.metadata.category << "/"
                  << p.metadata.dish << " " << p.metadata.difficulty
                  << " matches=" << p.matched_chunks << " score=" << p.max_score
                  << " doc=" << p.doc_id << " " << p.filename << std::endl;
    }
}

static void bench_and_print(RagService& rag) {
    if (!rag.is_ready()) {
        std::cout << "(RAG not available)" << std::endl;
        return;
    }

    const int warmup = 3, iters = 50;
    const std::string query = "排骨汤怎么做";
    std::vector<double> vec_us, bm5_us, hybrid_us;

    auto us = [](auto start, auto end) {
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    };

    int total = warmup + iters;
    std::cout << "Running " << total << " iterations (warmup=" << warmup << ")..." << std::flush;
    for (int i = 0; i < total; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        rag.vector_only_search(query, 3, 1);
        auto t1 = std::chrono::steady_clock::now();
        if (i >= warmup) vec_us.push_back(us(t0, t1));

        t0 = std::chrono::steady_clock::now();
        rag.bm25_only_search(query, 3);
        auto t2 = std::chrono::steady_clock::now();
        if (i >= warmup) bm5_us.push_back(us(t0, t2));

        t0 = std::chrono::steady_clock::now();
        rag.retrieve(query, 3, 1);
        auto t3 = std::chrono::steady_clock::now();
        if (i >= warmup) hybrid_us.push_back(us(t0, t3));
    }
    std::cout << " done" << std::endl;

    auto stats = [](std::vector<double>& s, const char* label) {
        std::sort(s.begin(), s.end());
        double sum = std::accumulate(s.begin(), s.end(), 0.0);
        printf("%-22s %8.0f us  %8.0f us  %8.0f us  %8.0f us  %8.0f us\n",
               label, sum / s.size(),
               s[s.size() / 2],
               s[s.size() * 99 / 100],
               s.front(), s.back());
    };

    std::cout << "\n=== RAG Performance (" << iters << " samples) ===" << std::endl;
    std::cout << "Component              avg(us)   p50(us)   p99(us)   min(us)   max(us)" << std::endl;
    std::cout << "─────────────────────────────────────────────────────────────────────" << std::endl;
    stats(vec_us, "Vector (embed+FAISS)");
    stats(bm5_us, "BM25 search");
    stats(hybrid_us, "Hybrid (vec+bm5+RRF)");
    std::cout << std::endl;
}

static void stream_chat(ModelManager& mgr, const std::string& prompt, InferParams& params) {
    auto* engine = mgr.get_engine("");
    if (!engine || !engine->is_loaded()) {
        std::cout << "(no model loaded)" << std::endl;
        return;
    }

    g_interrupt = false;
    bool first_token = true;

    engine->stream_complete(prompt, params,
        [&first_token](const std::string& token) {
            if (g_interrupt) return;
            if (first_token) { std::cout << "\n> "; first_token = false; }
            std::cout << token << std::flush;
        },
        nullptr,  // skip reasoning for CLI simplicity
        [&first_token](const InferResult& result) {
            if (!g_interrupt && !first_token)
                std::cout << "  [" << result.completion_tokens << " tokens]" << std::endl;
        }
    );
}

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::warn);
    signal(SIGINT, sigint_handler);

    std::string model_path;
    std::string embedding_model_path;
    std::string rag_dir;
    std::string rag_store = "data/rag";
    int n_ctx = 2048;
    int n_threads = 4;
    int max_tokens = 512;
    float temperature = 0.7f;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--model" && i+1 < argc) model_path = argv[++i];
        else if (arg == "--embedding-model" && i+1 < argc) embedding_model_path = argv[++i];
        else if (arg == "--rag-dir" && i+1 < argc) rag_dir = argv[++i];
        else if (arg == "--rag-store" && i+1 < argc) rag_store = argv[++i];
        else if (arg == "--ctx" && i+1 < argc) n_ctx = std::stoi(argv[++i]);
        else if (arg == "--threads" && i+1 < argc) n_threads = std::stoi(argv[++i]);
        else if (arg == "--max-tokens" && i+1 < argc) max_tokens = std::stoi(argv[++i]);
        else if (arg == "--temp" && i+1 < argc) temperature = std::stof(argv[++i]);
    }

    if (model_path.empty()) {
        std::cerr << "Usage: fast-llm-cli --model <path> [--rag-dir <dir>] [--ctx 2048] [--temp 0.7]\n";
        return 1;
    }

    ModelManager mgr;
    InferParams params;
    params.n_ctx = n_ctx;
    params.n_batch = 512;
    params.n_threads = n_threads;
    params.n_gpu_layers = -1;
    params.max_tokens = max_tokens;
    params.temperature = temperature;
    params.session_id = "cli";
    params.system_prompt = "You are a helpful AI assistant.";

    std::cout << "Loading model: " << model_path << "..." << std::endl;
    if (!mgr.load_model("default", model_path, params)) {
        std::cerr << "Failed to load model\n";
        return 1;
    }
    std::cout << "Model loaded (ctx=" << n_ctx << ", threads=" << n_threads
              << ", gpu_layers=all, temp=" << temperature << ")" << std::endl;

    // RAG setup
    RagService rag;
    rag.set_storage_dir(rag_store);

    auto* engine = dynamic_cast<LlmEngine*>(mgr.get_engine(""));
    if (engine && engine->is_loaded()) {
        engine->activate_session("cli", params.system_prompt);
    }

    if (!embedding_model_path.empty()) {
        auto embedder = std::make_unique<LlamaEmbedder>();
        if (embedder->load_model(embedding_model_path)) {
            rag.set_embedder(std::move(embedder));
            spdlog::info("Embedding model loaded: {}", embedding_model_path);
        }
    } else if (engine && engine->is_loaded()) {
        auto embedder = std::make_unique<LlamaEmbedder>();
        if (embedder->init_from_model(engine->get_llama_model())) {
            rag.set_embedder(std::move(embedder));
        }
    }
    if (rag.is_ready() && !rag_dir.empty()) {
        std::cout << "Loading documents from: " << rag_dir << std::endl;
        load_dir(rag, rag_dir);
    }

    std::cout << "\nType /help for commands, /quit to exit.\n\n";

    std::string line;
    while (g_running && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        if (line == "/quit" || line == "/exit") {
            break;
        } else if (line == "/help") {
            std::cout << "  /rag <query>   Search docs (full) then answer with RAG\n"
                       << "  /search <q>    Hybrid search (vec+BM25+RRF)\n"
                       << "  /vec <q>       Vector-only search\n"
                       << "  /bm5 <q>       BM25-only search\n"
                       << "  /parents <q>   Search parent documents (no LLM)\n"
                       << "  /bench         Run RAG performance benchmark\n"
                       << "  /load <file>   Load a text file into knowledge base\n"
                      << "  /docs          List loaded documents\n"
                      << "  /del <id>      Remove a document\n"
                      << "  /temp <0-2>    Set temperature (current: " << temperature << ")\n"
                      << "  /max-tk <n>    Set max_tokens (current: " << params.max_tokens << ")\n"
                      << "  /ctx           Show context usage\n"
                      << "  /clear         Clear session (reset KV cache)\n"
                       << "  /quit          Exit\n";
        } else if (line == "/bench") {
            bench_and_print(rag);
        } else if (line.rfind("/rag ", 0) == 0) {
            if (!rag.is_ready()) {
                std::cout << "(RAG not available)" << std::endl;
            } else {
                auto rag_prompt = rag.build_rag_prompt_from_parents(line.substr(5), 1, 3);
                stream_chat(mgr, rag_prompt, params);
            }
        } else if (line.rfind("/search ", 0) == 0) {
            search_and_print(rag, line.substr(8));
        } else if (line.rfind("/vec ", 0) == 0) {
            vec_and_print(rag, line.substr(5));
        } else if (line.rfind("/bm5 ", 0) == 0) {
            bm5_and_print(rag, line.substr(5));
        } else if (line.rfind("/parents ", 0) == 0) {
            parents_and_print(rag, line.substr(9));
        } else if (line.rfind("/load ", 0) == 0) {
            std::string path = line.substr(6);
            auto content = read_file(path);
            if (content.empty()) {
                std::cout << "Failed to read: " << path << std::endl;
            } else {
                std::string fname = std::filesystem::path(path).filename().string();
                std::string category = std::filesystem::path(path).parent_path().filename().string();
                int doc_id = rag.add_document(1, fname, content, category, "group");
                if (doc_id >= 0)
                    std::cout << "Loaded document #" << doc_id << ": " << fname
                              << " (" << content.size() << " bytes)" << std::endl;
                else
                    std::cout << "Failed to load document (RAG not ready?)" << std::endl;
            }
        } else if (line == "/docs") {
            std::cout << "Documents loaded: " << rag.document_count() << std::endl;
        } else if (line.rfind("/del ", 0) == 0) {
            int doc_id = std::stoi(line.substr(5));
            rag.remove_document(doc_id);
            std::cout << "Removed document #" << doc_id << " (" << rag.document_count() << " remaining)" << std::endl;
        } else if (line.rfind("/temp ", 0) == 0) {
            temperature = std::stof(line.substr(6));
            params.temperature = temperature;
            std::cout << "Temperature set to " << temperature << std::endl;
        } else if (line.rfind("/max-tk ", 0) == 0) {
            params.max_tokens = std::stoi(line.substr(8));
            std::cout << "Max tokens set to " << params.max_tokens << std::endl;
        } else if (line == "/ctx") {
            if (engine) {
                std::cout << "Session KV tokens: " << engine->session_kv_tokens("cli") << std::endl;
                std::cout << "KV cache usage: " << engine->kv_cache_usage() / 1024 / 1024 << " MB" << std::endl;
            }
        } else if (line == "/clear") {
            if (engine) {
                engine->deactivate_session("cli");
                engine->activate_session("cli", params.system_prompt);
                std::cout << "Session cleared." << std::endl;
            }
        } else {
            stream_chat(mgr, line, params);
        }
    }

    std::cout << "\nGoodbye." << std::endl;
    return 0;
}
