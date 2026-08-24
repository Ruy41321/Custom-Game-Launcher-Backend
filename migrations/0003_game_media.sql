-- 0003_game_media
--
-- Finishes the game_media table that migration 0001 declared and nothing has ever written to.
--
-- Media is content-addressed like a build file, but it is stored under its own root and served
-- from a public, unsigned location. A cover is public by definition: signing it would mean
-- minting one expiring signature per card in an Explore grid, and none of them could ever be
-- cached. Keeping the two roots separate is what makes that safe — the public location can
-- never reach a build blob, because build blobs are not under it.

-- These columns are added NOT NULL with no default on purpose. No endpoint has ever inserted
-- into this table, so it is empty in every deployment; if it is not, this migration should
-- fail loudly rather than invent a content type for rows nobody can explain.
ALTER TABLE game_media
    ADD COLUMN sha256       char(64) NOT NULL,
    ADD COLUMN content_type text     NOT NULL,
    ADD COLUMN size_bytes   bigint   NOT NULL;

ALTER TABLE game_media
    ADD CONSTRAINT game_media_sha256_lowercase_hex CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    ADD CONSTRAINT game_media_size_positive        CHECK (size_bytes > 0),
    -- The set the API sniffs for. Stored as a check rather than an enum so adding a format
    -- later is one ALTER instead of a type migration.
    ADD CONSTRAINT game_media_content_type_allowed
        CHECK (content_type IN ('image/png', 'image/jpeg', 'image/webp')),
    -- The storage key is derived from the hash and never from anything a client sent, but this
    -- table is what a cleanup sweep reads to decide which files on disk are still referenced,
    -- so the shape is worth enforcing where it is stored.
    ADD CONSTRAINT game_media_storage_key_shape
        CHECK (storage_key ~ '^[0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{64}\.[a-z]{3,4}$');

-- A game has one cover, one banner and one logo; screenshots are a gallery. Enforced here
-- rather than in a service so that no route, present or future, can create a second cover.
CREATE UNIQUE INDEX game_media_single_kind_unique
    ON game_media (game_id, kind) WHERE kind <> 'screenshot';

-- Deleting the last row that references a file is what makes that file collectable; the sweep
-- asks this question per storage key.
CREATE INDEX game_media_storage_key_idx ON game_media (storage_key);
