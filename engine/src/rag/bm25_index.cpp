#include "rag/bm25_index.h"
#include "rag/vector_index.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace fastllm {

void Bm25Index::synchronize_scopes(const std::unordered_map<int, std::pair<int, std::string>>& scopes) {
    std::lock_guard<std::mutex> lock(mutex_);
    docs_.erase(std::remove_if(docs_.begin(), docs_.end(), [&](const Bm25Doc& doc) {
        return scopes.count(doc.doc_id) == 0;
    }), docs_.end());
    inv_.clear();
    for (size_t i = 0; i < docs_.size(); ++i) {
        auto& doc = docs_[i];
        const auto& scope = scopes.at(doc.doc_id);
        doc.tenant_id = scope.first;
        doc.visibility = scope.second;
        for (const auto& term : doc.term_freq) inv_[term.first].insert(static_cast<int>(i));
    }
}

std::vector<std::string> Bm25Index::tokenize(const std::string& text) const {
    std::vector<std::string> tokens;
    if (text.empty()) return tokens;

    size_t i = 0;
    size_t len = text.size();
    std::vector<std::string> chars;

    while (i < len) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        int clen = 1;
        if ((c & 0x80) == 0) {
            clen = 1;
        } else if ((c & 0xE0) == 0xC0) {
            clen = 2;
        } else if ((c & 0xF0) == 0xE0) {
            clen = 3;
        } else if ((c & 0xF8) == 0xF0) {
            clen = 4;
        }
        if (i + clen > len) clen = static_cast<int>(len - i);

        std::string ch = text.substr(i, clen);
        if (clen == 1 && (std::isalnum(c) || c == '_')) {
            chars.push_back(ch);
        } else if (clen > 1) {
            chars.push_back(ch);
        }
        i += clen;
    }

    // unigrams
    for (const auto& ch : chars) {
        tokens.push_back(ch);
    }

    // bigrams
    for (size_t j = 0; j + 1 < chars.size(); ++j) {
        tokens.push_back(chars[j] + chars[j + 1]);
    }

    return tokens;
}

void Bm25Index::add(int doc_id, int chunk_index, const std::string& content,
                     const ChunkMetadata& metadata, int tenant_id,
                     const std::string& visibility) {
    std::lock_guard<std::mutex> lock(mutex_);

    Bm25Doc doc;
    doc.doc_id = doc_id;
    doc.chunk_index = chunk_index;
    doc.tenant_id = tenant_id;
    doc.visibility = visibility;
    doc.metadata = metadata;
    doc.content = content;

    auto tokens = tokenize(content);
    doc.term_count = static_cast<int>(tokens.size());

    for (const auto& t : tokens) {
        doc.term_freq[t]++;
    }

    int idx = static_cast<int>(docs_.size());
    docs_.push_back(std::move(doc));

    for (const auto& [term, freq] : docs_[idx].term_freq) {
        (void)freq;
        inv_[term].insert(idx);
    }
}

void Bm25Index::remove_by_doc(int doc_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t write = 0;
    std::vector<int> old_to_new(docs_.size(), -1);

    for (size_t i = 0; i < docs_.size(); ++i) {
        if (docs_[i].doc_id != doc_id) {
            if (write != i) {
                docs_[write] = std::move(docs_[i]);
            }
            old_to_new[i] = static_cast<int>(write);
            ++write;
        }
    }
    docs_.resize(write);

    inv_.clear();
    for (int i = 0; i < static_cast<int>(docs_.size()); ++i) {
        for (const auto& [term, freq] : docs_[i].term_freq) {
            (void)freq;
            inv_[term].insert(i);
        }
    }
}

