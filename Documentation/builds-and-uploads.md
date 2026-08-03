# Builds, blobs and resumable uploads

Covers how a build gets from a publisher's machine into content-addressed storage, and how the
manifest that describes it is produced and served. Implemented in `src/services/UploadService.*`,
`src/storage/BlobStore.*`, `src/domain/Manifest.*`, `src/repositories/postgres/PgCatalogRepositories.*`
and `src/controllers/v1/{Build,Upload}Controller.*`.

The catalog side — games, versions, visibility — is documented in [catalog.md](catalog.md).
The storage rationale is in `CLAUDE.md` §3.

## The publish flow

Publishing is four steps, and the split is the point: negotiation is what keeps an update
proportional to what actually changed.

```
 1. POST /api/v1/games/{id}/versions/{versionId}/builds   -> build, status "uploading"
 2. POST /api/v1/builds/{id}/blobs/missing                -> which blobs the server lacks
 3. for each missing blob:
        POST  /api/v1/builds/{id}/uploads                 -> an upload session
        PATCH /api/v1/uploads/{sessionId}   (repeat)      -> chunks, resumable
 4. POST /api/v1/builds/{id}/manifest                     -> build, status "ready"
```

A second build that changes one file re-uploads one file: step 2 answers with only that blob,
because every other file already exists under its content address.

## Endpoints

| Method | Path | Permission | Purpose |
|---|---|---|---|
| POST | `/api/v1/builds/{id}/blobs/missing` | `build.upload` + ownership | Which of these blobs is the server missing |
| POST | `/api/v1/builds/{id}/uploads` | `build.upload` + ownership | Open an upload session for one blob |
| GET | `/api/v1/uploads/{sessionId}` | session owner | How many bytes the server has |
| PATCH | `/api/v1/uploads/{sessionId}` | session owner | Send the next chunk |
| DELETE | `/api/v1/uploads/{sessionId}` | session owner | Give up on an upload |
| POST | `/api/v1/builds/{id}/manifest` | `build.upload` + ownership | Finalize the build |
| GET | `/api/v1/builds/{id}/manifest` | `game.download` | The manifest, as the bytes its hash covers |

A build the caller does not own is reported **404**, not 403 — build ids are opaque, and saying
"forbidden" would confirm one exists.

## The transfer protocol

