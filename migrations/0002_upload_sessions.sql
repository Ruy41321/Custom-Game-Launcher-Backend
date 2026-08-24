-- 0002_upload_sessions
--
-- Resumable blob uploads, and the attribution needed to account for them.
--
-- A build is published in three steps: create the build row, upload the blobs it needs that
-- the server does not already have, then submit the manifest. The middle step is the one that
-- can be interrupted on a home connection halfway through a two-gigabyte file, so it gets a
-- server-side session: the database records how many bytes have been accepted, and that
-- number — not the size of the file on disk — is the offset a resumed upload continues at.

-- Which account paid for a blob out of its upload quota. Nullable because a blob outlives the
-- account that first uploaded it: content-addressed storage means later builds, possibly from
-- other publishers, reference the very same row.
ALTER TABLE blobs ADD COLUMN uploaded_by_user_id uuid REFERENCES users(id) ON DELETE SET NULL;

CREATE INDEX blobs_uploaded_by_idx ON blobs (uploaded_by_user_id);

CREATE TYPE upload_session_status AS ENUM ('pending', 'completed', 'aborted');

CREATE TABLE upload_sessions (
    id                  uuid                  PRIMARY KEY DEFAULT gen_random_uuid(),
    build_id            uuid                  NOT NULL REFERENCES builds(id) ON DELETE CASCADE,
    -- Kept alongside the build so an abandoned session can be swept, and its quota reasoned
    -- about, without joining back through the catalog.
    user_id             uuid                  NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    blob_sha256         char(64)              NOT NULL,
    declared_size_bytes bigint                NOT NULL,
    received_bytes      bigint                NOT NULL DEFAULT 0,
    status              upload_session_status NOT NULL DEFAULT 'pending',
    created_at          timestamptz           NOT NULL DEFAULT now(),
    updated_at          timestamptz           NOT NULL DEFAULT now(),
    expires_at          timestamptz           NOT NULL,
    CONSTRAINT upload_sessions_sha256_format
        CHECK (blob_sha256 ~ '^[0-9a-f]{64}$'),
    CONSTRAINT upload_sessions_declared_size_non_negative
        CHECK (declared_size_bytes >= 0),
    -- Defence in depth: the API rejects an over-long chunk first, but no sequence of requests
    -- may leave a session claiming to hold more bytes than it declared.
    CONSTRAINT upload_sessions_received_within_declared
        CHECK (received_bytes >= 0 AND received_bytes <= declared_size_bytes)
    -- Deliberately no `expires_at > created_at` rule: forcing a session to expire by moving
    -- its expiry into the past is a legitimate administrative action, and a constraint that
    -- forbids it buys nothing the configuration check on the TTL does not already give.
);

CREATE TRIGGER upload_sessions_set_updated_at
    BEFORE UPDATE ON upload_sessions
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE INDEX upload_sessions_user_idx   ON upload_sessions (user_id)    WHERE status = 'pending';
CREATE INDEX upload_sessions_build_idx  ON upload_sessions (build_id);
CREATE INDEX upload_sessions_expiry_idx ON upload_sessions (expires_at) WHERE status = 'pending';

-- At most one open session per (build, blob). A client whose negotiation request is retried —
-- because the response was lost, not because the upload failed — then resumes the session it
-- already has instead of opening a second staging file for the same content.
CREATE UNIQUE INDEX upload_sessions_open_unique
    ON upload_sessions (build_id, blob_sha256) WHERE status = 'pending';
