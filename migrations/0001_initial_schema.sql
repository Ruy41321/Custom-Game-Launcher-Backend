-- 0001_initial_schema
--
-- Initial schema for the Custom Game Launcher.
--
-- Migrations are immutable once merged: the runner records a SHA-256 of every file and
-- refuses to start if an already applied migration has changed. Corrections go into a new
-- numbered file, never into this one.
--
-- Conventions used throughout:
--   * surrogate keys are uuid, generated server side
--   * every timestamp is timestamptz, never a naive timestamp
--   * enumerated domains are real PostgreSQL enums, so an invalid value cannot be stored
--   * every foreign key has an explicit ON DELETE policy and a supporting index

CREATE EXTENSION IF NOT EXISTS citext;   -- case-insensitive email comparison
CREATE EXTENSION IF NOT EXISTS pg_trgm;  -- trigram index for game title search

CREATE OR REPLACE FUNCTION set_updated_at() RETURNS trigger AS $$
BEGIN
    NEW.updated_at = now();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- ===========================================================================
-- Identity
-- ===========================================================================

CREATE TABLE users (
    id                 uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    email              citext      NOT NULL UNIQUE,
    email_verified_at  timestamptz,
    password_hash      text        NOT NULL,
    display_name       text        NOT NULL,
    -- Cumulative cap across everything the user has ever uploaded, not per game.
    -- Adjustable per user from the admin application.
    upload_quota_bytes bigint      NOT NULL DEFAULT 5368709120,  -- 5 GiB
    upload_used_bytes  bigint      NOT NULL DEFAULT 0,
    is_active          boolean     NOT NULL DEFAULT true,
    last_login_at      timestamptz,
    created_at         timestamptz NOT NULL DEFAULT now(),
    updated_at         timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT users_display_name_length  CHECK (char_length(display_name) BETWEEN 2 AND 64),
    CONSTRAINT users_quota_non_negative   CHECK (upload_quota_bytes >= 0),
    CONSTRAINT users_used_non_negative    CHECK (upload_used_bytes >= 0)
);

CREATE TRIGGER users_set_updated_at
    BEFORE UPDATE ON users
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- Roles and permissions.
--
-- Deliberately table driven rather than an enum column on users: adding a role or
-- re-cutting a permission set is an INSERT, never a destructive migration. The "devlist"
-- the server operator maintains is simply membership in the 'dev' role.
-- ---------------------------------------------------------------------------

CREATE TABLE roles (
    id          smallserial PRIMARY KEY,
    key         text        NOT NULL UNIQUE,
    description text        NOT NULL DEFAULT '',
    -- System roles are referenced by application logic and must not be deleted.
    is_system   boolean     NOT NULL DEFAULT false,
    created_at  timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT roles_key_format CHECK (key ~ '^[a-z][a-z0-9_]*$')
);

CREATE TABLE permissions (
    id          smallserial PRIMARY KEY,
    key         text        NOT NULL UNIQUE,
    description text        NOT NULL DEFAULT '',
    CONSTRAINT permissions_key_format CHECK (key ~ '^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$')
);

CREATE TABLE role_permissions (
    role_id       smallint NOT NULL REFERENCES roles(id)       ON DELETE CASCADE,
    permission_id smallint NOT NULL REFERENCES permissions(id) ON DELETE CASCADE,
    PRIMARY KEY (role_id, permission_id)
);

CREATE INDEX role_permissions_permission_id_idx ON role_permissions (permission_id);

CREATE TABLE user_roles (
    user_id    uuid        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role_id    smallint    NOT NULL REFERENCES roles(id) ON DELETE CASCADE,
    granted_at timestamptz NOT NULL DEFAULT now(),
    granted_by uuid        REFERENCES users(id) ON DELETE SET NULL,
    PRIMARY KEY (user_id, role_id)
);

CREATE INDEX user_roles_role_id_idx ON user_roles (role_id);

-- ---------------------------------------------------------------------------
-- Refresh tokens.
--
-- Only the hash is stored, so a database leak does not hand out sessions. Tokens are
-- rotated on every use and grouped into a family; replaying an already rotated token is
-- treated as theft and revokes the whole family.
-- ---------------------------------------------------------------------------

CREATE TABLE refresh_tokens (
    id          uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id     uuid        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    family_id   uuid        NOT NULL,
    token_hash  text        NOT NULL UNIQUE,
    issued_at   timestamptz NOT NULL DEFAULT now(),
    expires_at  timestamptz NOT NULL,
    revoked_at  timestamptz,
    replaced_by uuid        REFERENCES refresh_tokens(id) ON DELETE SET NULL,
    user_agent  text,
    ip_address  inet,
    CONSTRAINT refresh_tokens_expiry_after_issue CHECK (expires_at > issued_at)
);

