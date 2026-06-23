#pragma once

#include <string>
#include <unordered_map>
#include <list>
#include <mutex>
#include <functional>
#include <chrono>

namespace fastllm {

struct KvCacheEntry {
    std::string session_id;
    size_t num_tokens = 0;
    size_t gpu_bytes = 0;
    std::chrono::steady_clock::time_point last_access;
    std::list<std::string>::iterator lru_iter;
};

class KvCachePool {
public:
    explicit KvCachePool(size_t max_gpu_bytes);

    void on_session_active(const std::string& session_id, size_t num_tokens, size_t gpu_bytes);
    void on_session_release(const std::string& session_id);

    bool touch(const std::string& session_id);
    void evict(size_t target_bytes);
    void evict_session(const std::string& session_id);

    size_t total_gpu_bytes() const { return total_gpu_bytes_; }
    size_t max_gpu_bytes() const { return max_gpu_bytes_; }
    size_t active_sessions() const;

    bool has_session(const std::string& session_id) const;
    const KvCacheEntry* get_entry(const std::string& session_id) const;

    void set_eviction_callback(std::function<void(const std::string&)> cb) {
        eviction_cb_ = std::move(cb);
    }

private:
    void move_to_front(const std::string& session_id);
    void remove_entry(const std::string& session_id);

    size_t max_gpu_bytes_;
    size_t total_gpu_bytes_ = 0;

    std::unordered_map<std::string, KvCacheEntry> entries_;
    std::list<std::string> lru_order_;

    mutable std::mutex mutex_;
    std::function<void(const std::string&)> eviction_cb_;
};

}
