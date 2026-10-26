// In-memory repository fixture: executes the production AuthService without a
// live database. SQL/status-join integration is a separate deployment check.
#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>
#include "services/auth_service.h"
#include "security/crypto_utils.h"
using namespace fastllm;
namespace {
LoginUser fixture;
UserLookupStatus lookup = UserLookupStatus::Found;
int next_guest = 100;
}
namespace fastllm {
// Only repository boundaries are replaced; no HTTP/authentication code is mocked.
MysqlPool::MysqlPool(const std::string&, int, const std::string&, const std::string&, const std::string&, size_t) {}
UserRepository::UserRepository(MysqlPool& mysql) : mysql_(mysql) {}
UserLookupStatus UserRepository::find_login_user(const std::string&, LoginUser& user) { user = fixture; return lookup; }
bool UserRepository::create_guest_user(int tenant, const std::string& name, int& id) {
    assert(tenant == 2); assert(name.rfind("guest_", 0) == 0); id = ++next_guest; return true;
}
}
int main() {
    // Process-lifetime fixture: unrelated AppContext subsystems are never started.
    auto* ctx = new AppContext;
    auto* pool = new MysqlPool("", 0, "", "", "", 1);
    ctx->jwt = std::make_unique<JwtHandler>(std::string(48, 'x'));
    ctx->user_repository = std::make_unique<UserRepository>(*pool);
    fixture.id = 1; fixture.tenant_id = 1; fixture.username = "Admin"; fixture.role = "admin";
    fixture.password_hash = CryptoUtils::hash_password("correct-password"); fixture.active = true;
    AuthService auth(*ctx);
    HttpRequest req; req.method = HttpMethod::POST; req.path = "/api/auth/login"; req.peer_ip = "test-peer";
    req.body = R"({"username":"Admin","password":"correct-password"})";
    HttpResponse good; auth.login(req, good); assert(good.status_code == 200);
    assert(good.headers.at("Cache-Control") == "no-store");
    auto token = nlohmann::json::parse(good.body).at("token").get<std::string>();
    assert(ctx->jwt->verify_token(token).valid);
    fixture.active = false;
    HttpResponse disabled; auth.login(req, disabled); assert(disabled.status_code == 401);
    fixture.active = true; lookup = UserLookupStatus::NotFound;
    HttpResponse missing; auth.login(req, missing); assert(missing.status_code == 401);
    assert(disabled.body == missing.body);
    lookup = UserLookupStatus::Found;
    req.body = R"({"username":[],"password":true})";
    HttpResponse typed; auth.login(req, typed); assert(typed.status_code == 400);
    req.body = std::string(4097, 'x');
    HttpResponse huge; auth.login(req, huge); assert(huge.status_code == 400);
    AuthService limited(*ctx);
    req.body = R"({"username":"admin","password":"wrong-password"})";
    for (int n = 0; n < 10; ++n) { HttpResponse r; limited.login(req, r); assert(r.status_code == 401); }
    req.body = R"({"username":"ADMIN","password":"correct-password"})";
    HttpResponse throttled; limited.login(req, throttled); assert(throttled.status_code == 429);
    assert(throttled.headers.count("Retry-After"));
    HttpRequest guest_req; guest_req.method = HttpMethod::POST; guest_req.path = "/api/auth/guest";
    guest_req.body = "{}"; guest_req.peer_ip = "guest-peer";
    HttpResponse off; auth.guest_login(guest_req, off); assert(off.status_code == 403);
    ctx->guest_enabled = true; ctx->guest_tenant_id = 2;
    HttpResponse one, two; auth.guest_login(guest_req, one); auth.guest_login(guest_req, two);
    assert(one.status_code == 200 && two.status_code == 200);
    auto a = nlohmann::json::parse(one.body); auto b = nlohmann::json::parse(two.body);
    assert(a["user"]["id"] != b["user"]["id"]);
    assert(a["user"]["username"] != b["user"]["username"]);
    auto guest_token = a["token"].get<std::string>();
    assert(ctx->jwt->verify_token(guest_token).role == "guest");
    guest_req.body = R"({"role":"admin","tenant_id":1})";
    HttpResponse spoof; auth.guest_login(guest_req, spoof); assert(spoof.status_code == 400);
    HttpRequest resource; resource.headers["Authorization"] = "Bearer " + guest_token;
    UserInfo user;
    for (const auto& path : {"/api/users", "/api/tenants"}) {
        resource.path = path; resource.method = HttpMethod::GET;
        HttpResponse denied; assert(!auth.require_auth(resource, denied, user)); assert(denied.status_code == 403);
    }
    resource.path = "/api/sessions"; resource.method = HttpMethod::GET;
    HttpResponse allowed; assert(auth.require_auth(resource, allowed, user));
    // Verify signed identities through the production authorization boundary.
    const auto member_token = ctx->jwt->generate_token(1, 1, "member", "user");
    resource.headers["Authorization"] = "Bearer " + member_token;
    resource.body = R"({"role":"admin","tenant_id":2})";
    for (const auto& path : {"/api/users", "/api/tenants", "/api/documents/1/approve"}) {
        resource.path = path; resource.method = HttpMethod::POST;
        HttpResponse denied; assert(!auth.require_auth(resource, denied, user)); assert(denied.status_code == 403);
    }
    resource.path = "/api/documents"; resource.method = HttpMethod::POST;
    HttpResponse upload; assert(auth.require_auth(resource, upload, user));
    assert(user.role == Role::USER && user.tenant_id == 1);
    resource.headers["Authorization"] = "Bearer " + token;
    resource.path = "/api/users"; resource.method = HttpMethod::GET;
    HttpResponse administration; assert(auth.require_auth(resource, administration, user));
    resource.path = "/api/new-unregistered-endpoint";
    HttpResponse future; assert(!auth.require_auth(resource, future, user)); assert(future.status_code == 403);
    resource.headers.erase("Authorization");
    resource.path = "/api/sessions";
    HttpResponse anonymous; assert(!auth.require_auth(resource, anonymous, user)); assert(anonymous.status_code == 401);
    resource.headers["Authorization"] = "Bearer " + ctx->jwt->generate_token(101, 1, "wrong-tenant", "guest");
    HttpResponse isolation; assert(!auth.require_auth(resource, isolation, user)); assert(isolation.status_code == 403);
    resource.headers["Authorization"] = "Bearer " + guest_token;
    resource.path = "/api/chat/stream"; resource.method = HttpMethod::POST; resource.peer_ip = "guest-chat-peer";
    for (int n = 0; n < 20; ++n) { HttpResponse r; assert(auth.require_auth(resource, r, user)); }
    resource.path = "/api/documents"; resource.method = HttpMethod::GET;
    HttpResponse documents; assert(auth.require_auth(resource, documents, user));
    resource.path = "/mcp"; resource.method = HttpMethod::POST;
    HttpResponse mcp; assert(auth.require_auth(resource, mcp, user));
    resource.path = "/api/documents";
    HttpResponse guest_upload; assert(auth.require_auth(resource, guest_upload, user));
    resource.path = "/api/chat/cancel";
    HttpResponse cancel; assert(auth.require_auth(resource, cancel, user));
    ctx->guest_enabled = false;
    HttpResponse revoked; assert(!auth.require_auth(resource, revoked, user)); assert(revoked.status_code == 403);
    std::cout << "auth service fixture tests passed\n";
}