CREATE INDEX refresh_tokens_user_id_idx     ON refresh_tokens (user_id);
CREATE INDEX refresh_tokens_family_id_idx   ON refresh_tokens (family_id);
CREATE INDEX refresh_tokens_replaced_by_idx ON refresh_tokens (replaced_by);
CREATE INDEX refresh_tokens_expires_at_idx  ON refresh_tokens (expires_at) WHERE revoked_at IS NULL;

-- Email verification and password reset share one table: identical shape, identical
-- lifecycle (hashed, single use, expiring), distinguished by purpose.
CREATE TYPE user_token_purpose AS ENUM ('email_verification', 'password_reset');

CREATE TABLE user_tokens (
    id          uuid               PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id     uuid               NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    purpose     user_token_purpose NOT NULL,
    token_hash  text               NOT NULL UNIQUE,
    expires_at  timestamptz        NOT NULL,
    consumed_at timestamptz,
    created_at  timestamptz        NOT NULL DEFAULT now()
);

CREATE INDEX user_tokens_user_purpose_idx ON user_tokens (user_id, purpose);

-- ===========================================================================
-- Catalog
-- ===========================================================================

CREATE TYPE game_visibility AS ENUM ('draft', 'unlisted', 'public');

CREATE TABLE games (
    id                uuid            PRIMARY KEY DEFAULT gen_random_uuid(),
    slug              text            NOT NULL UNIQUE,
    title             text            NOT NULL,
    summary           text            NOT NULL DEFAULT '',
    description       text            NOT NULL DEFAULT '',
    publisher_user_id uuid            NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    -- Default ordering key for the library and the Explore section.
    release_date      date,
    visibility        game_visibility NOT NULL DEFAULT 'draft',
    created_at        timestamptz     NOT NULL DEFAULT now(),
    updated_at        timestamptz     NOT NULL DEFAULT now(),
    CONSTRAINT games_slug_format  CHECK (slug ~ '^[a-z0-9]+(-[a-z0-9]+)*$'),
    CONSTRAINT games_title_length CHECK (char_length(title) BETWEEN 1 AND 200)
);

CREATE TRIGGER games_set_updated_at
    BEFORE UPDATE ON games
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE INDEX games_publisher_idx    ON games (publisher_user_id);
CREATE INDEX games_release_date_idx ON games (release_date DESC NULLS LAST);
CREATE INDEX games_visibility_idx   ON games (visibility);
CREATE INDEX games_title_trgm_idx   ON games USING gin (title gin_trgm_ops);

CREATE TYPE game_media_kind AS ENUM ('cover', 'banner', 'logo', 'screenshot');

CREATE TABLE game_media (
    id          uuid            PRIMARY KEY DEFAULT gen_random_uuid(),
    game_id     uuid            NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    kind        game_media_kind NOT NULL,
    storage_key text            NOT NULL,
    alt_text    text            NOT NULL DEFAULT '',
    sort_order  integer         NOT NULL DEFAULT 0,
    created_at  timestamptz     NOT NULL DEFAULT now()
);

CREATE INDEX game_media_game_idx ON game_media (game_id, kind, sort_order);

-- Publisher-set release stage. A release build carries no badge in the UI; the other
-- three are shown as Demo / Alpha / Beta labels.
CREATE TYPE build_stage AS ENUM ('demo', 'alpha', 'beta', 'release');

CREATE TABLE game_versions (
    id            uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    game_id       uuid        NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    semver        text        NOT NULL,
    -- Parsed components, stored so ordering is numeric. Sorting the semver text would put
    -- 0.10.0 before 0.9.0.
    version_major integer     NOT NULL,
    version_minor integer     NOT NULL DEFAULT 0,
    version_patch integer     NOT NULL DEFAULT 0,
    stage         build_stage NOT NULL DEFAULT 'release',
    release_notes text        NOT NULL DEFAULT '',
    published_at  timestamptz,
    created_at    timestamptz NOT NULL DEFAULT now(),
    UNIQUE (game_id, semver),
    CONSTRAINT game_versions_semver_format
        CHECK (semver ~ '^[0-9]+(\.[0-9]+){0,2}$'),
    CONSTRAINT game_versions_components_non_negative
        CHECK (version_major >= 0 AND version_minor >= 0 AND version_patch >= 0)
);

CREATE INDEX game_versions_ordering_idx
    ON game_versions (game_id, version_major DESC, version_minor DESC, version_patch DESC);
