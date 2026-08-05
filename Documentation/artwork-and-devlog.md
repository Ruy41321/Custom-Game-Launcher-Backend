# Artwork and the devlog

Covers the two surfaces a game has besides its builds: the pictures that describe it, and the
posts its publisher writes about it. Implemented in `src/services/{Media,PatchNote}Service.*`,
`src/domain/{Media,PatchNote}.*`, `src/storage/MediaStore.*`,
`src/repositories/postgres/Pg{Media,PatchNote}Repository.*` and
`src/controllers/v1/{Media,PatchNote}Controller.*`.

Both share the catalog's authorization rules — `domain::mayViewGame` and `domain::mayEditGame`,
the pure functions described in [catalog.md](catalog.md) — rather than re-deriving them, so no
service can drift from another on who may see or edit a game.

---

## Part 1 — Artwork

### Endpoints

Reading needs a bearer token like the rest of the catalog. The URLs that come back do **not**:
see [Why artwork is public](#why-artwork-is-public-and-blobs-are-not).

| Method | Path | Permission | Purpose |
|---|---|---|---|
| GET | `/api/v1/games/{idOrSlug}/media` | `game.read` (+ visibility) | Everything hanging off the game |
| POST | `/api/v1/games/{idOrSlug}/media` | `game.publish` + ownership | Upload one image |
| PATCH | `/api/v1/media/{mediaId}` | `game.publish` + ownership | Alt text and position only |
| DELETE | `/api/v1/media/{mediaId}` | `game.publish` + ownership | Remove one image |

Artwork is exactly as visible as the game it belongs to. A picture on a draft is a 404 to
everybody but its publisher, for the same reason the draft itself is.

### Uploading

The image is the **raw request body**, not a multipart form. There is one file and no other
field that has to travel with it, so the descriptive parts are query parameters and the body
stays byte-for-byte the thing that gets hashed and stored.

```
POST /api/v1/games/{id}/media?kind=cover&altText=Key%20art&sortOrder=0
Content-Type: image/png          <- recorded nowhere, see below
<the bytes>
```

| Parameter | Default | Notes |
|---|---|---|
| `kind` | `screenshot` | `cover`, `banner`, `logo` or `screenshot` |
| `altText` | empty | At most 300 characters. What a screen reader reads out |
| `sortOrder` | 0 | Position in the gallery; meaningless for the singleton kinds |

Four rules decide whether the bytes are stored at all, and all four are checked before anything
is written:

1. **Size.** `media.maxBytes`, 5 MiB by default. Rejected with 413.
2. **Format, decided by the leading bytes.** PNG, JPEG and WebP, identified by signature. The
   uploader's `Content-Type` header is not consulted at all — see
   [What an image is](#what-an-image-is-is-decided-by-the-bytes).
3. **Gallery size.** At most 12 screenshots per game. A gallery, not an archive: the cap is
   what stops one game filling the media volume.
4. **One of each identity kind.** A game has one cover, one banner and one logo. Enforced by a
   partial unique index in migration 0003, not by the service, so no route present or future
   can create a second cover.

`PATCH` changes the alt text and the sort order — the two fields that *describe* an image
rather than being one. Replacing the picture means uploading a new one; there is no route that
swaps bytes under an existing id, because the id's whole meaning is the content it points at.

### Storage layout

Content-addressed, exactly like a build blob, but under a root of its own:

```
/data/media/ab/cd/abcdef0123….png        <- media.root, served publicly
/data/blobs/ab/cd/abcdef0123…            <- storage.blobRoot, signed URLs only
```

The storage key is `ab/cd/<sha256>.<ext>` and is derived from the hash, never from anything the
client sent. The extension is part of the key so nginx answers with a usable `Content-Type`
from its own mime table instead of `application/octet-stream`.

Being content-addressed means two games with the same picture are one file — which is the
point, and also the reason **deleting a row is not deleting a file**. Every delete asks whether
any row still points at that storage key, and removes the file only when none does. Without
that check, removing one game's cover would blank the other game's.

### Why artwork is public, and blobs are not

A cover is public by definition. Signing it would mean minting one expiring signature per card
in an Explore grid: none of them cacheable, and some of them expiring while somebody was
looking at the page.

That is only safe because the root is **separate**. A public nginx location over `/data/blobs`
would hand out every build to anyone who learned a hash. The two roots are the security
boundary, and the nginx configuration keeps it narrow on its side too — the media location is a
*regex* location that matches only the shape the API can have written:

```nginx
location ~ "^/media/([0-9a-f]{2})/([0-9a-f]{2})/([0-9a-f]{64}\.(?:png|jpg|webp))$" {
    alias /data/media/$1/$2/$3;
    add_header Cache-Control "public, max-age=31536000, immutable" always;
    add_header X-Content-Type-Options "nosniff" always;
    add_header Content-Security-Policy "default-src 'none'; sandbox" always;
}
```

Anything else under `/media/` falls through to a 404. The immutable cache lifetime is honest:
a file named after the hash of its own contents can never change.

### What an image is, is decided by the bytes

`domain::sniffImageFormat` reads the leading bytes and the declared `Content-Type` is ignored
entirely. The reason is narrow and worth stating: the answer becomes the `Content-Type` of a
**public URL**, so it cannot be something the uploader chose.

**SVG is refused on purpose.** It is a document format that can carry script, and served from a
public unsigned location it would be a stored cross-site-scripting vector rather than a
picture. The `Content-Security-Policy` above is the second line of that defence, not the first.

### `coverUrl` and `media`

The cover rides on the **game** — `gameToJson` resolves it with a correlated subquery — so an
Explore grid gets one picture per card without a second request per result. It is an empty
string when there is no cover, rather than an absent key, so a client reads the field either
way instead of checking that the key exists.

The full list lives on the **detail** response, under `media`. A game detail is a fixed-size
document and a gallery is bounded at 12, so it fits; the devlog does not, which is why it is
paged separately.

---

## Part 2 — The devlog

### Endpoints

| Method | Path | Permission | Purpose |
|---|---|---|---|
| GET | `/api/v1/games/{idOrSlug}/patch-notes` | `game.read` (+ visibility) | Paged, newest first |
| POST | `/api/v1/games/{idOrSlug}/patch-notes` | `patchnote.write` + ownership | Write one |
| PATCH | `/api/v1/patch-notes/{noteId}` | `patchnote.write` + ownership | Edit, publish or withdraw |
| DELETE | `/api/v1/patch-notes/{noteId}` | `patchnote.write` + ownership | Remove one |

`page` defaults to 1 and `pageSize` to 20, clamped to 100. An unreadable page number falls back
to the default rather than failing — a stale or hand-typed query string should still return a
devlog. The response is the usual `{ items, total, limit, offset }` envelope.

Published entries are visible to anyone who can see the game. **Drafts are visible only to
somebody who could edit it**, which is the same rule game detail applies to unpublished
versions.

### A patch note is not a version's release notes

`game_versions.release_notes` describes exactly one version and is written by whoever published
it. A devlog entry is a different thing:

- it **may name a version, or none at all** — "what we are working on this month" is a
  legitimate post;
- it has a **publication state of its own**, so a draft can be written before the build it
  talks about exists;
- it can be **withdrawn**: publishing and unpublishing are one field, because a note that went
  out by mistake has to be able to come back.

Re-publishing keeps the original `published_at`: the date is when readers saw it, not when it
was last edited.

When `versionId` is set it must name a version of the *same* game. That is checked in the
service rather than left to the foreign key, so naming somebody else's version is a 404 and not
a constraint violation surfacing as a 500.

### Fields and limits

| Field | Rule |
|---|---|
| `title` | 1–200 characters after trimming |
| `bodyMarkdown` | at most 40 000 characters |
| `versionId` | optional; an empty string on PATCH detaches the note from its version |
| `publish` / `published` | boolean; false is a draft |

The body is stored as Markdown and served as it was written. **The server renders nothing**,
and the launcher shows it as text: rendering remote markup is a decision with consequences, and
a devlog does not need one.

---

## What no client does yet

The **read** side of both surfaces is consumed by the launcher as of 2026-08-05: covers on
Explore and library cards, a banner-or-cover hero, a screenshot gallery, and a paged devlog on
the game page.

The **write** side is not. No screen uploads an image or writes a devlog entry; a publisher
does both with `curl` today. That is tracked as an open debt in `HANDOFF.md` alongside the
developer dashboard's other missing edits.

---

## Configuration

```json
"media": {
  "root":          "${MEDIA_ROOT:-./data/media}",
  "publicBaseUrl": "${MEDIA_PUBLIC_BASE_URL:-http://localhost:8081/media}",
  "maxBytes":      5242880
}
```

`publicBaseUrl` is prefixed to a storage key to build the URL a client is given. Nothing signs
it, so moving the deployment behind another hostname is a configuration change and invalidates
nothing — the same property signed blob URLs get by covering only the path (decision D22).

Note that `maxBytes` is not announced anywhere a client can read, which is the same gap the
upload chunk size has; it is recorded as an open debt.

## Related documents

- [catalog.md](catalog.md) — games, versions, visibility and the ownership rules these reuse
- [builds-and-uploads.md](builds-and-uploads.md) — the other content-addressed store, and why
  that one is never public
- [storage-lifecycle.md](storage-lifecycle.md) — what deletes a build, and what reclaims the
  files nothing references any more
