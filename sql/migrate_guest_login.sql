-- Required before enabling auth.guest_enabled on an existing database.
-- Run with the same database selected as sql/init.sql.
ALTER TABLE users MODIFY COLUMN role ENUM('admin','user','guest') DEFAULT 'user';
-- Guest rows are independent identities; never share one public guest account.
-- Retain for at most 24h if no longer needed (FK cascades remove private histories):
-- DELETE FROM users WHERE role='guest' AND created_at < NOW() - INTERVAL 1 DAY;