CREATE INDEX game_versions_published_idx
    ON game_versions (game_id, published_at DESC) WHERE published_at IS NOT NULL;

CREATE TYPE build_platform     AS ENUM ('windows', 'linux', 'macos');
CREATE TYPE build_architecture AS ENUM ('x64', 'arm64');
CREATE TYPE build_status       AS ENUM ('uploading', 'ready', 'failed');

CREATE TABLE builds (
    id                       uuid               PRIMARY KEY DEFAULT gen_random_uuid(),
    game_version_id          uuid               NOT NULL REFERENCES game_versions(id) ON DELETE CASCADE,
    platform                 build_platform     NOT NULL,
    architecture             build_architecture NOT NULL DEFAULT 'x64',
    status                   build_status       NOT NULL DEFAULT 'uploading',
    -- SHA-256 of the canonical manifest document, so a client can tell two builds apart
    -- without downloading the file list.
    manifest_sha256          char(64),
    total_size_bytes         bigint             NOT NULL DEFAULT 0,
    file_count               integer            NOT NULL DEFAULT 0,
    entrypoint_relative_path text,
    default_launch_args      text               NOT NULL DEFAULT '',
    created_at               timestamptz        NOT NULL DEFAULT now(),
    ready_at                 timestamptz,
    UNIQUE (game_version_id, platform, architecture),
    CONSTRAINT builds_manifest_hash_format
        CHECK (manifest_sha256 IS NULL OR manifest_sha256 ~ '^[0-9a-f]{64}$'),
    -- A build only becomes downloadable once it has a manifest and something to launch.
    CONSTRAINT builds_ready_is_complete CHECK (
        status <> 'ready'
        OR (manifest_sha256 IS NOT NULL
            AND entrypoint_relative_path IS NOT NULL
            AND ready_at IS NOT NULL)
    )
);

CREATE INDEX builds_game_version_idx ON builds (game_version_id);
CREATE INDEX builds_status_idx       ON builds (status);

-- ---------------------------------------------------------------------------
-- Content-addressed storage.
--
-- Every distinct file of every build exists exactly once, keyed by its SHA-256. Two
-- versions that share a file share the row, so unchanged content is neither re-uploaded
-- nor re-stored, and the delta between any two builds is a set difference over
-- build_files. See CLAUDE.md section 3.
-- ---------------------------------------------------------------------------

CREATE TABLE blobs (
    sha256      char(64)    PRIMARY KEY,
    size_bytes  bigint      NOT NULL,
    storage_key text        NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT blobs_sha256_lowercase_hex CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    CONSTRAINT blobs_size_non_negative    CHECK (size_bytes >= 0)
);

CREATE TABLE build_files (
    build_id      uuid     NOT NULL REFERENCES builds(id) ON DELETE CASCADE,
    relative_path text     NOT NULL,
    -- RESTRICT, not CASCADE: a blob may only be removed once no manifest references it.
    -- Garbage collection deletes unreferenced blobs, never referenced ones.
    blob_sha256   char(64) NOT NULL REFERENCES blobs(sha256) ON DELETE RESTRICT,
    is_executable boolean  NOT NULL DEFAULT false,
    PRIMARY KEY (build_id, relative_path),
    -- Defence in depth against path traversal: the API validates these paths too, but a
    -- malicious manifest must not be storable in the first place.
    CONSTRAINT build_files_relative_path_safe CHECK (
        relative_path <> ''
        AND relative_path !~ '^/'
        AND relative_path !~ '(^|/)\.\.(/|$)'
        AND relative_path !~ '\\'
        AND relative_path !~ '^[A-Za-z]:'
    )
);

CREATE INDEX build_files_blob_idx ON build_files (blob_sha256);

CREATE TABLE patch_notes (
    id              uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    game_id         uuid        NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    game_version_id uuid        REFERENCES game_versions(id) ON DELETE SET NULL,
    title           text        NOT NULL,
    body_markdown   text        NOT NULL DEFAULT '',
    author_user_id  uuid        REFERENCES users(id) ON DELETE SET NULL,
    published_at    timestamptz,
    created_at      timestamptz NOT NULL DEFAULT now(),
    updated_at      timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT patch_notes_title_length CHECK (char_length(title) BETWEEN 1 AND 200)
);

CREATE TRIGGER patch_notes_set_updated_at
    BEFORE UPDATE ON patch_notes
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE INDEX patch_notes_game_published_idx ON patch_notes (game_id, published_at DESC);
CREATE INDEX patch_notes_version_idx        ON patch_notes (game_version_id);
CREATE INDEX patch_notes_author_idx         ON patch_notes (author_user_id);

-- ===========================================================================
-- Library, analytics, compliance
-- ===========================================================================

