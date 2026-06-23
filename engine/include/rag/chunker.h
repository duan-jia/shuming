#pragma once

#include <string>
#include <vector>

namespace fastllm {

struct ChunkMetadata {
    std::string category;
    std::string dish;
    std::string difficulty;
};

struct DocumentChunk {
    int doc_id = 0;
    int chunk_index = 0;
    std::string content;
    ChunkMetadata metadata;
};

class Chunker {
public:
    struct Config {
        size_t min_chunk_size = 16;
    };

    Chunker();
    explicit Chunker(const Config& config);

    std::vector<DocumentChunk> chunk(int doc_id, const std::string& text,
                                      const std::string& category = "") const;

private:
    Config config_;
};

}
