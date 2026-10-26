#pragma once

#include <memory>
#include <string>

#include "auth/jwt_handler.h"
#include "inference/batch_scheduler.h"
#include "inference/model_manager.h"
#include "persistence/billing_service.h"
#include "persistence/document_repository.h"
#include "persistence/message_repository.h"
#include "persistence/mysql_pool.h"
#include "persistence/session_repository.h"
#include "persistence/tenant_repository.h"
#include "persistence/user_repository.h"
#include "rag/document_store.h"
#include "security/rate_limiter.h"
#include "security/rbac_manager.h"
#include "services/knowledge_search_service.h"
#include "services/rag_knowledge_search_backend.h"
#include "services/session_service.h"

namespace fastllm {

struct AppContext {
    bool guest_enabled = false;
    int guest_tenant_id = 0;
    std::string rag_dir = "data/rag";
    std::string system_prompt = "You are a helpful AI assistant for enterprise internal use.";

    std::unique_ptr<MysqlPool> mysql_pool;
    std::unique_ptr<JwtHandler> jwt;
    std::unique_ptr<ModelManager> model_mgr;
    std::unique_ptr<RagService> rag;
    std::unique_ptr<RagKnowledgeSearchBackend> knowledge_search_backend;
    std::unique_ptr<KnowledgeSearchService> knowledge_search_service;
    std::unique_ptr<RateLimiter> rate_limiter;
    std::unique_ptr<BillingService> billing_service;
    std::unique_ptr<UserRepository> user_repository;
    std::unique_ptr<TenantRepository> tenant_repository;
    std::unique_ptr<DocumentRepository> document_repository;
    std::unique_ptr<MessageRepository> message_repository;
    std::unique_ptr<SessionRepository> session_repository;
    std::unique_ptr<SessionService> session_service;
    std::unique_ptr<BatchScheduler> scheduler;
};

}
