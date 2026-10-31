-- Back up the target database, then apply before starting the new backend.
-- Additive and restartable; existing messages and document data are preserved.
USE llm_platform;
SET @agent_column_sql = IF(
    EXISTS(SELECT 1 FROM information_schema.COLUMNS
           WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='messages' AND COLUMN_NAME='request_id'),
    'SELECT 1', 'ALTER TABLE messages ADD COLUMN request_id VARCHAR(64) NULL');
PREPARE agent_column_stmt FROM @agent_column_sql;
EXECUTE agent_column_stmt;
DEALLOCATE PREPARE agent_column_stmt;

CREATE TABLE IF NOT EXISTS agent_requests (
    request_id VARCHAR(64) PRIMARY KEY,
    session_id VARCHAR(36) NOT NULL,
    model VARCHAR(128) NOT NULL,
    status VARCHAR(32) NOT NULL DEFAULT 'running',
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    INDEX idx_agent_session (session_id, created_at),
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS agent_events (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    request_id VARCHAR(64) NOT NULL,
    event_type VARCHAR(32) NOT NULL,
    payload JSON NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_agent_request (request_id, id),
    FOREIGN KEY (request_id) REFERENCES agent_requests(request_id) ON DELETE CASCADE
);
