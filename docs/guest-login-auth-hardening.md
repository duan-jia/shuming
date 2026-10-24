# Guest login and authentication hardening — DRAFT

Guest access is opt-in and disabled by default. Local merge validation covers full builds,
authentication tests, browser acceptance and isolated MySQL migration/login/session checks.
Real-index public/private retrieval, real-model billing and production proxy acceptance remain
outstanding. Keep guest access disabled until those deployment checks are complete.

## Implemented intent

- Opt-in `auth.guest_enabled` (default false); `/api/auth/config` controls login-page visibility.
- Each guest gets a distinct database user and at-most-15-minute JWT; no shared histories/account.
- Reserved guest tenant; guest role rejects password login. Guest capability allowlist permits owned sessions/chat, denies documents export/upload/management, admin and MCP.
- Guest chat skips the tool agent and uses public-only RAG retrieval, a public-demo system prompt, a 2048-byte question limit and at most 512 output tokens.
- In-memory rate limits for guest registration, chat and session creation. These are per-process, reset on restart, and are **not** a durable or distributed billing/spending cap.
- Password login checks user and tenant active status. Account identity is canonicalized by the database before account throttling; source limits use transport peer only, not spoofable forwarding headers.
- Strict login fields/size limits, generic credential errors, dummy password verification for missing accounts, no-store responses and Retry-After.
- Full-length PBKDF2 digest checks, bounded iterations, required JWT times/identity/known roles. Unknown role no longer falls back to USER.
- Startup rejects documented JWT-secret placeholders/short secrets; new admin bootstrap requires a non-default password of at least 12 bytes. Bootstrap passwords are no longer logged.

## Compatibility and deployment blockers

1. Apply `sql/migrate_guest_login.sql` before explicitly enabling guests on an existing database. No migration has been run against production.
2. Set a non-placeholder `JWT_SECRET` of at least 32 bytes (or `jwt.secret`). Set an adequate `admin.default_password` for a fresh database. Existing example placeholder config is intentionally rejected.
3. When Next/reverse proxies connect to the backend, source-IP counters aggregate users under the proxy peer; forwarding headers are deliberately not trusted. Trusted-proxy handling and gateway-level limits need an explicit deployment design.
4. Guest account cleanup is documented as SQL but is not scheduled or enabled. Review retention and cascading deletion before running it.
5. Disabling an account blocks **new password login**, not every previously issued bearer token. Immediate token revocation is not implemented.
6. Limits reduce abuse but do not guarantee protection against distributed attacks. WAF/rate-limit storage, TLS, monitoring and spending limits are deployment responsibilities.

## Verification recorded before handoff

- Frontend ESLint and Next production build passed during the implementation session.
- Changed backend sources passed a syntax check at an intermediate point; full final server build is not verified.
- `auth-security-test` and production AuthService with in-memory repository fixture passed when compiled directly. Fixtures do not validate MySQL SQL/migration behavior.
- Both test executables are included in CMake. Existing CI has not been updated to run these new targets; that wiring is a remaining task.
- Browser acceptance, guest-vs-member history isolation with real storage, public-only retrieval against real indexes, login cooldown interaction and real-model quotas still need verification.

## Merge validation — 2026-10-24

- CPU Release server, CLI, native MCP fixture and both authentication test targets built successfully.
  The authentication and knowledge-search tests passed. CI now builds and runs both authentication targets.
- Agent/MCP HTTP suite: 29 passed, one official SDK test skipped because no SDK command was supplied.
- Frontend ESLint, production build, SSE/title tests and session-navigation browser regression passed.
- An isolated MySQL 8.4 instance verified migration from the old role enum, password login,
  disabled user/tenant checks, separate guest identities, cross-user session denial and blocked
  document/admin/MCP access. Migration preserved the maximum daily quota and accumulated usage.
- The merged frontend passed guest login against that isolated backend, deferred session creation,
  first-question titles, desktop/mobile layout, logout and 429 cooldown checks. Model streams were
  mocked; these checks made no paid model calls. Temporary test containers were removed afterward.
- Existing local/production databases were not migrated, and their guest configuration was not enabled.

## Test commands

```sh
cmake -S . -B build -DGGML_CUDA=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --target auth-security-test auth-service-test knowledge-search-service-test fast-llm-reasoner -j2
ctest --test-dir build/engine --output-on-failure
cd web && npm ci && npm run lint && npm run build
```

Reference: [OWASP Authentication Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Authentication_Cheat_Sheet.html).
