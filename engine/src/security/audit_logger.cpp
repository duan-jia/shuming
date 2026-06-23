#include "security/audit_logger.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <chrono>
#include <ctime>
#include <sys/stat.h>

namespace fastllm {

AuditLogger::AuditLogger(const std::string& wal_dir) : wal_dir_(wal_dir) {
    ensure_dir(wal_dir_);
}

AuditLogger::~AuditLogger() = default;

void AuditLogger::log(int tenant_id, int user_id, const std::string& action,
                        const std::string& detail, const std::string& ip) {
    nlohmann::json j;
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&time_t));

    j["ts"] = time_buf;
    j["tenant_id"] = tenant_id;
    j["user_id"] = user_id;
    j["action"] = action;
    if (!detail.empty()) j["detail"] = detail;
    if (!ip.empty()) j["ip"] = ip;

    std::string line = j.dump() + "\n";

    std::lock_guard<std::mutex> lock(mutex_);
    rotate_if_needed();

    std::ofstream file(current_file_, std::ios::app);
    if (file.is_open()) {
        file << line;
        file.flush();
    } else {
        spdlog::warn("Failed to write audit log: {}", current_file_);
    }
}

void AuditLogger::set_flush_interval(int seconds) {
    // WAL files are flushed on every write for durability
}

void AuditLogger::ensure_dir(const std::string& dir) {
    mkdir(dir.c_str(), 0755);
}

std::string AuditLogger::current_wal_path() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&time_t));
    return wal_dir_ + "/audit-" + std::string(buf) + ".wal";
}

void AuditLogger::rotate_if_needed() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    int today = std::localtime(&time_t)->tm_yday;

    if (today != current_day_ || current_file_.empty()) {
        current_day_ = today;
        current_file_ = current_wal_path();
    }
}

}
