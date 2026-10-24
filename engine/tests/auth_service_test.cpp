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
    ctx->rbac = std::make_unique<RbacManager>();
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
    for (const auto& path : {"/api/users", "/api/documents", "/mcp"}) {
        resource.path = path; resource.method = HttpMethod::GET;
        HttpResponse denied; assert(!auth.require_auth(resource, denied, user)); assert(denied.status_code == 403);
    }
    resource.path = "/api/sessions"; resource.method = HttpMethod::GET;
    HttpResponse allowed; assert(auth.require_auth(resource, allowed, user));
    ctx->guest_enabled = false;
    HttpResponse revoked; assert(!auth.require_auth(resource, revoked, user));
    std::cout << "auth service fixture tests passed\n";
}
