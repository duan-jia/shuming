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
    LoginLimiter guests;
    for (int n = 0; n < 8; ++n) assert(guests.allow_guest_login("peer", now));
    assert(!guests.allow_guest_login("peer", now));
    for (int n = 0; n < 5; ++n) assert(guests.allow_guest_chat(1, "peer", now));
    assert(!guests.allow_guest_chat(1, "peer", now));
    assert(guest_route_allowed("POST", "/api/chat/stream"));
    assert(guest_route_allowed("DELETE", "/api/sessions/own"));
    assert(!guest_route_allowed("POST", "/api/documents"));
    assert(!guest_route_allowed("POST", "/mcp"));
    assert(!guest_route_allowed("GET", "/api/users"));
    assert(!guest_route_allowed("POST", "/api/documents/search"));
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
    RbacManager rbac;
    assert(RbacManager::string_to_role("unknown") == Role::UNKNOWN);
    assert(!rbac.check_permission(Role::GUEST, "kb", "write"));
    std::cout << "auth security unit tests passed\n";
}
