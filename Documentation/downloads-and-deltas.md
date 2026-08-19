# Downloads, deltas and integrity

How a build gets from the server onto a player's machine, and how the client finds out whether
what it ended up with is actually the build. Implemented in `src/services/DownloadService.*`,
`src/domain/Delta.*`, `src/storage/DownloadUrlSigner.*`,
`src/repositories/postgres/PgDownloadRepositories.*` and `src/controllers/v1/DownloadController.*`.

The publishing half — blobs, resumable uploads, manifests — is in
[builds-and-uploads.md](builds-and-uploads.md). The storage rationale is in `CLAUDE.md` §3.

## The shape of an update

```
 1. POST /api/v1/builds/{id}/download   {"fromBuildId": "…"}   -> a plan, with signed URLs
 2. GET  <url> for each file in the plan                       -> nginx, with Range and resume
 3. POST /api/v1/builds/{id}/verify     {"files": […]}         -> did it all arrive intact
```

The API never serves a byte of a build. It answers with URLs the file server validates on its
own, so a multi-gigabyte transfer never occupies an API worker and `Range` — the basis of a
resumed download — is handled by nginx natively.

## Endpoints

| Method | Path | Permission | Purpose |
|---|---|---|---|
| POST | `/api/v1/builds/{id}/download` | `game.download` | What to fetch, and where from |
| POST | `/api/v1/builds/{id}/verify` | `game.download` | Compare an install against the manifest |

Both are POST although neither changes a build: they mint signed URLs, and the plan records
that a download was handed out. Neither is cacheable and neither is free of consequence, which
is exactly what a GET would promise.

A build the caller may not see is **404**, never 403 — the same rule the rest of the catalog
follows, for the same reason: a 403 would confirm the build exists. A build that has not
finished uploading is also 404: from the outside there is nothing there yet.

**Who may see a build** is `domain::mayReadBuild`, and it asks two things: the game is not a
draft, **and** the version the build hangs off has been published. Either half missing makes
the build its publisher's alone — a public game may perfectly well carry a version nobody has
released yet, and `PATCH …/versions/{id}` can withdraw one that was. Until 2026-08-17 only the
first half was asked, so a build under an unpublished version was downloadable by anybody who
could name it, while the game's own detail response had been filtering that version out of the
listing all along (D62).

## The plan

```json
{
  "buildId": "…", "gameId": "…", "versionId": "…",
  "kind": "delta",
  "manifestSha256": "86e1…", "entrypoint": "Game.exe", "launchArgs": "--fullscreen",
  "files": [
    { "path": "Game.exe", "sha256": "53e5…", "size": 21, "executable": true,
      "url": "http://…/files/53/e5/53e5…?token=PaO-mGHwdduNpc2QdLDFUA&expires=1785774748" }
  ],
  "unchanged": [ { "path": "data/pak", "sha256": "8430…", "size": 56, "executable": false } ],
  "remove": ["old.dll"],
  "downloadBytes": 21, "totalBytes": 77,
  "urlsExpireAt": "2026-08-03T16:32:28Z"
}
```

`fromBuildId` is what the client currently has installed; leaving it out asks for a first
install. It has to be a build of the **same game**, or the request is a 422 — an update from
something unrelated is a mistake, not a plan.

A source the caller may **not read** is the one case that is neither an error nor a delta: the
plan falls back to a full download of the target. The source is a claim about what is already
on somebody's disk and nothing about it reaches the answer except which bytes may be skipped,
so refusing would only leave a player who installed a version that was withdrawn afterwards
unable to update at all — a larger consequence than the withdrawal chose. Nothing is skipped on
the strength of a build the caller may not read, and `download_events.from_version_id` is left
empty for it.

`downloadBytes` is what the transfer is expected to cost and `totalBytes` is the build as
installed. They differ because paths are the unit of the plan while blobs are the unit of the
transfer: two files with identical content are two entries and one download.

### Deltas

The delta is the difference between two manifests, computed on demand. Nothing is precomputed
and nothing is chained, so a client on **any** old build reaches the current one in one hop
rather than replaying every release in between.

A file is unchanged when the same path holds the same content address. Anything else is
fetched, and a path the target build does not have at all is listed under `remove` — an update
replaces an install rather than accumulating on top of it.

### `copyFrom`: content that is already on disk

A file that merely moved between builds does not have to travel. When the content of a file to
fetch also lives at some path the target build **keeps unchanged**, the entry carries
`"copyFrom": "<that path>"` and its bytes are excluded from `downloadBytes`.

