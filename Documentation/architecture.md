# Backend architecture

Detailed per-module documents will be added alongside the modules themselves (auth,
catalog, uploads, delta updates, admin). This document covers what exists today and the
rules everything else has to follow.

## Layering

```
controllers/  → services/ → repositories/ → PostgreSQL
                    ↓
                 domain/
```

Dependencies point in one direction only.

| Layer | May depend on | Must not contain |
|---|---|---|
| `controllers/` | services, `common/` | business rules, SQL, hand-built error bodies |
| `services/` | repository *interfaces*, `domain/`, `common/` | Drogon types, SQL |
| `repositories/` | `domain/`, `common/`, Drogon ORM | business rules |
| `domain/` | nothing | I/O of any kind |

The practical test: a service must be constructible in a unit test with mock repositories
and no database, no event loop and no HTTP.

## Composition root

Drogon instantiates controllers itself as singletons, so constructor injection into
controllers is not possible. `app/AppContext` is built once during start-up, owns the
configuration and the database client, and will own service instances as they appear.
Controllers read from `AppContext::instance()`; services always receive their collaborators
through their constructor.

## Request lifecycle

1. **Pre-routing advice** assigns a request id — reusing an inbound `X-Request-Id` when a
   proxy supplied one — and stores it on the request.
2. The **controller** validates input and delegates to a service.
3. A service returns `Result<T>` for expected failures, or throws `ApiException` for
   conditions it cannot handle locally.
4. The **central exception handler** converts either into the standard envelope. Internal
   failures are logged in full and reported to the client as a bare 500 plus the request id.
5. **Post-handling advice** echoes the request id on the response and logs one structured
   line per request.

Error envelope:

```json
{
  "type": "about:blank",
  "title": "Validation failed",
  "status": 422,
  "code": "invalid_input",
  "detail": "email is not a valid address",
  "requestId": "9f1c…"
}
```

`code` is the stable machine-readable discriminator; clients switch on it, never on `title`.

## Configuration

`config/config.<environment>.json` is chosen by `LAUNCHER_ENV`. Values may contain `${VAR}`
and `${VAR:-default}`, expanded before the JSON is parsed — which is why numeric fields can
be written unquoted as `"port": ${SERVER_PORT:-8080}` and still yield a JSON number.

A `${VAR}` with neither a value nor a default is an error, not an empty string. Silent blank
secrets are the failure mode this rule exists to prevent. Use `${VAR:-}` to opt in to an
empty value explicitly.

`AppConfig::validate()` additionally refuses to start a non-development environment with a
short JWT secret, a missing file-server secret or a blank database password.

## Migrations

Numbered `migrations/NNNN_name.sql`, applied in order, recorded in `schema_migrations` with
a SHA-256 of the file.

**Migrations are immutable once merged.** The runner refuses to start when an applied
migration's checksum no longer matches, when an applied migration has vanished from disk, or
when a new migration is numbered behind the current head — that last case being what happens
when two branches each add the "next" number and both get merged.

The pure parts (`discoverMigrations`, `planMigrations`) are separated from the database work
so the policy above is unit tested without PostgreSQL.

Each migration runs inside its own transaction together with its bookkeeping row, so a
failure leaves neither schema changes nor history behind.

Run them with `launcher-api --migrate`; the container entrypoint does this at boot unless
`RUN_MIGRATIONS=false`.

## Data model

See [`migrations/0001_initial_schema.sql`](../migrations/0001_initial_schema.sql), which is
commented in place. Highlights:

- **Roles and permissions are rows, not enum values.** Adding a role is an `INSERT`. The
  operator-managed "devlist" is membership in the `dev` role.
- **`blobs` is the content-addressed store.** `build_files` maps a build's relative paths to
  blob hashes. The foreign key is `ON DELETE RESTRICT`, so a blob cannot disappear while a
  manifest still references it — garbage collection removes unreferenced blobs only.
- **`build_files.relative_path` has a CHECK constraint rejecting path traversal.** The API
  validates paths too; this is the layer that holds if that validation is ever bypassed.
- **`game_versions` stores parsed major/minor/patch** alongside the semver text, because
  ordering by the text would place `0.10.0` before `0.9.0`.
- **Analytics rows survive user erasure**: `download_events.user_id` is
  `ON DELETE SET NULL`.

## Storage and delta updates

Blobs live at `<blobRoot>/<first two hex>/<next two hex>/<full sha256>`, a two-level fan-out
that keeps directory sizes manageable.

Downloads are served by nginx, not the API: the API signs a URL (HMAC plus expiry) and
nginx's `secure_link` module validates it. No API worker is occupied for the duration of a
multi-gigabyte transfer, and `Range` requests — the basis of resume — are handled natively.

The delta between two builds is the set difference of their `build_files` rows. Because that
is computed on demand, a client on any old version reaches the current one in a single step.
When `delta_bytes / full_bytes` exceeds `updates.fullDownloadThresholdRatio` the server
advises a full download instead.

Deltas are currently file-level: a changed file is fetched whole. Sub-file binary diffing is
a later addition that needs no schema change.

## Logging

spdlog, one JSON object per line, level from configuration. Any dynamic value embedded in a
message must go through `common::escapeJson` so a quote in user input cannot break the line.

## Testing

| Suite | CTest label | Needs |
|---|---|---|
| `tests/unit` | `unit` | nothing |
| `tests/integration` | `integration` | PostgreSQL via `LAUNCHER_TEST_DB_*`; self-skips otherwise |

Integration tests create and drop a uniquely named throwaway database per test, so they
never observe each other's state.
