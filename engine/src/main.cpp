#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>
#include <random>
#include <sstream>
#include <iomanip>

#include <muduo/net/EventLoop.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "server/http_server.h"
#include "persistence/mysql_pool.h"
#include "persistence/redis_manager.h"
#include "persistence/double_buffer.h"
#include "auth/jwt_handler.h"
#include "security/rbac_manager.h"
#include "security/crypto_utils.h"
#include "inference/llm_engine.h"
#include "inference/model_manager.h"
#include "inference/remote_engine.h"
#include "inference/batch_scheduler.h"
#include "rag/document_store.h"
#include "security/rate_limiter.h"
#include "persistence/billing.h"
#include "service/config_manager.h"

using namespace fastllm;

static std::string escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

static std::string generate_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    uint32_t d[4] = {dist(gen), dist(gen), dist(gen), dist(gen)};
    char buf[37];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
        d[0],
        (d[1] >> 16) & 0xFFFF,
        d[1] & 0x0FFF | 0x4000,
        (d[2] >> 16) & 0x3FFF | 0x8000,
        d[2] & 0xFFFF,
        d[3]);
    return std::string(buf);
}

static std::string g_rag_dir = "data/rag";
static std::string g_system_prompt = "You are a helpful AI assistant for enterprise internal use.";

static std::unique_ptr<MysqlPool> g_mysql_pool;
static std::unique_ptr<RedisManager> g_redis;
static std::unique_ptr<JwtHandler> g_jwt;
static std::unique_ptr<RbacManager> g_rbac;
static std::unique_ptr<ModelManager> g_model_mgr;
static std::unique_ptr<RagService> g_rag;
static std::unique_ptr<RateLimiter> g_rate_limiter;
static std::unique_ptr<BatchScheduler> g_scheduler;
static DoubleBuffer<nlohmann::json> g_message_buffer;

UserInfo extract_user(const HttpRequest& req) {
    UserInfo info{0, 0, "", Role::USER};
    auto auth = req.header("Authorization", "");
    if (auth.substr(0, 7) == "Bearer ") {
        auto token = auth.substr(7);
        auto claims = g_jwt->verify_token(token);
        if (claims.valid) {
            info.id = claims.user_id;
            info.tenant_id = claims.tenant_id;
            info.username = claims.username;
            info.role = RbacManager::string_to_role(claims.role);
        }
    }
    return info;
}

bool require_auth(const HttpRequest& req, HttpResponse& resp, UserInfo& user) {
    user = extract_user(req);
    if (user.id == 0) {
        resp.json_error(401, "Unauthorized");
        return false;
    }
    return true;
}

