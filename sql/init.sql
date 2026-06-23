CREATE DATABASE IF NOT EXISTS llm_platform DEFAULT CHARSET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE llm_platform;

-- ========== 1. 研究组（租户） ==========
CREATE TABLE IF NOT EXISTS tenants (
    id                INT PRIMARY KEY AUTO_INCREMENT,
    name              VARCHAR(64) NOT NULL UNIQUE,
    token_limit_daily INT DEFAULT 100000,
    status            ENUM('active','inactive') DEFAULT 'active',
    created_at        DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- ========== 2. 用户 ==========
CREATE TABLE IF NOT EXISTS users (
    id                INT PRIMARY KEY AUTO_INCREMENT,
    tenant_id         INT NOT NULL,
    username          VARCHAR(64) NOT NULL,
    password_hash     VARCHAR(256) NOT NULL,
    role              ENUM('admin','user') DEFAULT 'user',
    status            ENUM('active','disabled') DEFAULT 'active',
    created_at        DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at        DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    FOREIGN KEY (tenant_id) REFERENCES tenants(id) ON DELETE CASCADE,
    UNIQUE KEY uk_tenant_user (tenant_id, username),
    INDEX idx_status (status)
);

-- ========== 3. 对话会话 ==========
CREATE TABLE IF NOT EXISTS sessions (
    id                VARCHAR(36) PRIMARY KEY,
    tenant_id         INT NOT NULL,
    user_id           INT NOT NULL,
    title             VARCHAR(256) DEFAULT 'New Chat',
    model             VARCHAR(64) DEFAULT 'qwen3b-q4',
    created_at        DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at        DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_tenant_user (tenant_id, user_id)
);

-- ========== 4. 对话消息 ==========
CREATE TABLE IF NOT EXISTS messages (
    id                BIGINT PRIMARY KEY AUTO_INCREMENT,
    session_id        VARCHAR(36) NOT NULL,
    role              ENUM('system','user','assistant') NOT NULL,
    content           TEXT NOT NULL,
    reasoning         TEXT,
    rag_references    JSON,
    tokens_in         INT DEFAULT 0,
    tokens_out        INT DEFAULT 0,
    created_at        DATETIME DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_session_created (session_id, created_at),
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
);

-- ========== 5. 文档 ==========
CREATE TABLE IF NOT EXISTS documents (
    id                INT PRIMARY KEY AUTO_INCREMENT,
    tenant_id         INT NOT NULL,
    filename          VARCHAR(256) NOT NULL,
    content           MEDIUMTEXT NOT NULL,
    chunk_count       INT DEFAULT 0,
    visibility        ENUM('group','public') DEFAULT 'group',
    uploaded_by       INT NOT NULL DEFAULT 0,
    status            ENUM('processing','ready','deleted') DEFAULT 'processing',
    created_at        DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at        DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    FOREIGN KEY (tenant_id) REFERENCES tenants(id) ON DELETE CASCADE,
    INDEX idx_tenant (tenant_id),
    INDEX idx_visibility (visibility),
    INDEX idx_status (status)
);

-- ========== 6. 文档分块 ==========
CREATE TABLE IF NOT EXISTS document_chunks (
    id                BIGINT PRIMARY KEY AUTO_INCREMENT,
    document_id       INT NOT NULL,
    tenant_id         INT NOT NULL,
    chunk_index       INT NOT NULL,
    content           TEXT NOT NULL,
    visibility        ENUM('group','public') DEFAULT 'group',
    created_at        DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (document_id) REFERENCES documents(id) ON DELETE CASCADE,
    INDEX idx_document (document_id),
    INDEX idx_tenant_visibility (tenant_id, visibility)
);

-- ========== 7. 审计日志 ==========
CREATE TABLE IF NOT EXISTS audit_logs (
    id                BIGINT PRIMARY KEY AUTO_INCREMENT,
    tenant_id         INT NOT NULL,
    user_id           INT NOT NULL,
    action            VARCHAR(32) NOT NULL,
    detail            JSON,
    created_at        DATETIME NOT NULL,
    INDEX idx_tenant_time (tenant_id, created_at)
);

-- ========== 8. 计费记录 ==========
CREATE TABLE IF NOT EXISTS billing_records (
    id                BIGINT PRIMARY KEY AUTO_INCREMENT,
    tenant_id         INT NOT NULL,
    user_id           INT NOT NULL,
    session_id        VARCHAR(36),
    tokens_used       INT NOT NULL DEFAULT 0,
    request_id        VARCHAR(64),
    created_at        DATETIME DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_tenant_date (tenant_id, created_at),
    INDEX idx_session (session_id)
);

-- ========== 初始数据 ==========
INSERT IGNORE INTO tenants (id, name, token_limit_daily) VALUES
    (1, 'default', 100000);

-- Default admin (password: changeme123) is auto-created on first startup
-- via main.cpp bootstrap. Change the password immediately after first login.
-- To create additional users, use POST /api/users (admin only).
