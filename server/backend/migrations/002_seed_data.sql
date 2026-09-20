-- 002_seed_data.sql
-- Default API key for development.

-- Development bootstrap key. Seeded only while NO key exists at all: the
-- entrypoint re-runs every migration on each start, so an ON CONFLICT DO
-- NOTHING insert quietly RESURRECTED this well-known admin credential after
-- a deployment had deliberately deleted it.
INSERT INTO api_keys (access_key, secret_key, owner, permissions)
SELECT 'minioadmin', 'minioadmin', 'admin', 'read,write,admin'
WHERE NOT EXISTS (SELECT 1 FROM api_keys);
