-- Explicit, one-time publication of all current, non-deleted documents.
-- Back up this database and the local RAG directory before executing.
-- This does not restore deleted documents or modify document/user/session IDs.
-- Apply only to the intended database, stop its application writers, then restart
-- the updated backend so both vector and BM25 permissions reload from MySQL.
START TRANSACTION;
UPDATE documents SET visibility='public' WHERE status<>'deleted';
UPDATE document_chunks dc JOIN documents d ON d.id=dc.document_id
    SET dc.visibility='public' WHERE d.status<>'deleted';
-- Keep the guest group's ID and membership while giving it its business name.
-- Stop and resolve any pre-existing name collision before running this line.
UPDATE tenants SET name='测试实验组' WHERE name='__guest__';
COMMIT;
