# Storage lifecycle: deletion, collection and quota

What happens to bytes after they are uploaded: how a publisher removes a build or a version,
what reclaims the files nothing references any more, and how the upload quota gets its space
back. Implemented in `src/services/RetentionService.*`, `src/services/CatalogService.*`
(`deleteBuild`, `deleteVersion`), `src/repositories/postgres/PgBlobRepository.*` and
`src/app/Bootstrap.cpp` (the timers).

Read [builds-and-uploads.md](builds-and-uploads.md) first: this document assumes the
content-addressed store and the quota accounting it describes.

## The shape of the problem

A build's files are blobs shared by every build that contains the same bytes, so **deleting a
build is not deleting files**. `build_files.blob_sha256` is `ON DELETE RESTRICT`, which means a
referenced blob was never at risk of going away. The gap this closes is the other half: until a
collector existed, nothing removed the *unreferenced* ones either. Deleting a build freed no
disk and no quota, and an upload that was never finalised was stored and paid for forever.

So deletion happens in two steps, deliberately far apart in time:

```
1. A publisher deletes a build or a version   -> rows go, files stay
2. A timer sweeps blobs nothing references    -> files go, quota comes back
```

## Deleting a build or a version

| Method | Path | Permission | Effect |
|---|---|---|---|
| DELETE | `/api/v1/builds/{buildId}` | `build.upload` + ownership | The build and its `build_files` rows |
| DELETE | `/api/v1/games/{gameId}/versions/{versionId}` | `game.publish` + ownership | The version and, by cascade, every build under it |
| DELETE | `/api/v1/games/{idOrSlug}` | ownership | The game and everything under it — see below |

Both follow the catalog's 404-not-403 rule. A build whose game the caller cannot see is
reported missing rather than refused, using the same `domain::mayReadBuild` /
`mayPublishBuild` pair the upload and download sides use — so this route cannot disagree with
them about who may know a draft's builds exist. A version of another game is likewise a 404:
the caller was told about a path that does not exist, not one they may not use.

Neither route touches the filesystem. That is not laziness, it is the only ordering that is
safe with content-addressed storage: the same blob may still belong to three other builds, and
the question of whether it does is asked once, later, by the collector.

## Deleting a game

One `DELETE` removes the game row, and the database removes everything hanging off it: versions,
builds, `build_files`, artwork rows, patch notes, library entries, and the game's
`download_events`. The blobs those manifests named are left where they are, for the collector to
reclaim one grace period later — exactly as when a single build goes.

Three decisions are worth stating, because none of them is forced by the schema.

### It is allowed while other people hold the game in their library

A library entry is a bookmark, not a licence. Nothing was paid for, and refusing while any
entry exists would mean one stranger adding a game could permanently stop its publisher from
withdrawing their own work. So `user_games` cascades away and the delete goes through.

What was already **installed** keeps working: an install is a directory of files on somebody's
machine, and this server never knew about it. What stops working is *updating* and *verifying*
it, and both answer **404** rather than 403, because after the delete there genuinely is no such
game. The client is expected to show that as "no longer available", never as a permissions
problem — the same rule the catalog applies to drafts.

A publisher who wants a title to stop being visible without destroying it has `visibility:
"draft"` already, which is why this route does not need a softer form.

### The artwork goes with it, the shared picture does not

Images are content-addressed, so two games with the same cover are one file, and the row going
away says nothing about whether the bytes are still in use. The delete therefore returns the
storage keys of the rows it cascaded away and asks about each one before touching the disk —
the same question `MediaService` asks when a publisher removes one picture, asked from the same
place: `services::MediaReclaimer`, which both services hold.

The keys come out of the *same statement* that removes the game, from a sub-query reading the
pre-command snapshot. Reading them afterwards would find nothing, and reading them in a separate
statement first would open a window in which a new cover could be uploaded and then have its
file deleted out from under it.

### The download history goes too

`download_events.game_id` is `ON DELETE CASCADE`, so a deleted game takes its rows out of the
operator's analytics. That is the schema's answer rather than a preference, and it is a real
consequence: totals on the admin console fall when a publisher deletes a game. The alternative —
keeping events whose subject no longer exists — would need a migration and a report that can
name a game it cannot join to.

Note the contrast with account erasure, where `user_id` goes null and the row survives: an
erasure removes a *person* from data that is still about something, and this removes the thing
itself.

## The collector