bool require_permission(const UserInfo& user, const std::string& resource,
                        const std::string& action, HttpResponse& resp) {
    if (!g_rbac->check_permission(user.role, resource, action)) {
        resp.json_error(403, "Forbidden");
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::debug);

    // Load configuration from file (CLI --config overrides default path)
    ConfigManager config;
    std::string config_path = "config.json";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }
    if (!config.load(config_path)) {
        spdlog::warn("Could not load config.json ({}), using defaults", config.last_error());
    }

    int port = config.get_int("server.port", 8080);
    g_rag_dir = config.get_str("rag.storage_dir", "data/rag");
    g_system_prompt = config.get_str("inference.system_prompt",
        "You are a helpful AI assistant for enterprise internal use.");
    std::string model_path;
    std::string embedding_model_path;
    std::string static_dir = config.get_str("server.static_dir", "");
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::string(argv[i]) == "--model" && i + 1 < argc) {
            model_path = argv[++i];
        } else if (std::string(argv[i]) == "--embedding-model" && i + 1 < argc) {
            embedding_model_path = argv[++i];
        } else if (std::string(argv[i]) == "--static" && i + 1 < argc) {
            static_dir = argv[++i];
        } else if (std::string(argv[i]) == "--rag-dir" && i + 1 < argc) {
            g_rag_dir = argv[++i];
        }
    }

    // Initialize MySQL (must succeed before starting)
    g_mysql_pool = std::make_unique<MysqlPool>(
        config.get_str("mysql.host", "127.0.0.1"),
        config.get_int("mysql.port", 3306),
        config.get_str("mysql.user", "root"),
        config.get_str("mysql.password", ""),
        config.get_str("mysql.database", "llm_platform"),
        config.get_int("mysql.pool_size", 4)
    );
    if (!g_mysql_pool->initialize()) {
        spdlog::critical("MySQL pool initialization failed, exiting");
        return 1;
    }

    // Ensure an admin exists (first-run bootstrap)
    {
        std::string admin_username = config.get_str("admin.username", "admin");
        std::string default_pw = config.get_str("admin.default_password", "changeme123");
        auto conn = g_mysql_pool->acquire();
        if (conn) {
            auto* res = conn->query("SELECT COUNT(*) FROM users WHERE role='admin'");
            if (res) {
                auto* row = mysql_fetch_row(res);
                int count = row ? std::stoi(row[0]) : 0;
                mysql_free_result(res);
                if (count == 0) {
                    std::string hash = CryptoUtils::hash_password(default_pw);
                    std::string sql = "INSERT INTO users (tenant_id, username, password_hash, role) VALUES "
                                      "(1, '" + escape_sql(conn->raw(), admin_username) + "', '"
                                      + escape_sql(conn->raw(), hash) + "', 'admin')";
                    if (conn->execute(sql)) {
                        spdlog::warn("Created default admin '{}' with password '{}'. CHANGE IT IMMEDIATELY.",
                                     admin_username, default_pw);
                    }
                }
            }
            g_mysql_pool->release(conn);
        }
    }

    // Initialize Redis
    g_redis = std::make_unique<RedisManager>(
        config.get_str("redis.host", "127.0.0.1"),
        config.get_int("redis.port", 6379),
        config.get_int("redis.db", 0)
    );
    if (!g_redis->initialize()) {
        spdlog::critical("Redis initialization failed, exiting");
        return 1;
    }

    // Initialize JWT
    g_jwt = std::make_unique<JwtHandler>(
        config.get_str("jwt.secret", "fast-llm-reasoner-secret-key-change-in-production"),
        config.get_int("jwt.expiry_seconds", 86400)
    );

    // Initialize RBAC
    g_rbac = std::make_unique<RbacManager>();

    // Initialize Rate Limiter (load rules from config, with fallback defaults)
    g_rate_limiter = std::make_unique<RateLimiter>();
    if (config.raw().contains("rate_limiter") && config.raw()["rate_limiter"].contains("rules")) {
        for (auto& rule : config.raw()["rate_limiter"]["rules"]) {
            g_rate_limiter->add_rule(
                rule.value("action", ""),
                rule.value("max_requests", 30),
                rule.value("window_seconds", 60)
            );
        }
    } else {
        g_rate_limiter->add_rule("login", 5, 60);
        g_rate_limiter->add_rule("chat", 20, 60);
        g_rate_limiter->add_rule("upload", 10, 60);
        g_rate_limiter->add_rule("documents", 30, 60);
        g_rate_limiter->add_rule("sessions", 30, 60);
    }

    // Set daily token quota for default tenant
    BillingManager::set_daily_quota(*g_redis,
        config.get_int("default_tenant.id", 1),
        config.get_int("default_tenant.daily_token_quota", 100000)
    );

    // Initialize Model Manager
    g_model_mgr = std::make_unique<ModelManager>();

    // Initialize RAG Service
    g_rag = std::make_unique<RagService>();
    g_rag->set_storage_dir(g_rag_dir);
    g_rag->set_mysql_pool(g_mysql_pool.get());

    if (!model_path.empty()) {
        spdlog::info("Loading main model: {}", model_path);
        InferParams params;
        params.n_ctx = config.get_int("inference.n_ctx", 2048);
        params.n_batch = config.get_int("inference.n_batch", 512);
        params.n_threads = config.get_int("inference.n_threads", 4);
        params.n_gpu_layers = config.get_int("inference.n_gpu_layers", -1);
        params.system_prompt = g_system_prompt;
        if (!g_model_mgr->load_model("default", model_path, params)) {
            spdlog::error("Failed to load model, chat will not work");
        }
    } else {
        spdlog::warn("No model specified (--model <path>), chat will not work");
    }

    auto remote = std::make_unique<RemoteEngine>(
        config.get_str("remote_engine.endpoint", "https://api.deepseek.com/v1"),
        config.get_str("remote_engine.model", "deepseek-chat"),
        config.get_str("remote_engine.api_key", "")
    );
    g_model_mgr->add_engine("remote", std::move(remote));

    // Use shared main model for embeddings (only loads a tiny CPU context)
    if (!embedding_model_path.empty()) {
        spdlog::info("Loading embedding model: {}", embedding_model_path);
        auto embedder = std::make_unique<LlamaEmbedder>();
        if (embedder->load_model(embedding_model_path)) {
            g_rag->set_embedder(std::move(embedder));
            spdlog::info("Embedding model loaded, RAG ready");
        } else {
            spdlog::warn("Failed to load embedding model, RAG disabled");
        }
    } else {
        auto* engine = g_model_mgr->get_engine("");
        auto* llm = dynamic_cast<LlmEngine*>(engine);
        if (llm && llm->is_loaded()) {
            spdlog::info("Using main model for embeddings (shared)");
            auto embedder = std::make_unique<LlamaEmbedder>();
            if (embedder->init_from_model(llm->get_llama_model())) {
                g_rag->set_embedder(std::move(embedder));
                spdlog::info("RAG ready (using shared model for embeddings)");
            } else {
                spdlog::warn("Failed to init embedder from shared model, RAG disabled");
            }
        }
    }

    // Initialize double buffer
    g_message_buffer.set_handler([](std::vector<nlohmann::json>& batch) {
        auto conn = g_mysql_pool->acquire();
        if (!conn || !conn->valid()) return;

        for (auto& msg : batch) {
            std::string sql = "INSERT INTO messages (session_id, role, content, tokens_in, tokens_out) VALUES ('"
                + msg["session_id"].get<std::string>() + "','"
                + msg["role"].get<std::string>() + "','"
                + escape_sql(conn->raw(), msg["content"].get<std::string>()) + "',"
                + std::to_string(msg.value("tokens_in", 0)) + ","
                + std::to_string(msg.value("tokens_out", 0)) + ")";
            conn->execute(sql);
        }
        g_mysql_pool->release(conn);
        spdlog::debug("Flushed {} messages to MySQL", batch.size());
    });

    // Initialize inference scheduler (bounded worker pool, replaces detached threads)
    g_scheduler = std::make_unique<BatchScheduler>();
    g_scheduler->start(config.get_int("scheduler.num_workers", 2));

    muduo::net::EventLoop loop;
    muduo::net::InetAddress addr(port);
    HttpServer server(&loop, addr, "fast-llm-reasoner", config.get_int("http_threads", 2));
    server.set_static_dir(static_dir);

    // Health check
    server.router().get("/api/health", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        nlohmann::json j;
        j["status"] = "ok";
        j["service"] = "fast-llm-reasoner";
        j["mysql"] = "connected";
        j["redis"] = g_redis ? "connected" : "disconnected";
        j["model_loaded"] = g_model_mgr->get_engine("") != nullptr;
        resp.json(j.dump());
    });

    // Login
    server.router().post("/api/auth/login", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo dummy = extract_user(req);
        if (g_redis && g_rate_limiter && !g_rate_limiter->allow(dummy.id, "login", *g_redis)) {
            resp.json_error(429, "Too many login attempts"); return;
        }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");

            auto conn = g_mysql_pool->acquire();
            if (!conn) {
                resp.json_error(503, "Database unavailable");
                return;
            }
            std::string sql = "SELECT id, tenant_id, role, password_hash FROM users WHERE username='"
                + escape_sql(conn->raw(), username) + "'";
            auto* result = conn->query(sql);
            if (!result) {
                g_mysql_pool->release(conn);
                resp.json_error(500, "Database error");
                return;
            }

            auto* row = mysql_fetch_row(result);
            if (!row) {
                mysql_free_result(result);
                g_mysql_pool->release(conn);
                resp.json_error(401, "Invalid credentials");
                return;
            }

            int user_id = std::stoi(row[0]);
            int tenant_id = std::stoi(row[1]);
            std::string role = row[2] ? row[2] : "user";
            std::string password_hash = row[3] ? row[3] : "";
            mysql_free_result(result);
            g_mysql_pool->release(conn);

            if (!CryptoUtils::verify_password(password, password_hash)) {
                resp.json_error(401, "Invalid credentials");
                return;
            }

            std::string token = g_jwt->generate_token(user_id, tenant_id, username, role);

            nlohmann::json j;
            j["token"] = token;
            j["user"]["id"] = user_id;
            j["user"]["username"] = username;
            j["user"]["role"] = role;
            j["user"]["tenant_id"] = tenant_id;
            resp.json(j.dump());
        } catch (const std::exception& e) {
            resp.json_error(400, std::string("Invalid request: ") + e.what());
        }
    });

    // Chat completions (SSE streaming)
    server.router().sse("/api/chat/stream", [](const HttpRequest& req, RouteParams params, std::shared_ptr<SseContext> sse_ctx) {
        auto auth = req.header("Authorization", "");
        if (auth.substr(0, 7) != "Bearer ") {
            nlohmann::json err;
            err["error"] = "Unauthorized";
            sse_ctx->send_event("error", err.dump());
            sse_ctx->close();
            return;
        }

        auto claims = g_jwt->verify_token(auth.substr(7));
        if (!claims.valid) {
            nlohmann::json err;
            err["error"] = "Invalid token";
            sse_ctx->send_event("error", err.dump());
            sse_ctx->close();
            return;
        }

        sse_ctx->user_id = claims.user_id;
        sse_ctx->tenant_id = claims.tenant_id;
        sse_ctx->role = claims.role;

        if (g_redis && g_rate_limiter && !g_rate_limiter->allow(claims.user_id, "chat", *g_redis)) {
            nlohmann::json err;
            err["error"] = "Rate limited";
            sse_ctx->send_event("error", err.dump());
            sse_ctx->close();
            return;
        }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string message = body.value("message", "");
            std::string session_id = body.value("session_id", "default");
            std::string model_name = body.value("model", "remote");

            auto* engine = g_model_mgr->get_engine(model_name);
            if (!engine || !engine->is_loaded()) {
                nlohmann::json err;
                err["error"] = "No model loaded";
                sse_ctx->send_event("error", err.dump());
                sse_ctx->close();
                return;
            }

            InferParams infer_params;
            infer_params.session_id = session_id;
            infer_params.max_tokens = body.value("max_tokens", 512);
            infer_params.temperature = body.value("temperature", 0.7f);
            infer_params.top_p = body.value("top_p", 0.9f);

            bool enable_rag = body.value("enable_rag", false);
            if (enable_rag && g_rag && g_rag->is_ready()) {
                infer_params.system_prompt = g_rag->build_rag_prompt(message, claims.tenant_id, 3);
            } else {
                infer_params.system_prompt = g_system_prompt;
            }

            auto sse = sse_ctx;
            int bill_tenant = claims.tenant_id;
            int bill_user = claims.user_id;

            auto req = std::make_shared<InferRequest>();
            req->session_id = session_id;
            req->prompt = message;
            req->system_prompt = infer_params.system_prompt;
            req->max_tokens = infer_params.max_tokens;
            req->temperature = infer_params.temperature;
            req->top_p = infer_params.top_p;
            req->top_k = infer_params.top_k;
            req->engine = engine;

            auto utf8_buf = std::make_shared<std::string>();
            auto reasoning_buf = std::make_shared<std::string>();

            req->on_token = [sse, utf8_buf](const std::string& token) {
                if (!sse->is_active()) return;
                *utf8_buf += token;
                std::string result;
                size_t total = utf8_buf->size();
                size_t i = 0;
                while (i < total) {
                    unsigned char c = static_cast<unsigned char>((*utf8_buf)[i]);
                    size_t len = 1;
                    if ((c & 0x80) == 0)       len = 1;
                    else if ((c & 0xE0) == 0xC0) len = 2;
                    else if ((c & 0xF0) == 0xE0) len = 3;
                    else if ((c & 0xF8) == 0xF0) len = 4;
                    else { utf8_buf->erase(i, 1); total--; continue; }
                    if (i + len > total) break;
                    bool valid = true;
                    for (size_t j = 1; j < len; j++) {
                        if ((static_cast<unsigned char>((*utf8_buf)[i+j]) & 0xC0) != 0x80)
                            { valid = false; break; }
                    }
                    if (!valid) { utf8_buf->erase(i, 1); total--; continue; }
                    result += utf8_buf->substr(i, len);
                    i += len;
                }
                utf8_buf->erase(0, i);
                if (!result.empty()) {
                    nlohmann::json j;
                    j["type"] = "answer";
                    j["content"] = result;
                    sse->send_event("message", j.dump());
                }
            };

            req->on_reasoning = [sse, reasoning_buf](const std::string& token) {
                if (!sse->is_active()) return;
                *reasoning_buf += token;
                nlohmann::json j;
                j["type"] = "reasoning";
                try {
                    j["content"] = *reasoning_buf;
                    sse->send_event("message", j.dump());
                } catch (...) {}
            };

            req->on_done = [sse, utf8_buf, reasoning_buf, message, session_id, bill_tenant, bill_user](const InferResult& result) {
                int total_tokens = result.prompt_tokens + result.completion_tokens;
                if (g_redis && total_tokens > 0) {
                    std::string req_id = generate_uuid();
                    BillingManager::deduct(*g_redis, bill_tenant, bill_user, total_tokens, req_id);
                    auto conn = g_mysql_pool->acquire();
                    if (conn && conn->valid()) {
                        std::string sql = "INSERT INTO billing_records (tenant_id, user_id, tokens_used, request_id) VALUES ("
                            + std::to_string(bill_tenant) + "," + std::to_string(bill_user)
                            + "," + std::to_string(total_tokens) + ",'"
                            + escape_sql(conn->raw(), req_id) + "')";
                        conn->execute(sql);
                        g_mysql_pool->release(conn);
                    }
                }

                nlohmann::json msg;
                msg["session_id"] = session_id;
                msg["role"] = "user";
                msg["content"] = message;
                msg["tokens_in"] = result.prompt_tokens;
                msg["tokens_out"] = result.completion_tokens;
                g_message_buffer.push(msg);

                nlohmann::json assistant_msg;
                assistant_msg["session_id"] = session_id;
                assistant_msg["role"] = "assistant";
                assistant_msg["content"] = result.text;
                assistant_msg["tokens_in"] = 0;
                assistant_msg["tokens_out"] = result.completion_tokens;
                g_message_buffer.push(assistant_msg);

                if (!sse->is_active()) return;
                if (!utf8_buf->empty()) {
                    nlohmann::json j;
                    j["type"] = "answer";
                    j["content"] = *utf8_buf;
                    sse->send_event("message", j.dump());
                    utf8_buf->clear();
                }
                if (!reasoning_buf->empty()) {
                    nlohmann::json j;
                    j["type"] = "reasoning";
                    j["content"] = *reasoning_buf;
                    try { sse->send_event("message", j.dump()); } catch (...) {}
                }

                nlohmann::json j;
                j["type"] = "done";
                j["usage"]["prompt_tokens"] = result.prompt_tokens;
                j["usage"]["completion_tokens"] = result.completion_tokens;
                sse->send_event("message", j.dump());
                sse->close();
            };

            g_scheduler->submit(req);

        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = std::string("Invalid JSON: ") + e.what();
            sse_ctx->send_event("error", err.dump());
            sse_ctx->close();
        }
    });

    // List sessions
    server.router().get("/api/sessions", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;

        auto conn = g_mysql_pool->acquire();
        if (!conn) { resp.json_error(500, "Database error"); return; }

        nlohmann::json result = nlohmann::json::array();
        std::string sql = "SELECT id, title, model, created_at, updated_at FROM sessions WHERE tenant_id="
            + std::to_string(user.tenant_id) + " AND user_id="
            + std::to_string(user.id) + " ORDER BY updated_at DESC LIMIT 50";
        auto* res = conn->query(sql);
        if (res) {
            auto* row = mysql_fetch_row(res);
            while (row) {
                nlohmann::json s;
                s["id"] = row[0] ? row[0] : "";
                s["title"] = row[1] ? row[1] : "New Chat";
                s["model"] = row[2] ? row[2] : "";
                s["created_at"] = row[3] ? row[3] : "";
                s["updated_at"] = row[4] ? row[4] : "";
                result.push_back(s);
                row = mysql_fetch_row(res);
            }
            mysql_free_result(res);
        }
        g_mysql_pool->release(conn);

        nlohmann::json j;
        j["sessions"] = result;
        resp.json(j.dump());
    });

    // Create session
    server.router().post("/api/sessions", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string title = body.value("title", "New Chat");
            std::string model = body.value("model", "qwen3b-q4");
            std::string session_id = generate_uuid();

            auto conn = g_mysql_pool->acquire();
            if (!conn) { resp.json_error(500, "Database error"); return; }

            std::string sql = "INSERT INTO sessions (id, tenant_id, user_id, title, model) VALUES ('"
                + session_id + "',"
                + std::to_string(user.tenant_id) + ","
                + std::to_string(user.id) + ",'"
                + escape_sql(conn->raw(), title) + "','"
                + escape_sql(conn->raw(), model) + "')";

            if (!conn->execute(sql)) {
                g_mysql_pool->release(conn);
                resp.json_error(500, "Failed to create session");
                return;
            }
            g_mysql_pool->release(conn);

            nlohmann::json j;
            j["id"] = session_id;
            j["title"] = title;
            j["model"] = model;
            j["status"] = "created";
            resp.json(j.dump());
        } catch (const std::exception& e) {
            resp.json_error(400, std::string("Invalid request: ") + e.what());
        }
    });

    // Get session detail
    server.router().get("/api/sessions/:id", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;

        std::string session_id = params.get("id");
        if (session_id.empty()) {
            resp.json_error(400, "Session ID required");
            return;
        }

        auto conn = g_mysql_pool->acquire();
        if (!conn) { resp.json_error(500, "Database error"); return; }

        nlohmann::json j;
        std::string sql = "SELECT id, title, model, created_at, updated_at FROM sessions WHERE id='"
            + escape_sql(conn->raw(), session_id) + "' AND tenant_id="
            + std::to_string(user.tenant_id) + " AND user_id="
            + std::to_string(user.id);
        auto* res = conn->query(sql);
        if (res) {
            auto* row = mysql_fetch_row(res);
            if (row) {
                j["id"] = row[0] ? row[0] : "";
                j["title"] = row[1] ? row[1] : "New Chat";
                j["model"] = row[2] ? row[2] : "";
                j["created_at"] = row[3] ? row[3] : "";
                j["updated_at"] = row[4] ? row[4] : "";
            }
            mysql_free_result(res);
        }
        g_mysql_pool->release(conn);

        if (j.empty()) {
            resp.json_error(404, "Session not found");
            return;
        }

        auto conn2 = g_mysql_pool->acquire();
        nlohmann::json msgs = nlohmann::json::array();
        if (conn2) {
            std::string msql = "SELECT id, role, content, reasoning, tokens_in, tokens_out, created_at FROM messages WHERE session_id='"
                + escape_sql(conn2->raw(), session_id) + "' ORDER BY created_at ASC LIMIT 200";
            auto* mres = conn2->query(msql);
            if (mres) {
                auto* row = mysql_fetch_row(mres);
                while (row) {
                    nlohmann::json m;
                    m["id"] = row[0] ? std::stoi(row[0]) : 0;
                    m["role"] = row[1] ? row[1] : "";
                    m["content"] = row[2] ? row[2] : "";
                    m["reasoning"] = row[3] ? row[3] : "";
                    m["tokens_in"] = row[4] ? std::stoi(row[4]) : 0;
                    m["tokens_out"] = row[5] ? std::stoi(row[5]) : 0;
                    m["created_at"] = row[6] ? row[6] : "";
                    msgs.push_back(m);
                    row = mysql_fetch_row(mres);
                }
                mysql_free_result(mres);
            }
            g_mysql_pool->release(conn2);
        }

        j["messages"] = msgs;
        resp.json(j.dump());
    });

    // Delete session
    server.router().del("/api/sessions/:id", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;

        std::string session_id = params.get("id");
        if (session_id.empty()) {
            resp.json_error(400, "Session ID required");
            return;
        }

        auto conn = g_mysql_pool->acquire();
        if (conn) {
            std::string sql = "DELETE FROM sessions WHERE id='"
                + escape_sql(conn->raw(), session_id) + "' AND tenant_id="
                + std::to_string(user.tenant_id) + " AND user_id="
                + std::to_string(user.id);
            conn->execute(sql);
            g_mysql_pool->release(conn);
        }

        resp.json(R"({"status":"deleted"})");
    });

    // Get session messages
    server.router().get("/api/sessions/:id/messages", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;

        std::string session_id = params.get("id");
        if (session_id.empty()) {
            resp.json_error(400, "Session ID required");
            return;
        }

        auto conn = g_mysql_pool->acquire();
        if (!conn) { resp.json_error(500, "Database error"); return; }

        nlohmann::json msgs = nlohmann::json::array();
        std::string sql = "SELECT id, role, content, reasoning, tokens_in, tokens_out, created_at FROM messages WHERE session_id='"
            + escape_sql(conn->raw(), session_id) + "' ORDER BY created_at ASC LIMIT 200";
        auto* res = conn->query(sql);
        if (res) {
            auto* row = mysql_fetch_row(res);
            while (row) {
                nlohmann::json m;
                m["id"] = row[0] ? std::stoi(row[0]) : 0;
                m["role"] = row[1] ? row[1] : "";
                m["content"] = row[2] ? row[2] : "";
                m["reasoning"] = row[3] ? row[3] : "";
                m["tokens_in"] = row[4] ? std::stoi(row[4]) : 0;
                m["tokens_out"] = row[5] ? std::stoi(row[5]) : 0;
                m["created_at"] = row[6] ? row[6] : "";
                msgs.push_back(m);
                row = mysql_fetch_row(res);
            }
            mysql_free_result(res);
        }
        g_mysql_pool->release(conn);

        nlohmann::json j;
        j["messages"] = msgs;
        resp.json(j.dump());
    });

    // Document APIs

    // Upload document
    server.router().post("/api/documents", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        if (g_redis && g_rate_limiter && !g_rate_limiter->allow(user.id, "upload", *g_redis)) {
            resp.json_error(429, "Rate limited"); return;
        }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string filename = body.value("filename", "");
            std::string content = body.value("content", "");
            std::string visibility = body.value("visibility", "group");

            if (filename.empty() || content.empty()) {
                resp.json_error(400, "filename and content required");
                return;
            }

            if (visibility != "group" && visibility != "public") {
                resp.json_error(400, "visibility must be 'group' or 'public'");
                return;
            }

            if (!g_rag || !g_rag->is_ready()) {
                resp.json_error(503, "RAG service not available (no embedding model)");
                return;
            }

            int doc_id = g_rag->add_document(user.tenant_id, filename, content, "", visibility, user.id);

            nlohmann::json j;
            j["id"] = doc_id;
            j["filename"] = filename;
            j["visibility"] = visibility;
            j["status"] = "uploaded";
            resp.json(j.dump());
        } catch (const std::exception& e) {
            resp.json_error(400, std::string("Invalid request: ") + e.what());
        }
    });

    // List documents (own group + public from all groups)
    server.router().get("/api/documents", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;

        auto conn = g_mysql_pool->acquire();
        if (!conn) { resp.json_error(500, "Database error"); return; }

        nlohmann::json result = nlohmann::json::array();
        std::string sql = "SELECT id, tenant_id, filename, chunk_count, visibility, created_at FROM documents WHERE status='ready' AND (tenant_id="
            + std::to_string(user.tenant_id) + " OR visibility='public') ORDER BY created_at DESC";
        auto* res = conn->query(sql);
        if (res) {
            auto* row = mysql_fetch_row(res);
            while (row) {
                nlohmann::json d;
                d["id"] = row[0] ? std::stoi(row[0]) : 0;
                d["tenant_id"] = row[1] ? std::stoi(row[1]) : 0;
                d["filename"] = row[2] ? row[2] : "";
                d["chunk_count"] = row[3] ? std::stoi(row[3]) : 0;
                d["visibility"] = row[4] ? row[4] : "group";
                d["created_at"] = row[5] ? row[5] : "";
                result.push_back(d);
                row = mysql_fetch_row(res);
            }
            mysql_free_result(res);
        }
        g_mysql_pool->release(conn);

        nlohmann::json j;
        j["documents"] = result;
        resp.json(j.dump());
    });

    // Delete document (admin only, own group only)
    server.router().del("/api/documents/:id", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        if (user.role != Role::ADMIN) {
            resp.json_error(403, "Admin only");
            return;
        }

        std::string doc_id_str = params.get("id");
        if (doc_id_str.empty()) {
            resp.json_error(400, "Document ID required");
            return;
        }

        int doc_id = std::stoi(doc_id_str);
        g_rag->remove_document(doc_id);

        resp.json(R"({"status":"deleted"})");
    });

    // Search documents (RAG retrieve: own group + public)
    server.router().post("/api/documents/search", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string query = body.value("query", "");
            int top_k = body.value("top_k", 5);
            std::string mode = body.value("mode", "chunks");

            if (query.empty()) {
                resp.json_error(400, "query required");
                return;
            }

            if (!g_rag || !g_rag->is_ready()) {
                resp.json_error(503, "RAG service not available");
                return;
            }

            nlohmann::json result_arr = nlohmann::json::array();

            if (mode == "parents") {
                auto results = g_rag->retrieve_parents(query, top_k, user.tenant_id);
                for (auto& p : results) {
                    nlohmann::json j;
                    j["doc_id"] = p.doc_id;
                    j["filename"] = p.filename;
                    j["content"] = p.content;
                    j["matched_chunks"] = p.matched_chunks;
                    j["max_score"] = p.max_score;
                    result_arr.push_back(j);
                }
            } else {
                auto results = g_rag->retrieve(query, top_k, user.tenant_id);
                for (auto& r : results) {
                    nlohmann::json j;
                    j["doc_id"] = r.doc_id;
                    j["chunk_index"] = r.chunk_index;
                    j["score"] = r.score;
                    j["content"] = r.content;
                    result_arr.push_back(j);
                }
            }

            nlohmann::json j;
            j["results"] = result_arr;
            resp.json(j.dump());
        } catch (const std::exception& e) {
            resp.json_error(400, std::string("Invalid request: ") + e.what());
        }
    });

    // === Tenant management (admin only) ===

    server.router().post("/api/tenants", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        if (user.role != Role::ADMIN) { resp.json_error(403, "Admin only"); return; }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string name = body.value("name", "");
            int token_limit = body.value("token_limit_daily", 100000);
            if (name.empty()) { resp.json_error(400, "name required"); return; }

            auto conn = g_mysql_pool->acquire();
            if (!conn) { resp.json_error(500, "Database error"); return; }
            std::string sql = "INSERT INTO tenants (name, token_limit_daily) VALUES ('"
                + escape_sql(conn->raw(), name) + "',"
                + std::to_string(token_limit) + ")";
            if (!conn->execute(sql)) {
                g_mysql_pool->release(conn);
                resp.json_error(409, "Tenant name already exists");
                return;
            }
            int tenant_id = mysql_insert_id(conn->raw());
            g_mysql_pool->release(conn);

            nlohmann::json j;
            j["id"] = tenant_id;
            j["name"] = name;
            j["token_limit_daily"] = token_limit;
            j["status"] = "created";
            resp.json(j.dump());
        } catch (const std::exception& e) {
            resp.json_error(400, std::string("Invalid request: ") + e.what());
        }
    });

    server.router().get("/api/tenants", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        if (user.role != Role::ADMIN) { resp.json_error(403, "Admin only"); return; }

        auto conn = g_mysql_pool->acquire();
        if (!conn) { resp.json_error(500, "Database error"); return; }
        nlohmann::json result = nlohmann::json::array();
        auto* res = conn->query("SELECT id, name, token_limit_daily, status, created_at FROM tenants ORDER BY id");
        if (res) {
            auto* row = mysql_fetch_row(res);
            while (row) {
                nlohmann::json t;
                t["id"] = row[0] ? std::stoi(row[0]) : 0;
                t["name"] = row[1] ? row[1] : "";
                t["token_limit_daily"] = row[2] ? std::stoi(row[2]) : 0;
                t["status"] = row[3] ? row[3] : "active";
                t["created_at"] = row[4] ? row[4] : "";
                result.push_back(t);
                row = mysql_fetch_row(res);
            }
            mysql_free_result(res);
        }
        g_mysql_pool->release(conn);
        nlohmann::json j;
        j["tenants"] = result;
        resp.json(j.dump());
    });

    // === User management (admin only) ===

    server.router().post("/api/users", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        if (user.role != Role::ADMIN) { resp.json_error(403, "Admin only"); return; }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");
            int tenant_id = body.value("tenant_id", user.tenant_id);
            std::string role = body.value("role", "user");

            if (username.empty() || password.empty()) {
                resp.json_error(400, "username and password required");
                return;
            }
            if (role != "admin" && role != "user") {
                resp.json_error(400, "role must be 'admin' or 'user'");
                return;
            }

            std::string hash = CryptoUtils::hash_password(password);

            auto conn = g_mysql_pool->acquire();
            if (!conn) { resp.json_error(500, "Database error"); return; }
            std::string sql = "INSERT INTO users (tenant_id, username, password_hash, role) VALUES ("
                + std::to_string(tenant_id) + ",'"
                + escape_sql(conn->raw(), username) + "','"
                + escape_sql(conn->raw(), hash) + "','"
                + escape_sql(conn->raw(), role) + "')";
            if (!conn->execute(sql)) {
                g_mysql_pool->release(conn);
                resp.json_error(409, "Username already exists in this tenant");
                return;
            }
            int user_id = mysql_insert_id(conn->raw());
            g_mysql_pool->release(conn);

            nlohmann::json j;
            j["id"] = user_id;
            j["username"] = username;
            j["tenant_id"] = tenant_id;
            j["role"] = role;
            j["status"] = "created";
            resp.json(j.dump());
        } catch (const std::exception& e) {
            resp.json_error(400, std::string("Invalid request: ") + e.what());
        }
    });

    server.router().get("/api/users", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        if (user.role != Role::ADMIN) { resp.json_error(403, "Admin only"); return; }

        auto conn = g_mysql_pool->acquire();
        if (!conn) { resp.json_error(500, "Database error"); return; }
        nlohmann::json result = nlohmann::json::array();
        std::string sql = "SELECT id, tenant_id, username, role, created_at FROM users ORDER BY id";
        auto* res = conn->query(sql);
        if (res) {
            auto* row = mysql_fetch_row(res);
            while (row) {
                nlohmann::json u;
                u["id"] = row[0] ? std::stoi(row[0]) : 0;
                u["tenant_id"] = row[1] ? std::stoi(row[1]) : 0;
                u["username"] = row[2] ? row[2] : "";
                u["role"] = row[3] ? row[3] : "user";
                u["created_at"] = row[4] ? row[4] : "";
                result.push_back(u);
                row = mysql_fetch_row(res);
            }
            mysql_free_result(res);
        }
        g_mysql_pool->release(conn);
        nlohmann::json j;
        j["users"] = result;
        resp.json(j.dump());
    });

    server.router().del("/api/users/:id", [](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        if (user.role != Role::ADMIN) { resp.json_error(403, "Admin only"); return; }

        std::string id_str = params.get("id");
        if (id_str.empty()) { resp.json_error(400, "User ID required"); return; }
        int target_id = std::stoi(id_str);
        if (target_id == user.id) { resp.json_error(400, "Cannot delete yourself"); return; }

        auto conn = g_mysql_pool->acquire();
        if (conn) {
            std::string sql = "DELETE FROM users WHERE id=" + id_str;
            conn->execute(sql);
            g_mysql_pool->release(conn);
        }
        resp.json(R"({"status":"deleted"})");
    });

    spdlog::info("Starting fast-llm-reasoner on port {}", port);
    server.start();
    loop.loop();

    return 0;
}
