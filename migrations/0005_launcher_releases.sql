-- 0005_launcher_releases
--
-- Where a release of the *launcher itself* lives, as opposed to a build of somebody's game.
--
-- The catalog was the obvious home and is the wrong one. Every catalog route carries an Actor
-- and asks `mayViewGame`, and the launcher that most needs an update is the one that cannot
-- sign in yet — a server it has never seen, an address nobody confirmed, a password somebody
-- lost. Putting a release behind the catalog would mean opening an unauthenticated hole inside
-- CatalogService, which is the one place this schema has spent four milestones concentrating
-- authorization. A table of its own, read by a route that never looks for a token, has no such
-- hole to open.
--
-- What is *not* duplicated is the storage: the artifact is a content-addressed blob like
-- everything else here, on its own root, because that root is served without a signature.
--
-- **The signed document is the record, and the columns are derived from it.** `document` holds
-- the exact bytes the release was signed over; every other column here is extracted from those
-- bytes at publish time so that a query can order and filter without parsing JSON. They cannot
-- disagree, because nothing writes them separately: see app/ReleasePublish.cpp, which parses
-- the document and takes both from the same parse. If they ever did disagree, the document is
-- the one that was signed and therefore the one that is true.

CREATE TABLE launcher_releases (
    id              uuid        PRIMARY KEY DEFAULT gen_random_uuid(),

    -- Which stream this release belongs to. Text rather than an enum for the reason 0004 gives
    -- for crash_reports.kind: the set is a product decision and adding to it must not need a
    -- migration. The service validates it, so a typo cannot create a third channel by accident.
    channel         text        NOT NULL,

    -- The version as it was written, plus its components, because ordering has to be numeric:
    -- compared as text 0.10.0 sorts before 0.9.0, which would offer a downgrade as the latest
    -- release. Same reasoning, and the same parser, as game_versions.
    version         text        NOT NULL,
    version_major   integer     NOT NULL,
    version_minor   integer     NOT NULL,
    version_patch   integer     NOT NULL,

    -- Which build of the launcher this is. A launcher asks for its own pair and gets nothing
    -- when a platform has not been published yet, which is the honest answer.
    platform        text        NOT NULL,
    arch            text        NOT NULL,

    artifact_sha256 char(64)    NOT NULL,
    artifact_size   bigint      NOT NULL,
    -- `ab/cd/<sha256>.zip`, the same layout the blobs and the artwork use, under the release
    -- root. Derivable from artifact_sha256 and kept anyway, for the same reason game_media
    -- keeps one: the rule that derives it lives in C++, and a row that names its own file can
    -- be swept by something that does not link this binary.
    storage_key     text        NOT NULL,

    -- The exact bytes the signature covers. Served verbatim, never re-serialised: the client
    -- hashes and verifies what arrived, exactly as it does with a build manifest, so a
    -- canonical form does not have to be reproduced in a second language.
    document        text        NOT NULL,
    -- base64 of the DER ECDSA signature over `document`. Produced off this machine, by whoever
    -- holds the private key; this server only ever verifies it. That is the whole point: an
    -- attacker who owns this database still cannot publish a launcher update.
    signature       text        NOT NULL,

    released_at     timestamptz NOT NULL,
    created_at      timestamptz NOT NULL DEFAULT now(),

    -- Set when a release is withdrawn. Not a delete, because the row is the record of what was
    -- once handed out, and not a boolean, because *when* it stopped being offered is the
    -- question somebody actually asks afterwards. A retired release is invisible to the route,
    -- so clients fall back to the previous one — and refuse it, since it is older than what
    -- they are running. Standing still is the correct outcome of withdrawing a bad build.
    retired_at      timestamptz,

    CONSTRAINT launcher_releases_channel_not_empty  CHECK (channel <> ''),
    CONSTRAINT launcher_releases_version_not_empty  CHECK (version <> ''),
    CONSTRAINT launcher_releases_platform_not_empty CHECK (platform <> ''),
    CONSTRAINT launcher_releases_arch_not_empty     CHECK (arch <> ''),
    CONSTRAINT launcher_releases_sha256_format      CHECK (artifact_sha256 ~ '^[0-9a-f]{64}$'),
    CONSTRAINT launcher_releases_size_positive      CHECK (artifact_size > 0),
    CONSTRAINT launcher_releases_document_not_empty CHECK (document <> ''),
    CONSTRAINT launcher_releases_signature_not_empty CHECK (signature <> ''),
    CONSTRAINT launcher_releases_version_components CHECK (
        version_major >= 0 AND version_minor >= 0 AND version_patch >= 0)
);

-- One release per version of a given build, so publishing the same version twice is a conflict
-- rather than two rows racing to be the newest. Re-publishing after a mistake means retiring
-- the row and raising the version, which is also what every client's downgrade rule requires.
CREATE UNIQUE INDEX launcher_releases_identity_idx
    ON launcher_releases (channel, platform, arch, version);

-- The only query the public route makes: newest live release for one triple.
CREATE INDEX launcher_releases_latest_idx
    ON launcher_releases (channel, platform, arch,
                          version_major DESC, version_minor DESC, version_patch DESC)
    WHERE retired_at IS NULL;
