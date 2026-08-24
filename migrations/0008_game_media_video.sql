-- 0008_game_media_video
--
-- A video is a fifth kind of game media, stored the same way the pictures are.
--
-- The maintainer asked for a trailer on a game's page and chose **uploaded files, not external
-- links**: a publisher-supplied URL would mean the launcher fetching bytes from whatever host
-- somebody typed, which is the thing D27 already refused for artwork. So a video travels the
-- route a screenshot travels, is content-addressed like a screenshot, and lands under the same
-- public media root — a separate root would buy nothing, because the property that makes that
-- root safe is that it is not the *blob* root, and that stays true with one more extension in
-- it.
--
-- Three things this migration cannot do in one breath, and the shapes that work around them:
--
-- **`ALTER TYPE ... ADD VALUE` may run inside a transaction on PostgreSQL 12 and later, but the
-- value it adds may not be *used* until that transaction commits** — and the migration runner
-- wraps every file in one (`BEGIN` … `COMMIT` in `MigrationRunner::run`). Every statement below
-- therefore names only the four kinds that already existed. That is why the partial unique index
-- becomes an allow-list of the singleton kinds instead of the deny-list it was: `kind <>
-- 'screenshot'` would have had to become `kind NOT IN ('screenshot', 'video')`, which is exactly
-- the forbidden reference. The allow-list is the better statement of the rule anyway — the index
-- exists to say *these three kinds are singular*, and it now says so directly.
--
-- **The predicate has to match what `PgMediaRepository::create` infers against.** Its upsert
-- names the same predicate in its `ON CONFLICT`, so the two are changed in the same commit and
-- the text is deliberately identical.
--
-- **A kind and a content type must agree.** Storing a PNG under `kind = 'video'` would produce a
-- row every client would try to play, so the pairing is enforced here rather than trusted to the
-- service. It is written as "the four original kinds are exactly the image ones", which says the
-- same thing without naming the new value.

ALTER TYPE game_media_kind ADD VALUE IF NOT EXISTS 'video';

-- The set the API sniffs for, widened by the two containers it will now identify. Still a check
-- rather than a lookup table, for the reason migration 0003 gave: adding a format is one ALTER.
ALTER TABLE game_media
    DROP CONSTRAINT game_media_content_type_allowed;

ALTER TABLE game_media
    ADD CONSTRAINT game_media_content_type_allowed
        CHECK (content_type IN ('image/png', 'image/jpeg', 'image/webp',
                                'video/mp4', 'video/webm'));

-- A game has one cover, one banner and one logo. Screenshots are a gallery and so are videos,
-- and both are capped in the service instead — a bound on how many, not on whether a second may
-- exist at all.
DROP INDEX game_media_single_kind_unique;

CREATE UNIQUE INDEX game_media_single_kind_unique
    ON game_media (game_id, kind) WHERE kind IN ('cover', 'banner', 'logo');

-- The storage key's own shape has to widen too, and this is the sort of thing a suite catches
-- and reading does not: migration 0003 spelled the extension `[a-z]{3,4}`, and "mp4" has a digit
-- in it. Every extension this server writes comes from `domain::extensionOf`, so the constraint
-- is not what decides which formats exist — it is what stops a row pointing at a path the sweep
-- and the file server could not agree on.
ALTER TABLE game_media
    DROP CONSTRAINT game_media_storage_key_shape;

ALTER TABLE game_media
    ADD CONSTRAINT game_media_storage_key_shape
        CHECK (storage_key ~ '^[0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{64}\.[a-z0-9]{3,4}$');

-- Reads as: a row whose kind is one of the four original ones carries an image content type, and
-- a row whose kind is anything else does not. With the allow-list above, "not an image" leaves
-- only the two video types.
ALTER TABLE game_media
    ADD CONSTRAINT game_media_kind_matches_content_type
        CHECK ((kind IN ('cover', 'banner', 'logo', 'screenshot')) = (content_type LIKE 'image/%'));
