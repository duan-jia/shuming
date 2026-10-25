#include "rag/chunker.h"

#include <regex>
#include <sstream>
#include <algorithm>

namespace fastllm {

Chunker::Chunker() : config_{} {}

Chunker::Chunker(const Config& config) : config_(config) {}

std::vector<DocumentChunk> Chunker::chunk(int doc_id, const std::string& text,
                                           const std::string& category) const {
    // --- Extract document-level metadata ---
    ChunkMetadata meta;
    meta.category = category;

    static const std::regex title_re(R"(^# (.+))");
    std::istringstream pre_scan(text);
    std::string line;
    while (std::getline(pre_scan, line)) {
        std::smatch match;
        if (std::regex_match(line, match, title_re)) {
            meta.title = match[1].str();
            break;
        }
    }

    // --- Section parsing ---
    if (text.empty()) return {};

    static const std::regex heading_re(R"(^(#{1,6})\s+(.+)$)");

    struct RawSection {
        int heading_level = 0;
        std::string heading;
        std::string body;
    };

    std::vector<RawSection> sections;
    RawSection current;
    bool started = false;

    auto flush = [&]() {
        if (started) sections.push_back(std::move(current));
        current = RawSection{};
        started = false;
    };

    std::istringstream stream(text);
    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_match(line, match, heading_re)) {
            int lvl = static_cast<int>(match[1].length());
            if (lvl > 3) {
                if (started) current.body += line + "\n";
                continue;
            }
            flush();
            current.heading_level = lvl;
            current.heading = match[2].str();
            started = true;
        } else if (started) {
            current.body += line + "\n";
        }
    }
    flush();

    // --- Convert to DocumentChunk ---
    int chunk_index = 0;
    std::vector<DocumentChunk> result;

    // Track H2 indices for post-processing
    std::vector<size_t> h2_indices;
    for (size_t i = 0; i < sections.size(); ++i)
        if (sections[i].heading_level == 2) h2_indices.push_back(i);

    for (size_t si = 0; si < sections.size(); ++si) {
        auto& s = sections[si];

        std::string body_trimmed = s.body;
        while (!body_trimmed.empty() && body_trimmed.back() == '\n')
            body_trimmed.pop_back();
        while (!body_trimmed.empty() && body_trimmed.front() == '\n')
            body_trimmed.erase(0, 1);

        DocumentChunk ch;
        ch.doc_id = doc_id;
        ch.chunk_index = chunk_index++;
        ch.metadata = meta;

        std::string hdr = std::string(s.heading_level, '#') + " " + s.heading;
        if (body_trimmed.empty()) {
            ch.content = hdr;
        } else {
            ch.content = hdr + "\n" + body_trimmed;
        }

        result.push_back(ch);
    }

    // Post-process: fill empty H2 sections with child listing
    for (auto h2_idx : h2_indices) {
        bool has_empty_body = result[h2_idx].content.find('\n') == std::string::npos;
        if (has_empty_body) {
            std::vector<std::string> child_heads;
            for (size_t j = h2_idx + 1; j < result.size(); ++j) {
                if (sections[j].heading_level == 2) break;
                if (sections[j].heading_level == 3)
                    child_heads.push_back(sections[j].heading);
            }
            if (!child_heads.empty()) {
                std::ostringstream oss;
                oss << "[子步骤: ";
                for (size_t c = 0; c < child_heads.size(); ++c) {
                    if (c > 0) oss << ", ";
                    oss << child_heads[c];
                }
                oss << "]";
                result[h2_idx].content += "\n" + oss.str();
            }
        }
    }

    // Preamble (text before first heading)
    std::string preamble;
    {
        std::istringstream st2(text);
        std::string l;
        while (std::getline(st2, l)) {
            std::smatch m;
            if (std::regex_match(l, m, heading_re)) break;
            if (!l.empty() || !preamble.empty())
                preamble += l + "\n";
            else if (preamble.empty())
                continue;
        }
    }
    while (!preamble.empty() && preamble.back() == '\n') preamble.pop_back();
    if (!preamble.empty() && preamble.size() >= config_.min_chunk_size) {
        DocumentChunk pre;
        pre.doc_id = doc_id;
        pre.chunk_index = chunk_index++;
        pre.metadata = meta;
        pre.content = preamble;
        result.insert(result.begin(), pre);
        for (size_t i = 1; i < result.size(); ++i)
            result[i].chunk_index = static_cast<int>(i);
    }

    return result;
}

}
