-- 0006_build_names
--
-- A label a publisher gives a build when they publish it.
--
-- `builds` is unique on (game_version_id, platform, architecture), so the row already has an
-- identity — and that identity is exactly what a publisher looking at four rows of
-- "windows x64 ready" cannot use. What tells two of them apart is what was in the directory:
-- the branch it came from, the machine it was cut on, whether it is the one with the demo
-- levels. None of that is derivable from anything stored here, so it has to be typed.
--
-- Deliberately **not** unique. A name is a label, not a key: the same "Nightly" belongs on the
-- Windows build and the Linux build of one version, and refusing that would make the field
-- useless for the case it exists for.
--
-- Empty is the default and stays valid. Every build published before this migration has no
-- name and never will, and a publisher who does not want to name one should not have to.

ALTER TABLE builds ADD COLUMN name text NOT NULL DEFAULT '';

-- Mirrors domain::MAX_BUILD_NAME_LENGTH, so an over-long name is a 422 naming the field
-- rather than a driver error arriving from underneath the service.
ALTER TABLE builds ADD CONSTRAINT builds_name_length CHECK (char_length(name) <= 100);
