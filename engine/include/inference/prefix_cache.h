#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <functional>

namespace fastllm {

class PrefixCache {
public:
    size_t compute_prefix_hash(const std::string& prefix);

    bool has_prefix(size_t hash) const;
    void register_prefix(size_t hash, const std::string& session_id);
    void unregister_prefix(size_t hash, const std::string& session_id);

    std::string get_source_session(size_t hash) const;
    std::vector<std::string> get_sessions_sharing(size_t hash) const;

    void set_prefix_length(size_t hash, size_t num_tokens);
    size_t get_prefix_length(size_t hash) const;

    size_t shared_prefixes() const;
    size_t total_shared_sessions() const;

private:
    struct PrefixEntry {
        size_t hash = 0;
        size_t num_tokens = 0;
        std::string primary_session;
        std::vector<std::string> sharing_sessions;
    };

    std::unordered_map<size_t, PrefixEntry> entries_;
    mutable std::mutex mutex_;
};

}
