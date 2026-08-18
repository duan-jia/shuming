-- Migration: retain source PDF metadata for document management and downloads.

USE llm_platform;

ALTER TABLE documents
  ADD COLUMN source_filename VARCHAR(256) NULL AFTER uploaded_by,
  ADD COLUMN source_storage_name VARCHAR(64) NULL AFTER source_filename,
  ADD COLUMN source_size_bytes BIGINT UNSIGNED NULL AFTER source_storage_name;
