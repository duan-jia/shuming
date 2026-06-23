#include "inference/prefix_cache.h"

#include <spdlog/spdlog.h>

namespace fastllm {

size_t PrefixCache::compute_prefix_hash(const std::string& prefix) {
    return std::hash<std::string>{}(prefix);
}

bool PrefixCache::has_prefix(size_t hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.count(hash) > 0;
}

void PrefixCache::register_prefix(size_t hash, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(hash);
    if (it != entries_.end()) {
        auto& entry = it->second;
        if (entry.primary_session.empty()) {
            entry.primary_session = session_id;
        } else {
            entry.sharing_sessions.push_back(session_id);
        }
        spdlog::debug("PrefixCache: session {} shares prefix {} (total sharing: {})",
                      session_id, hash, entry.sharing_sessions.size() + 1);
    } else {
        PrefixEntry entry;
        entry.hash = hash;
        entry.primary_session = session_id;
        entries_[hash] = entry;
        spdlog::debug("PrefixCache: new prefix {} registered by session {}", hash, session_id);
    }
}

void PrefixCache::unregister_prefix(size_t hash, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(hash);
    if (it == entries_.end()) return;

    auto& entry = it->second;
    if (entry.primary_session == session_id) {
        if (entry.sharing_sessions.empty()) {
            entries_.erase(it);
        } else {
            entry.primary_session = entry.sharing_sessions.back();
            entry.sharing_sessions.pop_back();
        }
    } else {
        auto& sessions = entry.sharing_sessions;
        sessions.erase(std::remove(sessions.begin(), sessions.end(), session_id), sessions.end());
    }
}

std::string PrefixCache::get_source_session(size_t hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(hash);
    return it != entries_.end() ? it->second.primary_session : "";
}

std::vector<std::string> PrefixCache::get_sessions_sharing(size_t hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(hash);
    if (it == entries_.end()) return {};

    auto result = it->second.sharing_sessions;
    result.push_back(it->second.primary_session);
    return result;
}

void PrefixCache::set_prefix_length(size_t hash, size_t num_tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(hash);
    if (it != entries_.end()) {
        it->second.num_tokens = num_tokens;
    }
}

size_t PrefixCache::get_prefix_length(size_t hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(hash);
    return it != entries_.end() ? it->second.num_tokens : 0;
}

size_t PrefixCache::shared_prefixes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t PrefixCache::total_shared_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& [_, entry] : entries_) {
        total += 1 + entry.sharing_sessions.size();
    }
    return total;
}

}
