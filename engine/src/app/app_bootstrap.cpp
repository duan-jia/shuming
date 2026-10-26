#include "app/app_bootstrap.h"

#include <mysql/mysql.h>
#include <spdlog/spdlog.h>

#include "inference/llm_engine.h"
#include "inference/remote_engine.h"
#include "security/crypto_utils.h"

namespace fastllm {

namespace {

std::string escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

}

bool AppBootstrap::initialize(AppContext& ctx, const ConfigManager& config, const AppOptions& options) {
    ctx.rag_dir = options.rag_dir.empty()
        ? config.get_str("rag.storage_dir", "data/rag")
        : options.rag_dir;
    ctx.system_prompt = config.get_str("inference.system_prompt",
        "You are a helpful AI assistant for enterprise internal use.");

    ctx.mysql_pool = std::make_unique<MysqlPool>(
        config.get_str("mysql.host", "127.0.0.1"),
        config.get_int("mysql.port", 3306),
        config.get_str("mysql.user", "root"),
        config.get_str("mysql.password", ""),
        config.get_str("mysql.database", "llm_platform"),
        config.get_int("mysql.pool_size", 4)
    );
    if (!ctx.mysql_pool->initialize()) {
        spdlog::critical("MySQL pool initialization failed, exiting");
        return false;
    }

    const auto jwt_secret = config.get_env_str("jwt.secret", "JWT_SECRET");
    if (jwt_secret.size() < 32 || jwt_secret.find("change-this") != std::string::npos
        || jwt_secret == "fast-llm-reasoner-secret-key-change-in-production") {
        spdlog::critical("Set a non-placeholder JWT_SECRET of at least 32 bytes before startup"); return false;
    }
    if (!ensure_admin(ctx, config)) {
        return false;
    }

    ctx.jwt = std::make_unique<JwtHandler>(
        jwt_secret,
        config.get_int("jwt.expiry_seconds", 86400)
    );

    ctx.rate_limiter = std::make_unique<RateLimiter>();
    if (config.raw().contains("rate_limiter") && config.raw()["rate_limiter"].contains("rules")) {
        for (auto& rule : config.raw()["rate_limiter"]["rules"]) {
            if (rule.value("action", "") != "chat") continue;
            ctx.rate_limiter->add_rule(
                rule.value("action", ""),
                rule.value("max_requests", 30),
                rule.value("window_seconds", 60)
            );
        }
    } else {
        ctx.rate_limiter->add_rule("chat", 20, 60);
    }
    if (!ctx.rate_limiter->has_rule("chat")) {
        ctx.rate_limiter->add_rule("chat", 20, 60);
    }

    ctx.billing_service = std::make_unique<BillingService>(*ctx.mysql_pool);
    ctx.user_repository = std::make_unique<UserRepository>(*ctx.mysql_pool);
    ctx.tenant_repository = std::make_unique<TenantRepository>(*ctx.mysql_pool);
    ctx.guest_enabled = config.get_bool("auth.guest_enabled", false);
    if (ctx.guest_enabled && !ctx.tenant_repository->ensure_guest_tenant(ctx.guest_tenant_id)) {
        spdlog::critical("Cannot initialize isolated guest tenant"); return false;
    }
    ctx.document_repository = std::make_unique<DocumentRepository>(*ctx.mysql_pool);
    ctx.message_repository = std::make_unique<MessageRepository>(*ctx.mysql_pool);
    ctx.session_repository = std::make_unique<SessionRepository>(*ctx.mysql_pool);
    ctx.session_service = std::make_unique<SessionService>(*ctx.session_repository);

    ctx.model_mgr = std::make_unique<ModelManager>();
    ctx.rag = std::make_unique<RagService>();
    ctx.rag->set_storage_dir(ctx.rag_dir);
    ctx.rag->set_mysql_pool(ctx.mysql_pool.get());
    // OpenAI-compatible generation can run on this host, independently of the backend.
    auto generation_endpoint = config.get_env_str("remote_engine.endpoint", "REMOTE_ENGINE_ENDPOINT");
    if (generation_endpoint.empty()) generation_endpoint = "http://127.0.0.1:8010/v1";
    auto generation_model = config.get_env_str("remote_engine.model", "REMOTE_ENGINE_MODEL");
    if (generation_model.empty()) generation_model = "Qwen3.8-27B";
    auto generation_key = config.get_env_str("remote_engine.api_key", "REMOTE_ENGINE_API_KEY");
    if (generation_key.empty()) generation_key = config.get_env_str("remote_engine.api_key", "DEEPSEEK_API_KEY");
    ctx.rag->set_query_rewriter(std::make_unique<QueryRewriter>(
        config.get_bool("rag.query_rewrite.enabled", true),
        generation_endpoint,
        config.get_str("rag.query_rewrite.model",
            generation_model),
        generation_key,
        config.get_int("rag.query_rewrite.timeout_seconds", 10)
    ));
    ctx.knowledge_search_backend = std::make_unique<RagKnowledgeSearchBackend>(*ctx.rag);
    ctx.knowledge_search_service = std::make_unique<KnowledgeSearchService>(
        *ctx.knowledge_search_backend);

    if (!options.model_path.empty()) {
        spdlog::info("Loading main model: {}", options.model_path);
        InferParams params;
        params.n_ctx = config.get_int("inference.n_ctx", 2048);
        params.n_batch = config.get_int("inference.n_batch", 512);
        params.n_threads = config.get_int("inference.n_threads", 4);
        params.n_gpu_layers = config.get_int("inference.n_gpu_layers", -1);
        params.system_prompt = ctx.system_prompt;
        if (!ctx.model_mgr->load_model("default", options.model_path, params)) {
            spdlog::error("Failed to load model, chat will not work");
        }
    } else {
        spdlog::info("No embedded model specified; generation uses the configured external API");
    }

    auto remote = std::make_unique<RemoteEngine>(
        generation_endpoint,
        generation_model,
        generation_key,
        config.get_int("remote_engine.context_window_tokens", 8192),
        config.get_bool("remote_engine.supports_images", false)
    );
    ctx.model_mgr->add_engine("remote", std::move(remote));

    if (!options.embedding_model_path.empty()) {
        spdlog::info("Loading embedding model: {}", options.embedding_model_path);
        auto embedder = std::make_unique<LlamaEmbedder>();
        if (embedder->load_model(options.embedding_model_path)) {
            ctx.rag->set_embedder(std::move(embedder));
            spdlog::info("Embedding model loaded, RAG ready");
        } else {
            spdlog::warn("Failed to load embedding model, RAG disabled");
        }
    } else {
        auto* engine = ctx.model_mgr->get_engine("");
        auto* llm = dynamic_cast<LlmEngine*>(engine);
        if (llm && llm->is_loaded()) {
            spdlog::info("Using main model for embeddings (shared)");
            auto embedder = std::make_unique<LlamaEmbedder>();
            if (embedder->init_from_model(llm->get_llama_model())) {
                ctx.rag->set_embedder(std::move(embedder));
                spdlog::info("RAG ready (using shared model for embeddings)");
            } else {
                spdlog::warn("Failed to init embedder from shared model, RAG disabled");
            }
        }
    }

    ctx.scheduler = std::make_unique<BatchScheduler>();
    ctx.scheduler->start(config.get_int("scheduler.num_workers", 2));
    return true;
}

bool AppBootstrap::ensure_admin(AppContext& ctx, const ConfigManager& config) {
    std::string admin_username = config.get_str("admin.username", "admin");
    std::string default_pw = config.get_str("admin.default_password", "changeme123");
    auto conn = ctx.mysql_pool->acquire();
    if (!conn) {
        return true;
    }

    auto* res = conn->query("SELECT COUNT(*) FROM users WHERE role='admin'");
    if (!res) {
        ctx.mysql_pool->release(conn);
        return true;
    }

    auto* row = mysql_fetch_row(res);
    int count = row ? std::stoi(row[0]) : 0;
    mysql_free_result(res);
    if (count == 0) {
        if (default_pw.size() < 12 || default_pw == "changeme123") {
            ctx.mysql_pool->release(conn);
            spdlog::critical("Configure a non-default admin.default_password of at least 12 bytes"); return false;
        }
        std::string hash = CryptoUtils::hash_password(default_pw);
        std::string sql = "INSERT INTO users (tenant_id, username, password_hash, role) VALUES "
                          "(1, '" + escape_sql(conn->raw(), admin_username) + "', '"
                          + escape_sql(conn->raw(), hash) + "', 'admin')";
        if (conn->execute(sql)) {
            spdlog::warn("Created bootstrap admin; rotate the bootstrap password before deployment");
        }
    }
    ctx.mysql_pool->release(conn);
    return true;
}

}
