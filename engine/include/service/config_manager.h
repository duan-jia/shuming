#pragma once

#include <string>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fastllm {

class ConfigManager {
public:
    bool load(const std::string& path) {
        try {
            std::ifstream f(path);
            if (!f.is_open()) return false;
            data_ = nlohmann::json::parse(f);
            return true;
        } catch (const std::exception& e) {
            last_error_ = e.what();
            return false;
        }
    }

    const std::string& last_error() const { return last_error_; }
    const nlohmann::json& raw() const { return data_; }

    std::string get_str(const std::string& path, const std::string& default_val = "") const {
        return value_at(path, default_val).get<std::string>();
    }

    // Empty environment entries (for example an unfilled Compose .env) do not
    // mask an existing local config value.
    std::string get_env_str(const std::string& path, const char* variable) const {
        const char* value = std::getenv(variable);
        return value && *value ? std::string(value) : get_str(path, "");
    }

    int get_int(const std::string& path, int default_val = 0) const {
        return value_at(path, default_val).get<int>();
    }

    float get_float(const std::string& path, float default_val = 0.0f) const {
        return value_at(path, default_val).get<float>();
    }

    bool get_bool(const std::string& path, bool default_val = false) const {
        return value_at(path, default_val).get<bool>();
    }

    const nlohmann::json& get(const std::string& path) const {
        return value_at(path);
    }

private:
    const nlohmann::json& value_at(const std::string& path, const nlohmann::json& default_val) const {
        try {
            return value_at(path);
        } catch (...) {
            return default_val;
        }
    }

    template<typename T>
    const nlohmann::json& value_at(const std::string& path, T default_val) const {
        static nlohmann::json s_default;
        s_default = default_val;
        try {
            return value_at(path);
        } catch (...) {
            return s_default;
        }
    }

    const nlohmann::json& value_at(const std::string& path) const {
        const nlohmann::json* cur = &data_;
        size_t start = 0;
        while (start < path.size()) {
            size_t dot = path.find('.', start);
            std::string key = (dot == std::string::npos) ? path.substr(start) : path.substr(start, dot - start);
            cur = &cur->at(key);
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return *cur;
    }

    nlohmann::json data_;
    std::string last_error_;
};

} // namespace fastllm
