-- Migration: allow document ingestion failures to be represented explicitly.

USE llm_platform;

ALTER TABLE documents
  MODIFY COLUMN status ENUM('processing','ready','failed','deleted') DEFAULT 'processing';