`RetentionService::collectUnreferencedBlobs` runs on a timer (`retention.sweepIntervalSeconds`,
hourly by default) and is the only thing that removes a blob file. Four properties are
load-bearing; none is a tuning knob.

### 1. The grace period is correctness, not tuning

A publisher uploads every blob of a build **before** submitting the manifest that names them.
During that window, perfectly live content is referenced by nothing at all. A sweep with no
grace period eats builds in flight.

`retention.blobGraceSeconds` therefore has to be longer than the slowest publish the deployment
expects, and defaults to 86 400 seconds — the same day the upload sessions themselves get.

### 2. The row goes first, the file second

The opposite order is the one that cannot be recovered from. If the file were removed and the
delete then lost a race against a build that had just taken that blob, a live manifest would
point at nothing and every download of it would fail.

This way, the worst a crash between the two steps can leave behind is a file nothing
references — which costs disk and nothing else, and which the next sweep will not even find,
because the row is already gone. (That orphan is real: it is the one case where the sweep
cannot clean up after itself. It is bounded by how often the process dies mid-sweep.)

### 3. The delete repeats its own condition

`deleteIfUnreferenced` re-checks *inside the statement* that the blob is still unreferenced and
still older than the grace period. Between listing the candidates and deleting one, a build can
legitimately have taken it. Repeating the condition turns that race into a no-op — the sweep
reports it as `skipped` — instead of a foreign-key violation surfacing as an error.

### 4. It refunds the quota

Quota is charged when an upload completes (decision D17). Without a refund it would be a
**lifetime cap** rather than an allowance: deleting a build would free the disk and leave the
publisher still paying for it. The collector calls `releaseUpload` for the account that
uploaded the blob, using the size it is about to reclaim.

`blobs.uploaded_by_user_id` is nullable, so a blob whose uploader has since been erased is
collected without a refund rather than skipped.

### Batching

`retention.sweepBatchSize` (500) caps one pass. A sweep that tried to reclaim a terabyte in one
go would hold an event loop for the whole of it; the next pass picks up the rest.

## Artwork is collected differently

Images are content-addressed too and shared the same way, but they are **not** swept on a
timer: `MediaService` asks, on every delete, whether any row still points at the storage key
and removes the file when none does.

The difference is that a picture is uploaded in one request that either creates its row or
does not, so there is no window in which live content is referenced by nothing — the window the
grace period exists for does not exist here. See
[artwork-and-devlog.md](artwork-and-devlog.md).

## Abandoned uploads

A second, older timer (`uploads.sweepIntervalSeconds`, ten minutes) expires upload sessions
past `uploads.sessionTtlSeconds` and discards their staging files. That reclaims scratch space
promptly; the blobs of a *completed* upload that never became a manifest are the collector's
problem, one grace period later.

Staging disk is bounded independently of quota, by
`maxOpenSessionsPerUser × maxBlobBytes` — 16 × 2 GiB by default.

## What does not exist: automatic retention

Nothing decides on its own that an old build may go. There is no policy, no "keep the last N",
no age limit — only what a publisher deletes by hand, and the collector picking up after them.

That is a deliberate gap rather than an oversight, and the shape it should take is already
decided: **keep the N most recent builds per (game, platform, architecture)**, with a default
that deletes nothing, so an operator who never configures it never loses a build to a policy
they did not know about. The place is `RetentionService`, beside the collector, and the
implementation is a delete driven by a window function — after which the existing collector
reclaims the storage with no new code at all.

Deleting a build is safe for clients in a way an in-place mutation would not be: the build
stops appearing in game detail, so a launcher picks the newest one that is still there, and a
plan that names a build which has gone is a 404 rather than a plan referring to files that are
not on disk any more.

## Configuration

```json
"retention": {
  "blobGraceSeconds":     86400,
  "sweepIntervalSeconds": 3600,
  "sweepBatchSize":       500
},
"uploads": {
  "sessionTtlSeconds":    86400,
  "sweepIntervalSeconds": 600,
  "maxOpenSessionsPerUser": 16,
  "maxBlobBytes":         2147483648
}
```

Lowering `blobGraceSeconds` below the time a large publish takes on the deployment's uplink is
the one change here that can destroy data. Everything else trades promptness for load.

## Related documents

- [builds-and-uploads.md](builds-and-uploads.md) — how blobs arrive, and how quota is charged
- [artwork-and-devlog.md](artwork-and-devlog.md) — the other content-addressed store
- [catalog.md](catalog.md) — ownership and the 404-not-403 rule these deletes follow
