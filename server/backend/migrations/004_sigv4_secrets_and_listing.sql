-- 004_sigv4_secrets_and_listing.sql
-- Re-run on every start: everything here is idempotent.

-- AES-256-GCM ciphertext of the access key's secret (see services/KeyStore).
-- NULL while the secret is still plaintext in secret_key; once encrypted,
-- secret_key holds the marker '!enc'. Lazily filled on first use when
-- S3_SECRET_ENCRYPTION_KEY is set.
ALTER TABLE api_keys ADD COLUMN IF NOT EXISTS secret_enc TEXT;

-- Listings are ordered by raw bytes (COLLATE "C"), like S3, whatever collation
-- the database was created with; this index serves that order and prefix scans.
CREATE INDEX IF NOT EXISTS idx_objects_bucket_key_c
    ON objects (bucket_id, key COLLATE "C");