-- Server-side library membership. The client additionally tracks local install state,
-- which is per machine and therefore never stored here.
CREATE TABLE user_games (
    user_id  uuid        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    game_id  uuid        NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    added_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, game_id)
);

CREATE INDEX user_games_game_idx ON user_games (game_id);

CREATE TYPE download_kind AS ENUM ('full', 'delta');

CREATE TABLE download_events (
    id              bigserial     PRIMARY KEY,
    game_id         uuid          NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    build_id        uuid          REFERENCES builds(id) ON DELETE SET NULL,
    -- Nullable so an analytics row survives a GDPR erasure of the user who produced it.
    user_id         uuid          REFERENCES users(id) ON DELETE SET NULL,
    from_version_id uuid          REFERENCES game_versions(id) ON DELETE SET NULL,
    kind            download_kind NOT NULL,
    bytes_planned   bigint        NOT NULL DEFAULT 0,
    created_at      timestamptz   NOT NULL DEFAULT now(),
    CONSTRAINT download_events_bytes_non_negative CHECK (bytes_planned >= 0)
);

CREATE INDEX download_events_game_time_idx ON download_events (game_id, created_at DESC);
CREATE INDEX download_events_build_idx     ON download_events (build_id);
CREATE INDEX download_events_user_idx      ON download_events (user_id);
CREATE INDEX download_events_from_idx      ON download_events (from_version_id);

CREATE TABLE audit_log (
    id            bigserial   PRIMARY KEY,
    actor_user_id uuid        REFERENCES users(id) ON DELETE SET NULL,
    action        text        NOT NULL,
    entity_type   text        NOT NULL,
    entity_id     text,
    metadata      jsonb       NOT NULL DEFAULT '{}'::jsonb,
    created_at    timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX audit_log_actor_time_idx  ON audit_log (actor_user_id, created_at DESC);
CREATE INDEX audit_log_entity_idx      ON audit_log (entity_type, entity_id);

CREATE TYPE deletion_request_status AS ENUM ('pending', 'completed', 'cancelled');

CREATE TABLE account_deletion_requests (
    id           uuid                    PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id      uuid                    NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status       deletion_request_status NOT NULL DEFAULT 'pending',
    reason       text                    NOT NULL DEFAULT '',
    requested_at timestamptz             NOT NULL DEFAULT now(),
    processed_at timestamptz
);

CREATE INDEX account_deletion_requests_user_idx ON account_deletion_requests (user_id);

-- A user can have at most one erasure request in flight.
CREATE UNIQUE INDEX account_deletion_requests_single_pending
    ON account_deletion_requests (user_id) WHERE status = 'pending';

-- ===========================================================================
-- Seed: roles and permissions
-- ===========================================================================

INSERT INTO permissions (key, description) VALUES
    ('library.read',          'View the personal library'),
    ('library.manage',        'Add and remove games from the personal library'),
    ('game.read',             'Browse published games in Explore'),
    ('game.download',         'Download builds'),
    ('game.publish',          'Create and update own games'),
    ('build.upload',          'Upload builds for own games'),
    ('patchnote.write',       'Write patch notes for own games'),
    ('admin.users.manage',    'Manage users, roles and upload quotas'),
    ('admin.roles.manage',    'Create roles and change permission assignments'),
    ('admin.games.manage',    'Manage any game regardless of publisher'),
    ('admin.settings.manage', 'Change server settings')
ON CONFLICT (key) DO NOTHING;

INSERT INTO roles (key, description, is_system) VALUES
    ('player', 'Default role: library, Explore, downloads', true),
    ('dev',    'Publisher: everything a player can do, plus publishing builds and patch notes', true),
    ('admin',  'Server operator: full control', true)
ON CONFLICT (key) DO NOTHING;

INSERT INTO role_permissions (role_id, permission_id)
SELECT r.id, p.id
FROM roles r
JOIN permissions p ON p.key IN (
    'library.read', 'library.manage', 'game.read', 'game.download'
)
WHERE r.key = 'player'
ON CONFLICT DO NOTHING;

INSERT INTO role_permissions (role_id, permission_id)
SELECT r.id, p.id
FROM roles r
JOIN permissions p ON p.key IN (
    'library.read', 'library.manage', 'game.read', 'game.download',
    'game.publish', 'build.upload', 'patchnote.write'
)
WHERE r.key = 'dev'
ON CONFLICT DO NOTHING;

INSERT INTO role_permissions (role_id, permission_id)
SELECT r.id, p.id
FROM roles r
CROSS JOIN permissions p
WHERE r.key = 'admin'
ON CONFLICT DO NOTHING;
