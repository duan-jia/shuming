#pragma once

#include "inference/i_engine.h"
#include "persistence/mysql_pool.h"

namespace fastllm {

// Durable execution journal. Ownership is checked against sessions on every read.
class AgentRepository {
public:
    explicit AgentRepository(MysqlPool& mysql) : mysql_(mysql) {}
    bool schema_ready();
    bool begin(const std::string& session, const std::string& request, const std::string& model,
               const std::string& prompt, const nlohmann::json& attachments = nlohmann::json::array());
    bool append(const std::string& request, const nlohmann::json& event);
    bool finish(const std::string& request, const std::string& status);
    std::vector<ChatMessage> history(const std::string& session);
    nlohmann::json attachments(const std::string& session);
    nlohmann::json requests(const std::string& session, int tenant, int user);
    nlohmann::json trace(const std::string& session, const std::string& request, int tenant, int user);

private:
    MysqlPool& mysql_;
    static std::string quote(MYSQL* mysql, const std::string& value);
};

} // namespace fastllm
