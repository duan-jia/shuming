-- Migration: replace legacy billing records with user daily token quota tables.
-- Run this against an existing llm_platform database before starting the updated server.

USE llm_platform;

CREATE TABLE IF NOT EXISTS user_token_usage_daily (
    id                BIGINT PRIMARY KEY AUTO_INCREMENT,
    tenant_id         INT NOT NULL,
    user_id           INT NOT NULL,
    usage_date        DATE NOT NULL,
    token_limit       INT NOT NULL,
    tokens_used       INT NOT NULL DEFAULT 0,
    created_at        DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at        DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_user_date (user_id, usage_date),
    INDEX idx_tenant_date (tenant_id, usage_date),
    FOREIGN KEY (tenant_id) REFERENCES tenants(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS token_usage_records (
    id                BIGINT PRIMARY KEY AUTO_INCREMENT,
    tenant_id         INT NOT NULL,
    user_id           INT NOT NULL,
    session_id        VARCHAR(36),
    request_id        VARCHAR(64) NOT NULL,
    usage_date        DATE NOT NULL,
    prompt_tokens     INT NOT NULL DEFAULT 0,
    completion_tokens INT NOT NULL DEFAULT 0,
    total_tokens      INT NOT NULL DEFAULT 0,
    status            ENUM('pending','success','failed') DEFAULT 'pending',
    failure_reason    VARCHAR(128),
    created_at        DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_request_id (request_id),
    INDEX idx_user_date (user_id, usage_date),
    INDEX idx_tenant_date (tenant_id, usage_date),
    INDEX idx_session (session_id),
    FOREIGN KEY (tenant_id) REFERENCES tenants(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

INSERT IGNORE INTO token_usage_records
    (tenant_id, user_id, session_id, request_id, usage_date,
     prompt_tokens, completion_tokens, total_tokens, status, failure_reason, created_at)
SELECT
    tenant_id,
    user_id,
    session_id,
    request_id,
    DATE(created_at),
    0,
    tokens_used,
    tokens_used,
    'success',
    NULL,
    created_at
FROM billing_records
WHERE request_id IS NOT NULL AND request_id <> '';

INSERT INTO user_token_usage_daily
    (tenant_id, user_id, usage_date, token_limit, tokens_used)
SELECT
    b.tenant_id,
    b.user_id,
    DATE(b.created_at),
    t.token_limit_daily,
    SUM(b.tokens_used)
FROM billing_records b
JOIN tenants t ON t.id = b.tenant_id
GROUP BY b.tenant_id, b.user_id, DATE(b.created_at), t.token_limit_daily
ON DUPLICATE KEY UPDATE
    token_limit = VALUES(token_limit),
    tokens_used = GREATEST(tokens_used, VALUES(tokens_used));
