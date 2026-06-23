#pragma once

#include <string>
#include <memory>
#include <mutex>

struct event;
struct evconnlistener;

namespace fastllm {

class AuditLogger {
public:
    explicit AuditLogger(const std::string& wal_dir = "/tmp/llm-platform/audit");
    ~AuditLogger();

    void log(int tenant_id, int user_id, const std::string& action,
             const std::string& detail = "",
             const std::string& ip = "");

    void set_flush_interval(int seconds);

private:
    void ensure_dir(const std::string& dir);
    std::string current_wal_path() const;
    void rotate_if_needed();

    std::string wal_dir_;
    std::string current_file_;
    int current_day_ = 0;
    std::mutex mutex_;
};

}
