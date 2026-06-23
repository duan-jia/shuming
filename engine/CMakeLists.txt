# Check for libcurl
find_package(OpenSSL REQUIRED)

add_executable(fast-llm-reasoner
    src/main.cpp
    src/server/http_server.cpp
    src/server/http_protocol.cpp
    src/server/router.cpp
    src/server/sse_connection.cpp
    src/persistence/mysql_pool.cpp
    src/persistence/redis_manager.cpp
    src/auth/jwt_handler.cpp
    src/security/rbac_manager.cpp
    src/security/audit_logger.cpp
    src/security/tenant_isolator.cpp
    src/security/crypto_utils.cpp
    src/security/rate_limiter.cpp
    src/persistence/billing.cpp
    src/inference/llm_engine.cpp
    src/inference/reasoning_parser.cpp
    src/inference/model_manager.cpp
    src/inference/remote_engine.cpp
    src/inference/kv_cache_pool.cpp
    src/inference/prefix_cache.cpp
    src/inference/batch_scheduler.cpp
    src/rag/chunker.cpp
    src/rag/embedder.cpp
    src/rag/vector_index.cpp
    src/rag/bm25_index.cpp
    src/rag/document_store.cpp
    src/rag/query_filter.cpp
)

target_include_directories(fast-llm-reasoner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(fast-llm-reasoner PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    jwt-cpp::jwt-cpp
    mysqlclient
    hiredis
    llama
    faiss
    gfortran
    pthread
    httplib::httplib
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_compile_definitions(fast-llm-reasoner PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

if(muduo_SOURCE_DIR)
    target_include_directories(fast-llm-reasoner PRIVATE
        ${muduo_SOURCE_DIR}
    )
    target_link_libraries(fast-llm-reasoner PRIVATE
        muduo_net
        muduo_base
    )
endif()

# CLI chat tool (no HTTP, no auth, no DB — testing only)
add_executable(fast-llm-cli
    src/cli.cpp
    src/inference/llm_engine.cpp
    src/inference/model_manager.cpp
    src/inference/reasoning_parser.cpp
    src/rag/chunker.cpp
    src/rag/embedder.cpp
    src/rag/vector_index.cpp
    src/rag/bm25_index.cpp
    src/rag/document_store.cpp
    src/rag/query_filter.cpp
)

target_include_directories(fast-llm-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(fast-llm-cli PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    llama
    faiss
    gfortran
    pthread
)