The restriction to paths the update keeps is the whole design: the source of the copy is then
neither deleted nor overwritten by any other step of the same plan, so the client may perform
it whenever it likes. Offering a copy from a path the update itself replaces would make
correctness depend on the order the client happened to apply things in — a bug that would only
appear on some machines, some of the time.

The `url` is filled in either way. `copyFrom` is an optimisation, never an instruction, so a
client that cannot find the local copy is never stuck.

### When a delta stops being worth it

When `downloadBytes / totalBytes` exceeds `updates.fullDownloadThresholdRatio` (default `0.7`)
the server returns `"kind": "full"` instead: every file, and `unchanged` empty. Past that point
a delta costs nearly as much as the build and a full download is the simpler thing for a client
to get right. `remove` still lists what the previous version left behind.

## Signed URLs

nginx's `secure_link` module validates a signature the API produces:

```
token = base64url( md5( "<expires><uri> <secret>" ) )
url   = <publicBaseUrl>/<ab>/<cd>/<sha256>?token=<token>&expires=<unix seconds>
```

MD5 is not a choice made here — it is the module's wire format. The construction puts the
secret **last**, so the length-extension weakness of a prefix-keyed MD5 does not apply: forging
a URL means recovering the secret, not extending a digest.

Only the **path** is signed, because `$uri` is all nginx sees. The same deployment therefore
keeps working when it is fronted by a different hostname or moved behind TLS, and
`storage.publicBaseUrl` can change without invalidating anything already minted.

The file server distinguishes the two failure modes: a signature that does not verify is
**403**, one that verifies but has passed its expiry is **410**. Worth knowing when debugging —
tampering with `expires` in a URL by hand gives 403, not 410, because it invalidates the
signature too.

`storage.signedUrlTtlSeconds` (default one hour) has to be long enough for a large file on a
slow line: the URL is checked when the request starts, not while it runs, but a resumed
download issues a *new* request. A client that has been interrupted for longer than the TTL
asks for a fresh plan.

## Integrity verification

`POST /api/v1/builds/{id}/verify` takes what the client found on disk and compares it against
the manifest, which is the authority on what an install must look like however it got that way
— a truncated download, a failing disk, a modified file.

```json
{ "files": [ { "path": "Game.exe", "sha256": "53e5…" } ] }
```

```json
{ "intact": false,
  "missing": ["data/pak"], "corrupt": ["Game.exe"], "unexpected": ["leftovers.log"],
  "repair": [ { "path": "…", "sha256": "…", "size": 56, "url": "…" } ],
  "repairBytes": 77, "urlsExpireAt": "…" }
```

* **missing** — in the manifest, not in the install. A file the client could not read is
  reported by leaving it out, which lands it here. Sending something that is not a hash is a
  422: that is a client bug, not a damaged install.
* **corrupt** — present, but not the content the manifest names.
* **unexpected** — in the install, not in the manifest. These do **not** make an install
  broken, and `intact` ignores them: an install directory legitimately accumulates saves,
  configuration and logs. They are reported so the client can decide, not so the server can
  order a deletion.

`repair` is the missing and corrupt files with fresh signed URLs, so a repair needs no second
round trip to the plan endpoint.

## Analytics

A plan that has something to fetch writes one `download_events` row: game, build, user, the
version the client was coming from, `full` or `delta`, and the bytes the plan expected to
transfer. How many installs are stuck on an old build is exactly the question a small publisher
wants answered, which is why the source version is kept.

Two things it deliberately is not. A plan with nothing to fetch — a client confirming it is
already up to date — is not recorded, because counting it would make the statistics say the
opposite of the truth. And `bytes_planned` is what was *planned*: the file server answers the
transfer itself and never reports back, so nothing here claims to be a byte count of what
actually moved.

Verification is not recorded either. A repair is a repair, and folding it into the download
counts would inflate them.

## Configuration

| Key | Default | Meaning |
|---|---|---|
| `storage.publicBaseUrl` | `http://localhost:8081/files` | Where the file server answers, path included |
| `storage.secureLinkSecret` | dev placeholder | Shared with nginx as `FILE_SECURE_LINK_SECRET`; required outside development |
| `storage.signedUrlTtlSeconds` | 3600 | How long a minted URL stays usable |
| `updates.fullDownloadThresholdRatio` | 0.7 | Past this, a delta becomes a full download |

## Not yet implemented

Sub-file binary diffing. A one-byte change inside a 2 GB `.pak` still re-downloads that file;
the CAS layout accepts bsdiff/zstd later as another blob kind, with no schema change.

Garbage collection of unreferenced blobs is still outstanding, and matters more now that
something depends on blobs being present when a plan names them.
