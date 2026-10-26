#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <jwt-cpp/jwt.h>
#include "auth/jwt_handler.h"
#include "security/crypto_utils.h"
#include "security/login_limiter.h"
#include "security/guest_policy.h"
#include "security/rbac_manager.h"

using namespace fastllm;
int main() {
    LoginLimiter limit;
    auto now = LoginLimiter::Clock::now();
    for (int n = 0; n < 10; ++n) assert(limit.allow_account("canonical-id:1", now));
    assert(!limit.allow_account("canonical-id:1", now));
    assert(limit.allow_account("canonical-id:2", now));
    assert(limit.allow_account("canonical-id:1", now + std::chrono::seconds(601)));
    LoginLimiter ips;
    for (int n = 0; n < 60; ++n) assert(ips.allow_source("127.0.0.1", now));
    assert(!ips.allow_source("127.0.0.1", now));
    assert(ips.allow_source("127.0.0.2", now));
    LoginLimiter concurrent;
    std::atomic<int> admitted{0};
    std::vector<std::thread> threads;
    for (int n = 0; n < 40; ++n) threads.emplace_back([&] { if (concurrent.allow_account("same", now)) ++admitted; });
    for (auto& thread : threads) thread.join();
    assert(admitted == 10);
    assert(guest_route_allowed("POST", "/api/chat/stream"));
    assert(guest_route_allowed("DELETE", "/api/sessions/own"));
    assert(guest_route_allowed("POST", "/api/documents"));
    assert(guest_route_allowed("POST", "/mcp"));
    assert(!guest_route_allowed("GET", "/api/users"));
    assert(guest_route_allowed("POST", "/api/documents/search"));
    const auto hash = CryptoUtils::hash_password("correct-password");
    assert(CryptoUtils::verify_password("correct-password", hash));
    assert(!CryptoUtils::verify_password("wrong-password", hash));
    auto split = hash.rfind('$');
    auto expected = CryptoUtils::base64_decode(hash.substr(split + 1));
    // Previously accepted a matching prefix instead of a full 32-byte digest.
    assert(!CryptoUtils::verify_password("correct-password", hash.substr(0, split + 1) + CryptoUtils::base64_encode(expected.data(), 1)));
    assert(!CryptoUtils::verify_password("correct-password", hash + "garbage"));
    assert(!CryptoUtils::verify_password("correct-password", "pbkdf2$999999999$AA==$AA=="));
    const std::string secret(48, 'x');
    JwtHandler jwt(secret);
    auto guest = jwt.generate_token(3, 2, "guest_test", "guest");
    assert(jwt.verify_token(guest).valid);
    const auto decoded = jwt::decode(guest);
    assert(decoded.get_expires_at() - decoded.get_issued_at() <= std::chrono::seconds(900));
    assert(!jwt.verify_token(guest + "broken").valid);
    const auto expired_guest = jwt::create().set_issuer("fast-llm-reasoner")
        .set_issued_at(std::chrono::system_clock::now() - std::chrono::seconds(901))
        .set_expires_at(std::chrono::system_clock::now() - std::chrono::seconds(1))
        .set_payload_claim("user_id", jwt::claim(std::string("3")))
        .set_payload_claim("tenant_id", jwt::claim(std::string("2")))
        .set_payload_claim("username", jwt::claim(std::string("guest_test")))
        .set_payload_claim("role", jwt::claim(std::string("guest")))
        .sign(jwt::algorithm::hs256{secret});
    assert(!jwt.verify_token(expired_guest).valid);
    auto signed_token = [&](const std::string& id, const std::string& role, bool times) {
        auto token = jwt::create().set_issuer("fast-llm-reasoner")
            .set_payload_claim("user_id", jwt::claim(id))
            .set_payload_claim("tenant_id", jwt::claim(std::string("2")))
            .set_payload_claim("username", jwt::claim(std::string("test")))
            .set_payload_claim("role", jwt::claim(role));
        if (times) token.set_issued_at(std::chrono::system_clock::now()).set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds(60));
        return token.sign(jwt::algorithm::hs256{secret});
    };
    assert(!jwt.verify_token(signed_token("3junk", "user", true)).valid);
    assert(!jwt.verify_token(signed_token("3", "unknown", true)).valid);
    assert(!jwt.verify_token(signed_token("3", "admin", false)).valid);
    assert(RbacManager::string_to_role("unknown") == Role::UNKNOWN);
    // Role matrix: capabilities, route admission and object ownership are
    // independent checks. Neither an admin wildcard nor a new enum may bypass it.
    for (Role role : {Role::ADMIN, Role::USER, Role::GUEST, Role::UNKNOWN}) {
        const bool known = role != Role::UNKNOWN;
        const bool member = known;
        const bool admin = role == Role::ADMIN;
        for (auto permission : {Permission::CHAT_WRITE, Permission::SESSION_READ,
                Permission::SESSION_CREATE, Permission::SESSION_DELETE})
            assert(RbacManager::check_permission(role, permission) == known);
        for (auto permission : {Permission::DOCUMENT_READ, Permission::DOCUMENT_UPLOAD,
                Permission::DOCUMENT_DELETE, Permission::KNOWLEDGE_SEARCH,
                Permission::MCP_ACCESS, Permission::WEB_USE})
            assert(RbacManager::check_permission(role, permission) == member);
        for (auto permission : {Permission::DOCUMENT_APPROVE, Permission::PUBLIC_LIBRARY_WRITE, Permission::USER_MANAGE,
                Permission::TENANT_MANAGE})
            assert(RbacManager::check_permission(role, permission) == admin);
        assert(!RbacManager::check_permission(role, static_cast<Permission>(999)));
        assert(!RbacManager::check_route_permission(role, "GET", "/api/future"));
        assert(!RbacManager::check_route_permission(role, "POST", "/api/sessions/id/future"));
        assert(!RbacManager::check_route_permission(role, "DELETE", "/api/sessions/id/messages"));
        assert(!RbacManager::check_route_permission(role, "GET", "/api/sessions/"));
        assert(!RbacManager::check_route_permission(role, "POST", "/api/documents/1/approve/future"));
        assert(!RbacManager::check_route_permission(role, "POST", "/api/users/1"));
        assert(RbacManager::can_use_web(role, false) == admin);
        assert(RbacManager::can_use_web(role, true) == member);
        UserInfo identity{1, 7, "test", role};
        assert(RbacManager::can_upload_document(identity, "group") == member);
        assert(RbacManager::can_upload_document(identity, "public") == admin);
        assert(!RbacManager::can_upload_document(identity, "unknown"));
        assert(RbacManager::can_delete_document(identity, 7, "group") == member);
        assert(!RbacManager::can_delete_document(identity, 8, "group"));
        assert(RbacManager::can_delete_document(identity, 7, "public") == admin);
        assert(RbacManager::can_delete_document(identity, 8, "public") == admin);
        assert(!RbacManager::can_delete_document(identity, 7, "unknown"));
        assert(RbacManager::can_approve_document(identity, 7, "group") == admin);
        assert(!RbacManager::can_approve_document(identity, 8, "group"));
        assert(RbacManager::can_approve_document(identity, 8, "public") == admin);
    }
    // Exercise every protected route class for all roles.
    struct RouteCase { const char* method; const char* path; bool user; bool guest; };
    for (const auto& route : std::vector<RouteCase>{
            {"POST", "/api/chat/stream", true, true}, {"POST", "/api/chat/cancel", true, true},
            {"GET", "/api/sessions", true, true}, {"POST", "/api/sessions", true, true},
            {"GET", "/api/sessions/id", true, true}, {"DELETE", "/api/sessions/id", true, true},
            {"GET", "/api/sessions/id/messages", true, true},
            {"GET", "/api/documents", true, true}, {"POST", "/api/documents", true, true},
            {"GET", "/api/documents/1", true, true}, {"DELETE", "/api/documents/1", true, true},
            {"POST", "/api/documents/search", true, true},
            {"POST", "/api/documents/deduplicate", true, true},
            {"POST", "/api/documents/1/approve", false, false},
            {"GET", "/api/users", false, false}, {"POST", "/api/users", false, false},
            {"DELETE", "/api/users/1", false, false},
            {"GET", "/api/tenants", false, false}, {"POST", "/api/tenants", false, false},
            {"POST", "/mcp", true, true}, {"POST", "/mcp/knowledge", true, true},
            {"POST", "/mcp/web", true, true}}) {
        assert(RbacManager::check_route_permission(Role::ADMIN, route.method, route.path));
        assert(RbacManager::check_route_permission(Role::USER, route.method, route.path) == route.user);
        assert(RbacManager::check_route_permission(Role::GUEST, route.method, route.path) == route.guest);
        assert(!RbacManager::check_route_permission(Role::UNKNOWN, route.method, route.path));
    }
    std::cout << "auth security unit tests passed\n";
}
