-- Apply once to an existing database before running the updated backend.
-- NULL inherits the tenant default; positive values override it per user.
-- Existing daily usage and limits are preserved.
USE llm_platform;

ALTER TABLE tenants MODIFY token_limit_daily BIGINT DEFAULT 100000;
ALTER TABLE users ADD COLUMN daily_token_quota BIGINT NULL DEFAULT NULL AFTER password_hash;
ALTER TABLE user_token_usage_daily
    MODIFY token_limit BIGINT NOT NULL,
    MODIFY tokens_used BIGINT NOT NULL DEFAULT 0;
