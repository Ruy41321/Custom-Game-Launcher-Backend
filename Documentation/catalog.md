# Catalog, Explore and library

Covers publishing games and versions, the Explore listing, game detail and the server-side
library. Implemented in `src/services/CatalogService.*`, `src/domain/Catalog.*`,
`src/repositories/postgres/PgCatalogRepositories.*` and `src/controllers/v1/{Game,Library}Controller.*`.

The upload half of publishing — blobs, resumable transfer and manifests — is documented
separately in [builds-and-uploads.md](builds-and-uploads.md).

## Endpoints

Every route requires `Authorization: Bearer <access token>`. The launcher is an online client
for everything except starting an already installed game, so an anonymous catalog would mean a
second set of visibility rules to keep correct for no gain. Every response carries
`X-Request-Id`.

| Method | Path | Permission | Purpose |
|---|---|---|---|
| GET | `/api/v1/games` | `game.read` | Explore: published games, searchable and paged |
| POST | `/api/v1/games` | `game.publish` | Create a game |
| GET | `/api/v1/games/{idOrSlug}` | `game.read` | Game detail with versions and builds |
| PATCH | `/api/v1/games/{idOrSlug}` | `game.publish` + ownership | Partial update |
| DELETE | `/api/v1/games/{idOrSlug}` | ownership | Remove the game and everything under it — see [storage-lifecycle.md](storage-lifecycle.md) |
| GET | `/api/v1/me/games` | `game.publish` | The publisher's own games, drafts included |
| POST | `/api/v1/games/{id}/versions` | `game.publish` + ownership | Create a version |
| PATCH | `/api/v1/games/{id}/versions/{versionId}` | `game.publish` + ownership | Partial update: stage, notes, published |
| DELETE | `/api/v1/games/{id}/versions/{versionId}` | `game.publish` + ownership | Remove the version and its builds |
| POST | `/api/v1/games/{id}/versions/{versionId}/builds` | `build.upload` + ownership | Create a build |
| GET | `/api/v1/library` | `library.read` | The account's library |
| PUT | `/api/v1/library/{idOrSlug}` | `library.manage` | Add a game (idempotent) |
| DELETE | `/api/v1/library/{gameId}` | `library.manage` | Remove a game |

`{idOrSlug}` accepts either a uuid or a slug; they are told apart by shape, so a catalog URL
can stay human-readable without the client having to say which form it holds.

## Query parameters for Explore

| Parameter | Default | Notes |
|---|---|---|
| `search` | – | Case-insensitive substring of the title, backed by the trigram index |
| `sort` | `releaseDate` | `releaseDate`, `title` or `recent`; an unknown value falls back to the default rather than failing, so a newer client cannot break an older server |
| `page` | 1 | 1-based, because that is what a UI shows; the repository works in offsets |
| `pageSize` | 20 | Clamped to 100 |

The response is `{ "items": [...], "total": N, "limit": L, "offset": O }`. The total comes from
a second `count(*)` rather than a `count(*) OVER ()` window: the window function reports zero
for a page past the end of the result, which is exactly when the client needs the real total to
compute its page count.

## Visibility

`games.visibility` is a PostgreSQL enum with three values, and the rules are enforced in
`CatalogService`, never in a controller.

| Visibility | Explore | Direct id or slug | Who can edit |
|---|---|---|---|
| `draft` | never | publisher and `admin.games.manage` only | publisher, `admin.games.manage` |
| `unlisted` | never | anybody holding the identifier | publisher, `admin.games.manage` |
| `public` | yes | anybody | publisher, `admin.games.manage` |

A draft that the caller may not see is reported as **404, not 403**. Reporting Forbidden would
confirm that an unannounced title exists, which turns id probing into a leak. The same rule
applies to adding a draft to a library and to every build-scoped route.

Explore clears `includeUnpublished` and `publisherUserId` on the way in, so a hand-crafted
request cannot widen it into a listing of everybody's unreleased work. The publisher dashboard
(`/api/v1/me/games`) is the only route that sets them, and it pins the publisher to the caller.

Game detail shows unpublished *versions* only to somebody who could edit the game; everybody
else sees the released ones.

## Validation

| Field | Rule |
|---|---|
| `title` | 1–200 characters after trimming |
| `slug` | `^[a-z0-9]+(-[a-z0-9]+)*$`, at most 80 characters; derived from the title when omitted |
| `summary` | at most 500 characters |
| `description` | at most 20 000 characters |
| `releaseDate` | ISO `YYYY-MM-DD`, a real calendar date, 1970–9999; empty clears it |
| `semver` | `1`, `1.2` or `1.2.3`; no leading zeros, components at most 999 999 |
| `stage` | `demo`, `alpha`, `beta`, `release` |
| `platform` | `windows`, `linux`, `macos` |
| `architecture` | `x64`, `arm64` |
| build `name` | at most 100 characters after trimming; optional, and empty stays valid |

