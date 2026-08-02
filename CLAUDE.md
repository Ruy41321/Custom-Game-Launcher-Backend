# CLAUDE.md — Custom Game Launcher / Backend

Context file for AI-assisted development sessions. **Read this before writing any code, and
update it at the end of every session** (see [Session protocol](#session-protocol)).

Companion repository: `Custom-Game-Launcher-Frontend` (Avalonia desktop client). Cross-cutting
contracts — API shapes, manifest format, error envelope — must stay in sync with it.

---

## 1. What this project is

An open-source, self-hostable game launcher in the style of the Epic Games Store, aimed at
indie/hobbyist developers who need to distribute in-development builds and demos to friends
and small tester groups without resorting to zip files on Discord or manual Drive links.

This repository is the **server side**: a C++ REST API, a PostgreSQL database, and a file
server for build storage, all deployed together with a single `docker-compose.yml` on a
cheap VPS. No paid content, no vendor lock-in.

---

## 2. Architecture

### Runtime topology

```
                    ┌──────────────────────────────────────────┐
   Launcher client  │  nginx  :443   TLS termination           │
   Admin (SSH tun.) │    ├── /api/v1/*   → api:8080            │
                    │    └── /files/*    → blob volume         │
                    │          (secure_link: HMAC + expiry,    │
                    │           HTTP Range for resume)         │
                    └───────────────┬──────────────────────────┘
                                    │
                      ┌─────────────▼─────────────┐
                      │  api (Drogon, C++20)      │
                      │  :8080 public             │
                      │  :9090 bound 127.0.0.1    │──► admin web GUI
                      └─────────────┬─────────────┘
                                    │
                    ┌───────────────▼──────────┐   ┌────────────────────┐
                    │  postgres:17             │   │  blob volume (CAS) │
                    │  metadata + manifests    │   │  files by SHA-256  │
                    └──────────────────────────┘   └────────────────────┘
```

### Code layers (strict, one direction only)

```
controllers/  HTTP surface. Parse + validate input, call a service, serialize the result.
              No business logic, no SQL, no error formatting.
      │
      ▼
services/     Business logic and authorization rules. Depends only on repository
              *interfaces*, never on Drogon or on SQL. This is what unit tests target.
      │
      ▼
repositories/ Data access. An abstract interface per aggregate plus a PostgreSQL
              implementation. The only place SQL is allowed to appear.
      │
      ▼
domain/       Entities and value objects. Pure C++, no dependencies at all.
```

`filters/` (Drogon middleware) and `common/` (errors, JSON helpers, logging, `Result<T>`)
sit alongside and may be used by any layer above `domain/`.

### Dependency injection

Drogon instantiates controllers itself as singletons, so it offers no DI container. We use a
**composition root**: `app/AppContext` is built once at startup, constructs the concrete
PostgreSQL repositories, injects them into services behind their interfaces, and exposes the
services. Controllers pull what they need from `AppContext::instance()`.

The point is that **services never name a concrete repository**, so unit tests construct
them with gmock fakes and touch no database.

---

## 3. Build storage and delta updates (the core mechanism)

### Content-addressed storage

Every file of every build is stored once as a blob named after its SHA-256:

```
/data/blobs/ab/cd/abcdef0123...   (2-level fan-out to keep directories small)
```

A build's **manifest** is a list of `(relative path → blob SHA-256, size, executable bit)`,
persisted in `build_files` and served as JSON.

### Why this instead of a chain of per-step delta files

| Requirement | How CAS satisfies it |
|---|---|
| Old client updates to current | Delta is the set difference of two manifests, computed on demand for **any** version pair. One hop, no cascade of N steps. |
| Retention | A blob is live while any retained manifest references it. Unchanged files are stored once across all versions. Garbage collection is a refcount sweep. |
| Resume without corruption | Per-blob HTTP `Range`. A partial blob is written to `.part`, hash-checked on completion, and discarded on mismatch — a half-written file never lands in the install. |
| Full-download fallback threshold | `delta_bytes / full_bytes > threshold` (config, default `0.7`) ⇒ server advises a full download. A truer signal than counting version steps. |
| Upload cost | The client-side publisher sends only blobs the server does not already have. |

**Known limitation:** deltas are *file-level*, not binary. A one-byte change inside a 2 GB
`.pak` re-downloads that file. Sub-file binary diffing (bsdiff/zstd) is deferred; the CAS
layout accepts it later as an additional blob kind, with no schema change.

---

## 4. Technical decisions

| # | Decision | Rationale | Alternatives rejected |
|---|---|---|---|
| D1 | **Drogon** as HTTP framework | Async, controller routing, filters (middleware), native async PostgreSQL client, built-in HTTP test client. Most of the plumbing we would otherwise hand-roll. | Crow + libpqxx (no middleware/async DB, all hand-built); oat++ (heavy macro boilerplate, weaker PG support) |
| D2 | **vcpkg manifest mode** | `vcpkg.json` committed with a pinned `builtin-baseline` ⇒ reproducible on Windows and in Docker, no global machine state. | Conan (extra Python toolchain); system packages (not reproducible) |
| D3 | **Content-addressed blob storage** | See §3. | Chained per-step delta files (storage growth, cascading downloads for old clients) |
| D4 | **Plain SQL migrations + in-repo runner** | Numbered files, `schema_migrations` table with checksums, run via `launcher-api --migrate`. Zero external tooling, no JVM, no lock-in. | Flyway/Liquibase (JVM); golang-migrate (extra binary) |
| D4a | **The migration runner talks to libpq directly, not through Drogon** | When libpq supports pipeline mode (18.x does), Drogon compiles its *batched* PostgreSQL backend, which sends everything through the extended query protocol. That protocol rejects multi-statement scripts — a migration file — with "cannot insert multiple commands into a prepared statement". libpq's simple protocol accepts them. `autoBatch=false` on `newPgClient` does **not** help: the backend is chosen at Drogon's compile time, not per client. | Splitting migration files into statements client-side (needs a parser handling dollar-quoting, string literals and comments — fragile for no gain) |
| D5 | **Argon2id via libsodium** | `crypto_pwhash` with `ALG_ARGON2ID13`, and the same audited library supplies the CSPRNG for tokens — one dependency instead of two. | bcrypt (weaker vs. GPU); raw libargon2 (no CSPRNG) |
| D6 | **JWT: short access + rotating refresh** | Access ~15 min; refresh 30 days, rotated on every use, grouped by `family_id`. Replaying a used refresh token revokes the whole family — stolen-token detection. | Long-lived access tokens (no revocation); server sessions (statefulness we do not need) |
| D7 | **nginx `secure_link` for downloads** | API signs a URL (HMAC + expiry); nginx validates it with no callback per chunk and serves `Range` natively. Downloads never occupy an API worker. | API-proxied downloads (blocks workers); `auth_request` per chunk (one API hit per range) |
| D8 | **Table-driven roles/permissions** | `roles`, `permissions`, `role_permissions`, `user_roles`. Adding a role is an INSERT, never a destructive migration. The *devlist* is simply membership in the `dev` role. | Enum column on `users` (every new role is a migration) |
| D9 | **spdlog, JSON lines** | Structured levels debug/info/warn/error with a request-id on every line, greppable in production. | Raw trantor `LOG_*` (unstructured) |
| D10 | **Docker-first builds** | The Linux container is the reference build. A first vcpkg/Drogon build on Windows takes tens of minutes and is failure-prone. | Windows-first (slow, diverges from production) |
| D11 | **Admin GUI = localhost-only web UI** | Second Drogon listener bound to `127.0.0.1:9090`, reached over an SSH tunnel. Works on a headless VPS, exposes nothing publicly. | Avalonia desktop app on the server (needs X11/VNC on a headless box) |

---

## 5. Repository layout

```
CMakeLists.txt          launcher_core static lib + thin launcher-api executable
CMakePresets.json       docker-* and windows-msvc presets
vcpkg.json              pinned dependency manifest
config/                 config.{development,staging,production}.json — no secrets, ${ENV} interpolation
migrations/             NNNN_name.sql, applied in order, never edited once merged
docker/                 api/Dockerfile, fileserver/{Dockerfile,nginx.conf}
src/
  main.cpp              entrypoint: `serve` (default) | `--migrate` | `--version`
  app/                  AppContext (composition root), ConfigLoader, Bootstrap
  controllers/v1/       HTTP surface, one controller per resource
  services/             business logic
  repositories/         I<Name>Repository interface + Pg<Name>Repository
  domain/               entities, value objects
  filters/              JwtAuthFilter, RateLimitFilter, RequestIdFilter
  common/               Error, Result<T>, JsonUtils, Logging
  migrations/           MigrationRunner
tests/
  unit/                 services against gmock repositories — no I/O
  integration/          real Drogon app + throwaway PostgreSQL database
Documentation/          one detailed document per module
```

---

## 6. Code conventions

Everything — identifiers, comments, docs, commit messages — is in **English**.

- **C++20**. Warnings are errors (`-Wall -Wextra -Wpedantic -Werror`).
- **Formatting** is `.clang-format` (LLVM base, 4-space indent, 100 columns). CI fails on any
  deviation; run `clang-format -i` before committing.
- **Naming:** `PascalCase` types, `camelCase` functions and variables, `member_` trailing
  underscore for private data, `SCREAMING_SNAKE_CASE` constants, `snake_case` files matching
  the primary type (`UserService.h` / `UserService.cpp`).
- **Interfaces** are prefixed `I` and have a virtual destructor: `IUserRepository`.
- **No raw `new`/`delete`.** `std::unique_ptr` for ownership, `std::shared_ptr` only where
  Drogon requires it, references for non-owning parameters.
- **Errors:** expected failures return `Result<T>` (`common/Result.h`); truly exceptional
  conditions throw `ApiException`. Both funnel through the single central mapper into an
  RFC 7807-style envelope — **controllers never format an error response by hand**:
  ```json
  { "type": "about:blank", "title": "Validation failed", "status": 422,
    "detail": "email is not a valid address", "requestId": "01H..." }
  ```
- **Comments** are sparing and explain *why*, never *what*. Self-explanatory code gets none.
- **SQL** lives only in `repositories/`, always parameterized — never string-concatenated.
- **Config and secrets:** every secret arrives via an environment variable. Nothing sensitive
  is ever committed; `config/*.json` holds only non-secret defaults and `${VAR}` references.
- **API versioning:** all routes under `/api/v1/`. Breaking changes open `/api/v2/`.

---

## 7. Commands

Docker is the reference path. Start Docker Desktop first.

```bash
# Full stack (api + db + fileserver), dev overrides
docker compose -f docker-compose.yml -f docker-compose.override.yml up --build -d

docker compose logs -f api
docker compose down                  # add -v to also drop the database volume

# Migrations (also run automatically on container start)
docker compose exec api /app/launcher-api --migrate

# psql is not installed on the host — go through the container
docker compose exec db psql -U launcher -d launcher -c "\dt"

# Tests. `api-build` is the toolchain image and sits behind the `tools` profile, so it is
# not started by `up`; the --profile flag is required.
docker compose --profile tools run --rm api-build ctest --test-dir build --output-on-failure
docker compose --profile tools run --rm api-build ctest --test-dir build -L unit --output-on-failure

# Formatting. CI fails on any deviation, so run this before committing.
docker compose --profile tools run --rm api-build \
    sh -c "find src tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i"
```

Optional local Windows build (CMake and Ninja ship with VS 2022 but are **not on PATH**;
`CMakePresets.json` encodes their full paths):

```bash
cmake --preset windows-msvc && cmake --build --preset windows-msvc
```

Health check:

```bash
curl -s http://localhost:8080/api/v1/health
```

---

## 8. Environment gotchas (verified on the maintainer's machine)

| Fact | Consequence |
|---|---|
| Docker Desktop is installed but its daemon is often **stopped** | Start it before any compose or integration-test command |
| CMake 3.31 and Ninja exist only inside the VS 2022 install, not on `PATH` | Use `CMakePresets.json`; do not assume bare `cmake` works |
| vcpkg is present only as the VS bundle, not bootstrapped | One-time `vcpkg-init` needed for local builds; Docker handles it itself |
| `psql` and `gh` are **not installed** | Use `docker compose exec db psql`; do GitHub work through git over SSH |
| MSVC 14.44 / VS 2022 Community is available | A local C++ build is possible but slow on first configure |
| **Never set `VCPKG_FORCE_SYSTEM_BINARIES=1`** in the build image | It makes vcpkg use the distro's CMake, which is older than the port scripts need; zlib fails to configure with a `string(JSON …)` error |
| **`vcpkg install` from the CLI writes to the *manifest* directory**, while the CMake toolchain looks in `${CMAKE_BINARY_DIR}/vcpkg_installed` | The Dockerfile must pass `-DVCPKG_INSTALLED_DIR=/src/vcpkg_installed`, or every `find_package` fails despite the dependencies being present |
| Some vcpkg ports need host tools that appear nowhere in `vcpkg.json` | `bison`/`flex` for libpq, and `autoconf`/`autoconf-archive`/`automake`/`libtool`/`gettext` for libsodium. Keep `docker/api/Dockerfile` and `.github/workflows/ci.yml` in sync |
| Drogon's batched PG backend rejects multi-statement SQL | See decision D4a; do not "simplify" the migration runner back onto `DbClient` |
| **Destroying a Drogon `DbClient` can abort with "Resource deadlock avoided"** | Its destructor joins the connection loop thread, and the last `shared_ptr` reference can end up owned *by* that thread. This showed up as intermittent `Subprocess aborted` failures in integration tests. Test setup/teardown therefore uses libpq directly (`tests/integration/TestDatabase`), and a `DbClient` is only created when a test genuinely exercises one |

---

## 9. Testing policy (non-negotiable)

1. Every feature ships with its tests in the **same commit** — unit tests for business logic,
   integration tests for API endpoints.
2. **The entire existing suite is re-run on every change.** A feature is not done until the
   full suite is green; a regression blocks the commit.
3. Unit tests (`tests/unit`, CTest label `unit`) do no I/O: services are constructed with
   gmock repositories.
4. Integration tests (`tests/integration`, label `integration`) boot the real Drogon app on
   an ephemeral port against a throwaway database that is migrated and dropped per run.
5. Security-relevant behaviour (authz, quotas, rate limits) is tested for the **denial** path,
   not just the happy path.

---

## 10. Git workflow

- All work happens on **`dev`**. Never commit to `main`.
- `main` is merged **manually by the repository owner** once work is validated — never
  propose or perform that merge.
- Atomic, well-described commits. Conventional-commit prefixes: `feat:`, `fix:`, `refactor:`,
  `test:`, `docs:`, `chore:`, `ci:`.
- Optional feature branches off `dev`, merged back into `dev` via pull request.
- CI runs on every push and pull request targeting `dev`.

---

## 11. Progress

Legend: ✅ done · 🚧 in progress · ⬜ not started

### Milestone 1 — Repository scaffolding ✅
- ✅ MIT `LICENSE`, `README.md`, `.gitignore`, `.editorconfig`
- ✅ CMake build (`launcher_core` + `launcher-api`), presets, `vcpkg.json`
- ✅ `.clang-format` / `.clang-tidy`
- ✅ Config loader with per-environment JSON and `${ENV}` interpolation
- ✅ Structured JSON logging, `Result<T>`, central error envelope
- ✅ `/api/v1/health`
- ✅ `docker-compose.yml` (api + postgres + nginx fileserver) and Dockerfiles
- ✅ GoogleTest wiring, CTest labels, GitHub Actions CI on `dev`

### Milestone 2 — Database schema ✅
- ✅ `0001_initial_schema.sql`: identity/RBAC, catalog, CAS blobs, analytics, GDPR — 19 tables
- ✅ Role and permission seed (`player` 4, `dev` 7, `admin` 11 permissions)
- ✅ `MigrationRunner` over libpq with checksum verification, exposed as `--migrate`
- ✅ Migration tests: fresh apply, idempotent re-run, checksum-mismatch abort, rollback of a
  failing migration, path-traversal rejection in `build_files`

### Verified on 2026-08-02
- 66/66 tests green (53 unit, 13 integration against a real PostgreSQL)
- `docker compose up` brings all three services to healthy; migrations apply at boot
- `/api/v1/health` 200, `/api/v1/health/ready` reports the database up, unknown routes
  return the JSON error envelope, and the file server rejects an unsigned URL with 403
- `clang-format` clean across `src/` and `tests/`
- ⚠️ The GitHub Actions workflow has **not** run yet; it is verified locally only

### Next up
- ⬜ **M3** Auth: register, email verification, login, refresh rotation, password reset,
  RBAC middleware, rate limiting
- ⬜ **M4** Catalog + Explore APIs, resumable build upload, manifest/blob ingestion, quotas
- ⬜ **M5** Delta endpoint, signed download URLs, integrity verification
- ⬜ **M9** Localhost admin web GUI
- ⬜ **M10** `Documentation/` per module, security hardening, GDPR erasure

---

## Session protocol

At the end of every working session, update:

1. **§11 Progress** — move items between ✅/🚧/⬜, add what is genuinely next.
2. **§4 Technical decisions** — append any new decision *with its rationale and the
   alternatives rejected*. Never delete a row; if a decision is reversed, add a new row that
   supersedes it and say why.
3. **§7 Commands** — add any command a future session would otherwise have to rediscover.
4. **§8 Environment gotchas** — record anything that cost time to figure out.

Keep it accurate over optimistic: a wrong progress table is worse than no progress table.
