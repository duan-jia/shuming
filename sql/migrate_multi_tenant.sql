-- Migration: multi-tenant redesign (super_admin/tenant_admin/member → admin/user, access_role → visibility)
-- Run this against an existing llm_platform database before starting the updated server.

USE llm_platform;

-- 1. Simplify user roles: expand enum first, update data, then shrink enum
ALTER TABLE users MODIFY COLUMN role ENUM('super_admin','tenant_admin','member','admin','user') DEFAULT 'user';
UPDATE users SET role='admin' WHERE role IN ('super_admin','tenant_admin');
UPDATE users SET role='user' WHERE role='member';
ALTER TABLE users MODIFY COLUMN role ENUM('admin','user') DEFAULT 'user';

-- 2. Documents: access_role → visibility, add uploaded_by
-- Same approach: expand to include new values, update, then change column
ALTER TABLE documents MODIFY COLUMN access_role ENUM('all','admin','group','public') DEFAULT 'group';
UPDATE documents SET access_role='group' WHERE access_role IN ('all','admin') OR access_role IS NULL;
ALTER TABLE documents
  CHANGE access_role visibility ENUM('group','public') DEFAULT 'group',
  ADD COLUMN uploaded_by INT NOT NULL DEFAULT 0;

-- 3. Tenants: add status column
ALTER TABLE tenants ADD COLUMN status ENUM('active','inactive') DEFAULT 'active';

-- 4. Verify
SELECT 'users' as tbl, role, COUNT(*) as cnt FROM users GROUP BY role
UNION ALL
SELECT 'documents', visibility, COUNT(*) FROM documents GROUP BY visibility
UNION ALL
SELECT 'tenants', status, COUNT(*) FROM tenants GROUP BY status;