std::vector<SearchResult> Bm25Index::search(const std::string& query, int top_k,
                                             int tenant_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (docs_.empty()) return {};

    auto q_terms = tokenize(query);
    if (q_terms.empty()) return {};

    std::unordered_set<int> candidate_set;
    for (const auto& t : q_terms) {
        auto it = inv_.find(t);
        if (it != inv_.end()) {
            candidate_set.insert(it->second.begin(), it->second.end());
        }
    }
    std::vector<int> candidates(candidate_set.begin(), candidate_set.end());
    if (candidates.empty()) return {};

    size_t N = docs_.size();
    double total_len = 0.0;
    for (const auto& d : docs_) total_len += d.term_count;
    double avgdl = total_len / N;

    std::vector<std::pair<double, int>> scored;
    for (int idx : candidates) {
        const auto& doc = docs_[idx];
        if (doc.visibility != "public" && tenant_id >= 0 && doc.tenant_id != tenant_id) continue;
        double score = 0.0;
        for (const auto& term : q_terms) {
            auto it = inv_.find(term);
            if (it == inv_.end()) continue;
            double df = static_cast<double>(it->second.size());
            double idf = std::log((N - df + 0.5) / (df + 0.5) + 1.0);

            auto tf_it = doc.term_freq.find(term);
            if (tf_it == doc.term_freq.end()) continue;
            double tf = static_cast<double>(tf_it->second);

            double numerator = tf * (k1_ + 1.0);
            double denominator = tf + k1_ * (1.0 - b_ + b_ * doc.term_count / avgdl);
            score += idf * numerator / denominator;
        }
        scored.emplace_back(score, idx);
    }

    int k = std::min(top_k, static_cast<int>(scored.size()));
    std::partial_sort(scored.begin(), scored.begin() + k, scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<SearchResult> results;
    for (int i = 0; i < k; ++i) {
        const auto& doc = docs_[scored[i].second];
        SearchResult r;
        r.doc_id = doc.doc_id;
        r.chunk_index = doc.chunk_index;
        r.score = static_cast<float>(scored[i].first);
        r.content = doc.content;
        r.metadata = doc.metadata;
        results.push_back(r);
    }
    return results;
}

bool Bm25Index::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& d : docs_) {
            nlohmann::json j;
            j["doc_id"] = d.doc_id;
            j["chunk_index"] = d.chunk_index;
            j["tenant_id"] = d.tenant_id;
            j["visibility"] = d.visibility;
            j["content"] = d.content;
            j["term_count"] = d.term_count;
            j["category"] = d.metadata.category;
            j["title"] = d.metadata.title;

            nlohmann::json tf;
            for (const auto& [term, freq] : d.term_freq) {
                tf[term] = freq;
            }
            j["term_freq"] = tf;
            arr.push_back(j);
        }

        std::string dumped = arr.dump(2);
        std::ofstream of(path, std::ios::trunc);
        if (!of.is_open()) return false;
        of << dumped;
        of.close();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Bm25Index save exception: {}", e.what());
        return false;
    }
}

bool Bm25Index::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        std::ifstream inf(path);
        if (!inf) return false;
        auto arr = nlohmann::json::parse(inf);
        inf.close();

        docs_.clear();
        inv_.clear();

        for (const auto& j : arr) {
            Bm25Doc d;
            d.doc_id = j["doc_id"];
            d.chunk_index = j["chunk_index"];
            d.tenant_id = j.value("tenant_id", 0);
            d.visibility = j.value("visibility", "group");
            d.content = j["content"];
            d.term_count = j["term_count"];
            d.metadata.category = j.value("category", "");
            // Read historical indexes without requiring a rebuild.
            d.metadata.title = j.value("title", j.value("dish", ""));

            auto tf = j["term_freq"];
            for (auto& [term, freq] : tf.items()) {
                d.term_freq[term] = freq.get<int>();
            }

            int idx = static_cast<int>(docs_.size());
            docs_.push_back(std::move(d));

            for (const auto& [term, freq] : docs_[idx].term_freq) {
                (void)freq;
                inv_[term].insert(idx);
            }
        }
        spdlog::info("Bm25Index loaded {} docs from {}", docs_.size(), path);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Bm25Index load: {}", e.what());
        return false;
    }
}

}
