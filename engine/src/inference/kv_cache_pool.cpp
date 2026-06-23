#include "inference/kv_cache_pool.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace fastllm {

KvCachePool::KvCachePool(size_t max_gpu_bytes)
    : max_gpu_bytes_(max_gpu_bytes) {}

void KvCachePool::on_session_active(const std::string& session_id, size_t num_tokens, size_t gpu_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(session_id);
    if (it != entries_.end()) {
        total_gpu_bytes_ -= it->second.gpu_bytes;
        it->second.num_tokens = num_tokens;
        it->second.gpu_bytes = gpu_bytes;
        it->second.last_access = std::chrono::steady_clock::now();
        lru_order_.erase(it->second.lru_iter);
        lru_order_.push_front(session_id);
        it->second.lru_iter = lru_order_.begin();
    } else {
        KvCacheEntry entry;
        entry.session_id = session_id;
        entry.num_tokens = num_tokens;
        entry.gpu_bytes = gpu_bytes;
        entry.last_access = std::chrono::steady_clock::now();

        lru_order_.push_front(session_id);
        entry.lru_iter = lru_order_.begin();
        entries_[session_id] = entry;
    }

    total_gpu_bytes_ += gpu_bytes;
    spdlog::debug("KV Cache: session {} active, {} tokens, {} bytes (total: {}/{})",
                  session_id, num_tokens, gpu_bytes, total_gpu_bytes_, max_gpu_bytes_);

    if (total_gpu_bytes_ > max_gpu_bytes_) {
        evict(total_gpu_bytes_ - max_gpu_bytes_);
    }
}

void KvCachePool::on_session_release(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    remove_entry(session_id);
}

bool KvCachePool::touch(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(session_id);
    if (it == entries_.end()) return false;

    it->second.last_access = std::chrono::steady_clock::now();
    lru_order_.erase(it->second.lru_iter);
    lru_order_.push_front(session_id);
    it->second.lru_iter = lru_order_.begin();
    return true;
}

void KvCachePool::evict(size_t target_bytes) {
    size_t freed = 0;

    while (freed < target_bytes && !lru_order_.empty()) {
        std::string victim = lru_order_.back();

        auto it = entries_.find(victim);
        if (it != entries_.end()) {
            freed += it->second.gpu_bytes;
            spdlog::info("KV Cache: evicting session {} ({} tokens, {} bytes)",
                        victim, it->second.num_tokens, it->second.gpu_bytes);

            if (eviction_cb_) {
                eviction_cb_(victim);
            }

            remove_entry(victim);
        }
    }

    spdlog::debug("KV Cache: evicted {} bytes, total now: {}/{}", freed, total_gpu_bytes_, max_gpu_bytes_);
}

void KvCachePool::evict_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    remove_entry(session_id);
}

size_t KvCachePool::active_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool KvCachePool::has_session(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.count(session_id) > 0;
}

const KvCacheEntry* KvCachePool::get_entry(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(session_id);
    return it != entries_.end() ? &it->second : nullptr;
}

void KvCachePool::move_to_front(const std::string& session_id) {
    auto it = entries_.find(session_id);
    if (it != entries_.end()) {
        lru_order_.erase(it->second.lru_iter);
        lru_order_.push_front(session_id);
        it->second.lru_iter = lru_order_.begin();
    }
}

void KvCachePool::remove_entry(const std::string& session_id) {
    auto it = entries_.find(session_id);
    if (it != entries_.end()) {
        total_gpu_bytes_ -= it->second.gpu_bytes;
        lru_order_.erase(it->second.lru_iter);
        entries_.erase(it);
    }
}

}
