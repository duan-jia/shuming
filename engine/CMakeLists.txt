# Check for libcurl
find_package(OpenSSL REQUIRED)

add_executable(fast-llm-reasoner
    src/agent/agent_runtime.cpp
    src/main.cpp
    src/mcp/mcp_service.cpp
    src/server/worker_pool.cpp
    src/services/knowledge_fetch_service.cpp
    src/services/tool_service.cpp
    src/services/web_search_service.cpp
    src/server/http_server.cpp
    src/server/http_protocol.cpp
    src/server/router.cpp
    src/server/sse_connection.cpp
    src/app/app_bootstrap.cpp
    src/persistence/mysql_pool.cpp
    src/persistence/billing_service.cpp
    src/persistence/document_repository.cpp
    src/persistence/message_repository.cpp
    src/persistence/rag_document_repository.cpp
    src/persistence/session_repository.cpp
    src/persistence/tenant_repository.cpp
    src/persistence/token_usage_repository.cpp
    src/persistence/user_repository.cpp
    src/services/admin_service.cpp
    src/services/auth_service.cpp
    src/services/chat_service.cpp
    src/services/document_service.cpp
    src/services/knowledge_search_service.cpp
    src/services/rag_knowledge_search_backend.cpp
    src/services/rag_prompt_formatter.cpp
    src/services/session_service.cpp
    src/routes/admin_routes.cpp
    src/routes/auth_routes.cpp
    src/routes/chat_routes.cpp
    src/routes/document_routes.cpp
    src/routes/session_routes.cpp
    src/auth/jwt_handler.cpp
    src/security/rbac_manager.cpp
    src/security/audit_logger.cpp
    src/security/tenant_isolator.cpp
    src/security/crypto_utils.cpp
    src/security/rate_limiter.cpp
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
    src/rag/query_rewriter.cpp
)

target_include_directories(fast-llm-reasoner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(fast-llm-reasoner PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    jwt-cpp::jwt-cpp
    mysqlclient
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
    src/persistence/mysql_pool.cpp
    src/persistence/rag_document_repository.cpp
    src/inference/llm_engine.cpp
    src/inference/model_manager.cpp
    src/inference/reasoning_parser.cpp
    src/rag/chunker.cpp
    src/rag/embedder.cpp
    src/rag/vector_index.cpp
    src/rag/bm25_index.cpp
    src/rag/document_store.cpp
    src/rag/query_rewriter.cpp
)

target_include_directories(fast-llm-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(fast-llm-cli PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    mysqlclient
    llama
    faiss
    gfortran
    pthread
    httplib::httplib
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_compile_definitions(fast-llm-cli PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

option(FASTLLM_BUILD_TESTS "Build focused backend unit tests" ON)
if(FASTLLM_BUILD_TESTS)
    enable_testing()
    add_executable(knowledge-search-service-test
        tests/knowledge_search_service_test.cpp
        src/services/knowledge_search_service.cpp
        src/services/rag_prompt_formatter.cpp
    )
    target_include_directories(knowledge-search-service-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME knowledge-search-service-test COMMAND knowledge-search-service-test)

    # Uses an isolated MySQL server supplied through MYSQL_TEST_* variables.
    add_executable(mysql-pool-test
        tests/mysql_pool_test.cpp
        src/persistence/mysql_pool.cpp
    )
    target_include_directories(mysql-pool-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(mysql-pool-test PRIVATE spdlog::spdlog mysqlclient pthread)
    add_test(NAME mysql-pool-test COMMAND mysql-pool-test)
    set_tests_properties(mysql-pool-test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

if(FASTLLM_BUILD_TESTS)
    add_executable(native-mcp-test-server
        tests/native_mcp_server.cpp
        src/agent/agent_runtime.cpp
        src/inference/remote_engine.cpp
        src/mcp/mcp_service.cpp
        src/services/knowledge_fetch_service.cpp
        src/services/tool_service.cpp
        src/services/web_search_service.cpp
        src/services/knowledge_search_service.cpp
        src/auth/jwt_handler.cpp
        src/security/rate_limiter.cpp
        src/server/http_server.cpp
        src/server/http_protocol.cpp
        src/server/router.cpp
        src/server/sse_connection.cpp
        src/server/worker_pool.cpp
    )
    target_include_directories(native-mcp-test-server PRIVATE include ${muduo_SOURCE_DIR})
    target_link_libraries(native-mcp-test-server PRIVATE
        nlohmann_json::nlohmann_json spdlog::spdlog jwt-cpp::jwt-cpp
        httplib::httplib muduo_net muduo_base OpenSSL::SSL OpenSSL::Crypto pthread)
    target_compile_definitions(native-mcp-test-server PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
endif()

if(FASTLLM_BUILD_TESTS)
    add_executable(auth-security-test
        tests/auth_security_test.cpp
        src/security/crypto_utils.cpp
        src/security/rbac_manager.cpp
        src/auth/jwt_handler.cpp
    )
    target_include_directories(auth-security-test PRIVATE include)
    target_link_libraries(auth-security-test PRIVATE
        spdlog::spdlog jwt-cpp::jwt-cpp OpenSSL::Crypto OpenSSL::SSL pthread)
    # These tests deliberately use assertions, including in Release CI.
    target_compile_options(auth-security-test PRIVATE -UNDEBUG)
    add_test(NAME auth-security-test COMMAND auth-security-test)
endif()

if(FASTLLM_BUILD_TESTS)
    add_executable(auth-service-test
        tests/auth_service_test.cpp
        src/services/auth_service.cpp
        src/security/crypto_utils.cpp
        src/security/rbac_manager.cpp
        src/auth/jwt_handler.cpp
        src/server/http_protocol.cpp
    )
    target_include_directories(auth-service-test PRIVATE include ${faiss_SOURCE_DIR})
    target_link_libraries(auth-service-test PRIVATE
        nlohmann_json::nlohmann_json spdlog::spdlog jwt-cpp::jwt-cpp
        OpenSSL::Crypto OpenSSL::SSL pthread)
    target_compile_options(auth-service-test PRIVATE -UNDEBUG)
    add_test(NAME auth-service-test COMMAND auth-service-test)
endif()