Deliberately the same shape as [tus](https://tus.io): `GET` reports the offset, `PATCH` sends
the next chunk with an `Upload-Offset` header, `DELETE` abandons the upload. Every response
carries `Upload-Offset` as a header as well as in the body.

```http
PATCH /api/v1/uploads/8f2c… HTTP/1.1
Authorization: Bearer eyJ…
Upload-Offset: 4194304
Content-Type: application/offset+octet-stream

<bytes>
```

`Upload-Offset` is **required**. Defaulting it to the server's current offset would be the one
mistake a resumed upload cannot recover from: a client that lost track of its progress would
silently duplicate or skip a range, and the hash check would only catch it after the whole file
had been sent. A `PATCH` at the wrong offset is refused with `409` and a detail naming the real
one, so a confused client recovers from the error itself.

The **database**, not the file on disk, is the authority on that offset. Advancing it is a
single conditional statement:

```sql
UPDATE upload_sessions SET received_bytes = received_bytes + $3
WHERE id = $1::uuid AND status = 'pending' AND expires_at > now()
  AND received_bytes = $2 AND received_bytes + $3 <= declared_size_bytes
RETURNING …
```

Two chunks racing at the same offset cannot both match `received_bytes = $2`, so only one of
them is ever told to write. The reservation happens *before* the write, which is why a
concurrent pair cannot overwrite each other's range.

If the write itself then fails, the database says more bytes arrived than the file holds. That
session is aborted immediately rather than left to fail its hash check much later.

## Content-addressed storage

A blob lives at `<blobRoot>/ab/cd/<sha256>` — two levels of fan-out taken from the first four
hex characters, so no single directory holds every file in the deployment. `BlobStore` refuses
to build a path from anything that is not 64 lowercase hex characters, which is what keeps a
crafted "hash" from escaping the root.

Uploads are assembled under `<blobRoot>/staging/<sessionId>.part`. Staging lives **inside** the
root on purpose: the final move is then a rename within one filesystem, which is atomic. A
staging directory elsewhere would silently degrade to a copy, and a crash mid-copy would publish
a truncated blob.

A blob only reaches its content address once its bytes have been hashed and matched. This is the
single most important rule in the upload path: a corrupted transfer, a truncated resume or a
tampered file is discarded, never stored. A blob that is already present is treated as success
without a move — identical content addresses are identical content.

## Quotas

`users.upload_quota_bytes` is a cumulative cap across everything an account has ever uploaded,
default 5 GiB, adjustable per user from the admin application.

The binding check is at the end of the upload, not at the start, and it is one statement:

```sql
UPDATE users SET upload_used_bytes = upload_used_bytes + $2
WHERE id = $1::uuid AND upload_used_bytes + $2 <= upload_quota_bytes
RETURNING upload_used_bytes
```

A read followed by a write would let two uploads finishing at once each see the same free space
and both take it. Here at most one of them updates the row; an empty result *is* the refusal.

Opening a session also checks the quota, but that check is advisory — a fast rejection before
megabytes travel, not the authority.

An account is charged only for bytes that actually became new storage:

* content the server already held → discarded, session completed, **nothing charged**;
* bytes that fail their hash → discarded, **charge refunded**;
* losing the race to record the row → the bytes are identical either way, **charge refunded**.

Staging disk is bounded separately, by `uploads.maxOpenSessionsPerUser` (default 16) together
with `uploads.maxBlobBytes` (default 2 GiB): one account can hold at most that product in
unfinished uploads.

## Abandoned uploads

A session expires after `uploads.sessionTtlSeconds` (default 24 h). A timer registered at boot
runs `UploadService::sweepExpiredSessions` every `uploads.sweepIntervalSeconds` (default 10 min),
deleting each expired session's staging file and then its row, in batches of 100.

The sweep is safe alongside live uploads because a session is only swept once it is past its
expiry, and no chunk is accepted for an expired session anyway.

## The manifest

Submitting the manifest flips the build to `ready`. It is one statement, with the file insert
gated on the status update:

```sql
WITH updated AS (
    UPDATE builds SET status = 'ready', … WHERE id = $1::uuid AND status = 'uploading'
    RETURNING *
), inserted AS (
    INSERT INTO build_files (…)
    SELECT u.id, t.entry->>'path', … FROM updated u, jsonb_array_elements($7::jsonb) AS t(entry)
)
SELECT … FROM updated
```

Two reasons for one statement rather than a transaction. A Drogon transaction commits
asynchronously when its object is destroyed, so a build could be reported ready before its rows
are durable (see gotcha in `CLAUDE.md` §8). And gating the insert on the update's result is what
makes a second, concurrent finalize a no-op instead of a duplicate-key error — the second caller
gets a `409`.

**File sizes are read back from `blobs`, never taken from the request.** The declared size is
the publisher's claim; the stored size is the fact, and a build must not be able to advertise a
download size its blobs do not have.

### The canonical document

`builds.manifest_sha256` is the SHA-256 of an exact byte sequence, and `GET …/manifest` serves
exactly those bytes with the hash in `X-Manifest-Sha256`. A client verifies its download by
hashing the response — it never has to reproduce a canonical form of its own.

```json
{"schema":1,"entrypoint":"Game.exe","launchArgs":"--fullscreen","files":[{"path":"Game.exe","sha256":"…","size":51,"executable":true}]}
```

Entries are sorted by path, keys are emitted in a fixed order, and there is no insignificant
whitespace. The serialiser is hand-written rather than delegated to jsoncpp precisely because
this is a wire contract: it must not shift if the JSON library ever changes how it emits
escapes.

The build id is deliberately **absent**. Two builds with identical content then carry identical
manifest hashes, which is what makes the hash usable as a content identity.

## Path safety

Manifest paths are validated twice, on purpose. `domain::validateRelativePath` rejects absolute
paths, drive letters, backslashes, `.` and `..` segments, empty segments and control characters,
and produces an error naming the offending path. The `build_files_relative_path_safe` CHECK
constraint enforces the same rules in the database, so a malicious manifest is unstorable even
if the service check is ever bypassed.

Duplicate paths are a `409`, not a last-one-wins merge: either could be the one the publisher
meant.

## Configuration

| Key | Default | Meaning |
|---|---|---|
| `uploads.defaultQuotaBytes` | 5 GiB | Schema default for a new account |
| `uploads.maxBlobBytes` | 2 GiB | Largest single file in a build |
| `uploads.maxChunkBytes` | 8 MiB | Largest request body; also fixes Drogon's own body limit |
| `uploads.sessionTtlSeconds` | 86400 | How long an interrupted upload stays resumable |
| `uploads.maxOpenSessionsPerUser` | 16 | Bounds staging disk per account |
| `uploads.sweepIntervalSeconds` | 600 | How often abandoned sessions are reclaimed |

`app::configureUploadLimits` raises Drogon's `setClientMaxBodySize` and
`setClientMaxMemoryBodySize` to `maxChunkBytes` plus headroom. Drogon's default is one megabyte
— well under a chunk — so without this every chunk is rejected before reaching the controller.
The integration harness calls the same function, so the tests never run under limits nobody
deploys.

## Not yet implemented

Downloading. The manifest tells a client which blobs it needs, but the signed download URLs
(nginx `secure_link`, HTTP `Range`) and the delta endpoint that computes the set difference
between two manifests are milestone 5.

Garbage collection of unreferenced blobs is also outstanding: `build_files.blob_sha256` is
`ON DELETE RESTRICT`, so nothing can remove a referenced blob, but nothing yet sweeps the
unreferenced ones.
