-- Add tenant-scoped PDF deduplication and pending-review metadata.

USE llm_platform;

ALTER TABLE documents
  MODIFY COLUMN source_storage_name VARCHAR(256) NULL,
  ADD COLUMN source_sha256 CHAR(64) NULL AFTER source_size_bytes,
  ADD COLUMN review_reason VARCHAR(1000) NOT NULL DEFAULT '' AFTER source_sha256,
  MODIFY COLUMN status ENUM('processing','pending','ready','failed','deleted') DEFAULT 'processing',
  ADD UNIQUE INDEX uq_tenant_source_sha256 (tenant_id, source_sha256);