Dates are validated in C++ rather than left to PostgreSQL so that a typo is a 422 naming the
field instead of a driver error surfacing as a 500. Version components are parsed and stored
separately from the text because ordering has to be numeric: compared as text, `0.10.0` sorts
before `0.9.0`.

Leading zeros are rejected so `1.01` and `1.1` cannot coexist as two versions of one game.

## PATCH semantics

`GameUpdate` carries an `std::optional` per field. Absent means "leave alone", so a PATCH that
omits the summary does not blank it. The SQL is a single `UPDATE … SET x = COALESCE($n, x)`,
which keeps that meaning in one place rather than building a statement per combination of
fields. `releaseDate` is the one field where an explicit empty string clears the value.

`GameVersionUpdate` is the same shape for a version, and it is what makes a version publishable
after it exists.

### Publishing a version, afterwards

Until 2026-08-17 `published_at` could only be set at creation, by `publish: true` on the POST.
There was no route that changed a version at all, so **a version created with that flag unset
could never be published** — the only way forward was to delete it, and its builds with it, and
upload everything again. `IGameVersionRepository::publish` had existed since migration 0001 and
was called by nothing; it has been replaced by `update`, which does the same job as part of a
partial update rather than as a verb of its own.

`published` has three states on the wire, and they are not two:

| `published` | Effect |
|---|---|
| absent | left alone — a PATCH that carries only new release notes must not withdraw anything |
| `true` | `published_at = COALESCE(published_at, now())`, so publishing twice **cannot move the date a release went out** |
| `false` | `published_at = NULL` |

Withdrawing is allowed, and what it costs is worth stating plainly: a player who has the game
installed stops being offered that version as an update, and one who has not stops seeing it at
all. That is the trade `visibility: draft` already makes for a whole game, and it is the
reversible thing standing beside a DELETE that is not.

## Ownership

`CatalogService::editableGame` resolves a game and checks ownership in one place, and every
mutating path goes through it. Ownership is *publisher or `admin.games.manage`* — written that
way rather than as a bare admin check, so a moderation permission never becomes a way to skip
the ownership rule for ordinary publishers.

`deleteGame` goes through the same helper, so the delete cannot disagree with the patch about
who owns what. What it does with the rows, the files and other people's installs is in
[storage-lifecycle.md](storage-lifecycle.md).

Creating a build additionally checks that the version belongs to the game named in the path.
Without that check a publisher could hang a build off somebody else's version by pairing it
with a game they do own.

### Sixteen write routes, tried by a stranger (2026-08-18)

Driven against a running server with two publishers, because reading the code is what missed
D62. Every write route of a game, a version, a build, the artwork and the devlog was called
from an account that owns none of them, and every one refused with the victim's game left
unchanged:

| Refused with | Routes |
|---|---|
| **403 forbidden** | `PATCH`/`DELETE /games/{id}`, `POST`/`PATCH`/`DELETE` on its versions, `POST .../builds`, `DELETE /builds/{id}`, `POST /games/{id}/media`, `PATCH`/`DELETE /media/{id}`, `POST /games/{id}/patch-notes`, `PATCH`/`DELETE /patch-notes/{id}` |
| **404 not found** | `POST /builds/{id}/blobs/missing`, `POST /builds/{id}/uploads`, `POST /builds/{id}/manifest` |

The split is not an inconsistency. A game the caller can *see* is refused by `mayEditGame`, and
saying so costs nothing. A build is reached by an id alone, so confirming it exists would leak
what a publisher has not released — `mayReadBuild` / `mayPublishBuild` answer 404 (D26), the
same reason D62 gives.

Denial tests cover all of them; the nine that had none as of 2026-08-18 are in
`CatalogEndpointTest`, `MediaEndpointTest`, `PatchNoteEndpointTest` and `UploadEndpointTest`,
and the three deletes are in `RetentionEndpointTest`. They use a **public** game deliberately:
on a draft the refusal comes from `mayViewGame` and says nothing about ownership.

## The devlist

Publishing is membership in the `dev` role, which the server operator grants by hand — there is
no endpoint for it, by design (see decision D8 in `CLAUDE.md`). Permissions live in the access
token, so a freshly granted role only takes effect on the next refresh; that is the same
trade-off the authentication document describes for every permission change.

## Library

`user_games` records membership only. What is *installed* is per machine and belongs to the
client, so nothing here knows about installations — which is what has to survive a reinstall.

`PUT` is deliberately idempotent: adding a game the account already has is not an error.
`DELETE` on a game that is not in the library is a 404, because there the client's model and
the server's genuinely disagree.
