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
| D12 | **Coroutines through controllers, services and repositories** | Controllers run *on* Drogon's event loops. `execSqlSync` there would block a loop thread for the whole query, so a handful of slow queries stalls every request the server is handling. `co_await execSqlCoro` suspends instead. Tests drive coroutines with `drogon::sync_wait`. | Sync repositories (blocks event loops); dispatching to a worker pool (reintroduces the thread-per-request cost Drogon exists to avoid) |
| D13 | **Repository/service coroutine parameters are taken by value** | A reference parameter to a coroutine dangles as soon as the coroutine first suspends, because the caller's frame may be gone. Passing by value moves the argument into the coroutine frame. This is a correctness rule, not a style preference. | `const&` parameters (use-after-free that only shows under load) |
| D14 | **Argon2id parameters default to libsodium's INTERACTIVE limits** | MODERATE costs 256 MiB *per concurrent hash*; a few simultaneous logins would OOM the cheap VPS this is designed for. INTERACTIVE (64 MiB) is the documented interactive-login profile and the limits are configurable for bigger hosts. | MODERATE/SENSITIVE (memory exhaustion under concurrent login) |
| D15 | **The database, not the staging file, owns an upload's offset** | The offset is handed out by one conditional `UPDATE … WHERE received_bytes = $expected`, and the write happens after. Two chunks racing at the same offset cannot both match, so only one is ever told to write. Reading the file size instead would let a duplicated request overwrite a range that a concurrent one was already writing. | `stat()` on the `.part` file (racy); a per-session mutex (does not survive more than one process) |
| D16 | **`Upload-Offset` is mandatory on every chunk** | Defaulting it to the server's current offset is the one mistake a resumed upload cannot recover from: a client that lost track silently duplicates or skips a range, and the hash check only catches it after the whole file has been sent. A wrong offset is a 409 carrying the real one, so the client recovers from the error itself. | Implicit append (silent corruption); `Content-Range` (semantics designed for responses, not partial writes) |
| D17 | **Quota is charged when an upload completes, by one conditional statement** | `UPDATE users SET upload_used_bytes = … WHERE upload_used_bytes + $2 <= upload_quota_bytes` — an empty result *is* the refusal. A read-then-write lets two uploads finishing at once each see the same free space. Dedup, hash failure and a lost insert race all refund, so an account only pays for bytes that became new storage. Staging disk is bounded separately by `maxOpenSessionsPerUser × maxBlobBytes`. | Reserving quota at session start (release paths on every abort, expiry and crash, for a counter that would then include bytes that never arrived) |
| D18 | **The manifest is a byte-exact canonical document, and the endpoint serves those exact bytes** | `builds.manifest_sha256` covers the served response, so a client verifies a download by hashing what it received instead of reproducing a canonical form of its own. Sorted by path, fixed key order, no whitespace, hand-written serialiser — jsoncpp changing how it escapes would silently break every stored hash. The build id is excluded so identical content yields identical hashes. | Re-serialising through jsoncpp on read (hash drifts with the library); hashing the database rows (no stable byte order) |
| D19 | **Lists of values reach SQL as one `jsonb` parameter, expanded with `jsonb_array_elements`** | A PostgreSQL array literal would mean hand-rolling the array-literal escaping rules for paths and hashes; jsoncpp already escapes correctly, and `jsonb_array_elements_text(… ) WITH ORDINALITY` even preserves the caller's order. | Array literals (custom escaping); one statement per element (N round trips) |
| D20 | **Numeric bind parameters are sent as text, not as C++ integers** | Drogon sends an integral parameter in PostgreSQL's *binary* format sized by the C++ type, so an `int` reaching a `bigint` column is rejected as malformed binary input. A text parameter is parsed by the server into whatever type it inferred for that position, which is correct whatever the column happens to be. | Matching each C++ width to its column by hand (one wrong pairing is a runtime error nothing catches at compile time) |
| D21 | **The download plan is a POST, not a GET** | It mints signed URLs and records that a download was handed out. Neither is cacheable and neither is free of consequence, which is exactly what GET promises. The body also leaves room for a client to describe its install more richly later without inventing a query-string encoding. | `GET …/download?from=…` (a cacheable, "safe" method with credentials in the response and a row written per call) |
| D22 | **Only the URL *path* is signed, never the scheme or host** | `$uri` is all nginx sees, so it is all the signature can cover. The consequence is the useful part: the same deployment keeps working when it is fronted by another hostname or moved behind TLS, and `storage.publicBaseUrl` can change without invalidating anything already minted. | Signing the absolute URL (any hostname change breaks every live link, and nginx cannot verify it anyway) |
| D23 | **A local copy is only ever offered from a path the update keeps unchanged** | A file that merely moved does not have to travel, but the source of the copy must survive the update: otherwise whether the copy works depends on the order the client applied the plan in, which is a bug that appears on some machines some of the time. Restricting `copyFrom` to survivors makes the plan order-independent by construction. | Offering any local path holding the content (ordering hazard); no copy hint at all (a rename re-downloads the whole file) |
| D24 | **Planned bytes count distinct blobs, not manifest entries** | Paths are the unit of the plan, blobs are the unit of the transfer. Two changed files with identical content are two entries and one download, so summing entries would overstate both what the client is told to expect and what the analytics record. | Summing entry sizes (double counts shared content) |
| D25 | **Files the manifest never mentioned do not make an install broken** | An install directory legitimately accumulates saves, configuration and logs. `verify` reports them under `unexpected` so the client can decide, but `intact` ignores them — a server that called an install corrupt because of a save file would train users to ignore the check. | Treating unexpected files as corruption (false positives); not reporting them at all (no way to clean up a botched update) |
| D26 | **Build authorization rules live in `domain/`, not in a service** | Both halves of a build's life ask the same two questions — who may publish to it, who may see it exists. `domain::mayPublishBuild` / `mayReadBuild` are pure functions over `BuildOwnership` and `Actor`, so `UploadService` and `DownloadService` cannot drift apart on the 404-not-403 rule. | A copy in each service (two places for one security rule to be wrong in) |

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
  domain/               entities, value objects, Actor (who is asking)
  storage/              BlobStore — the content-addressed filesystem layout
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

Fast edit/build/test loop. The image carries the toolchain and `vcpkg_installed`; bind-mounting
the working tree gives an incremental `build/` on the host instead of recompiling everything
inside a new image layer on every change. Roughly ten seconds per iteration against several
minutes for `docker compose build`.

```bash
# Configure once (from the repository root)
docker run --rm -v "${PWD}:/work" -w /work custom-game-launcher-api-build \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
        -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_MANIFEST_FEATURES=tests -DLAUNCHER_BUILD_TESTS=ON

# Then, per change — integration tests need the compose database on the same network, and
# LAUNCHER_TEST_DB_PASSWORD must match DB_PASSWORD in .env or every test silently skips.
docker compose up -d db
docker run --rm --network custom-game-launcher_default \
    -e LAUNCHER_TEST_DB_HOST=db -e LAUNCHER_TEST_DB_USER=launcher \
    -e LAUNCHER_TEST_DB_PASSWORD=change-me -e LAUNCHER_TEST_DB_ADMIN=launcher \
    -v "${PWD}:/work" -w /work custom-game-launcher-api-build \
    sh -c "cmake --build build --parallel && ctest --test-dir build --output-on-failure"
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
| `psql` is **not installed** | Use `docker compose exec db psql` |
| `gh` 2.97 is installed at `C:\Program Files\GitHub CLI` and authenticated as `Ruy41321` | Read CI failures with `gh run view <id> --log-failed` instead of guessing. The installer does not add it to an already-open shell's `PATH`; prepend the directory if `gh` is not found |
| **CI runs on hardware roughly 5x slower than the maintainer's** | ~49ms per integration HTTP request against ~10ms locally. Any assertion whose outcome depends on how many requests fit in a time window will pass locally and fail there — see the rate-limit row below |
| **A token-bucket assertion must shrink the bucket, not out-run its refill** | The throttle test sent a fixed number of requests against the default 500/60s limit. The bucket refills at 8.3 tokens/s, so the number of requests needed to empty it is a function of request latency: ~546 locally, ~845 in CI. Tests now narrow the limit with `ScopedAuthRateLimit` so the assertion is exact on any machine |
| MSVC 14.44 / VS 2022 Community is available | A local C++ build is possible but slow on first configure |
| **Never set `VCPKG_FORCE_SYSTEM_BINARIES=1`** in the build image | It makes vcpkg use the distro's CMake, which is older than the port scripts need; zlib fails to configure with a `string(JSON …)` error |
| **`vcpkg install` from the CLI writes to the *manifest* directory**, while the CMake toolchain looks in `${CMAKE_BINARY_DIR}/vcpkg_installed` | The Dockerfile must pass `-DVCPKG_INSTALLED_DIR=/src/vcpkg_installed`, or every `find_package` fails despite the dependencies being present |
| Some vcpkg ports need host tools that appear nowhere in `vcpkg.json` | `bison`/`flex` for libpq, and `autoconf`/`autoconf-archive`/`automake`/`libtool`/`gettext` for libsodium. Keep `docker/api/Dockerfile` and `.github/workflows/ci.yml` in sync |
| Drogon's batched PG backend rejects multi-statement SQL | See decision D4a; do not "simplify" the migration runner back onto `DbClient` |
| **A Drogon transaction commits asynchronously when its object is destroyed** | There is no `commitCoro()`. A coroutine that inserts inside a transaction and returns the new row's key can hand that key to the client *before* the commit lands, and the next request then cannot find it. This is exactly how refresh-token rotation broke. Prefer a single statement — data-modifying CTEs (`WITH inserted AS (INSERT … RETURNING …)`) give the same atomicity and are already durable when the query returns |
| **Do not detect unique violations by exception type** | `dynamic_cast` to `drogon::orm::SqlError` on what the batched backend throws did not match, so a duplicate registration surfaced as a 500. Use `ON CONFLICT … DO NOTHING RETURNING` and treat an empty result as the conflict; it is race-free and driver-independent |
| **Destroying a Drogon `DbClient` can abort with "Resource deadlock avoided"** | Its destructor joins the connection loop thread, and the last `shared_ptr` reference can end up owned *by* that thread. This showed up as intermittent `Subprocess aborted` failures in integration tests. Test setup/teardown therefore uses libpq directly (`tests/integration/TestDatabase`), and a `DbClient` is only created when a test genuinely exercises one |
| **Drogon's default request body limit is 1 MB** | Far below one upload chunk, and it rejects the request before the controller ever runs, so the failure looks like a routing problem rather than a size one. `app::configureUploadLimits` raises `setClientMaxBodySize` *and* `setClientMaxMemoryBodySize`; the integration harness calls the same function, so tests never run under limits nobody deploys |
| **A range-for over `bodyOf(response)["items"]` walks freed memory** | The helper returns a `Json::Value` by value; `["items"]` is a reference into that temporary, and C++20 does not extend its lifetime for the loop (P2718 fixes this in C++23). It cost a debugging cycle presenting as "the endpoint returns nothing" when the endpoint was correct. Bind the body to a named local first |
| **`.env` sets `DB_PASSWORD=change-me`, not the compose default** | `LAUNCHER_TEST_DB_PASSWORD` must match it, or every integration test *skips itself* with "LAUNCHER_TEST_DB_HOST is not set" — the connection error is printed once, before gtest's output, and is easy to scroll past. `docker compose --profile tools run` reads `.env` for you; a bare `docker run` does not |
| **Git Bash rewrites container paths in `docker run`** | `-v "$(pwd):/work" -w /work` becomes `C:/Program Files/Git/work` and the run fails. Use `MSYS_NO_PATHCONV=1`, `$(pwd -W)` for the source side and `//work` for `-w`. `docker compose` is unaffected, which is why this only bites the fast build loop |
| **Tampering with `expires` in a signed URL gives 403, not 410** | The expiry is part of what the signature covers, so changing it invalidates the signature and nginx reports "bad signature" before it ever looks at the clock. To see the 410 path, sign a URL whose expiry is already in the past |
| **MD5 is in OpenSSL 3's *default* provider** | `EVP_md5()` works with no legacy-provider setup, unlike MD4/MD2. `secure_link` needs MD5 and there is nothing to configure |
| **A fake hash in a test must still be valid hex** | Verification checks the shape of what a client reports before comparing anything, so a filler like `std::string(64, 'x')` is rejected as malformed and the assertion under test never runs. Build test hashes out of `0-9a-f` |
| **A `CHECK (expires_at > created_at)` on upload sessions blocks force-expiry** | Moving an expiry into the past is a legitimate administrative action, and it is how the expiry path is tested. The constraint was removed from migration 0002 before it was merged |

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

### Finishing a milestone

Pushing `dev` at the end of a milestone is **not** something to ask permission for — do it,
then watch the run it triggers. A milestone is not finished until CI is green:

```bash
git push origin dev
# gh lives in "C:\Program Files\GitHub CLI" and is not on an already-open shell's PATH
gh run list --branch dev --limit 3          # the new run appears a few seconds after the push
gh run watch <id>                           # or poll `gh run list` until it completes
gh run view <id> --log-failed               # only what failed, not the whole log
```

A red run is part of the same milestone, not the next session's problem: fix it, push the fix,
and check again. `Docker image builds` takes around twelve minutes, so the whole run is worth
waiting for rather than guessing at.

This applies to finishing a milestone. Mid-milestone pushes are still the maintainer's call.

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

### Milestone 3 — Authentication ✅
- ✅ Argon2id password hashing with transparent rehash on parameter upgrade
- ✅ JWT access tokens (HS256, jsoncpp traits), permissions embedded in the claims
- ✅ Refresh tokens: hashed at rest, rotated on every use, family revoked on reuse
- ✅ Register, email verification, login, logout, password reset
- ✅ Repository layer over Drogon coroutines; `JwtAuthFilter` + `requirePermission`
- ✅ Per-address token-bucket rate limiting on the unauthenticated endpoints
- ✅ [Documentation/authentication.md](Documentation/authentication.md)
- ⚠️ No mail transport yet: verification and reset tokens are returned in the response in
  **development only**. Remove those fields when delivery lands.

### GitHub Actions, first real runs (2026-08-03)
The workflow finally ran. Three runs, and what each taught:

- Run 1 was cancelled by the concurrency group.
- Run 2 (`d100e28`, pre-auth): `clang-format` failed, everything else green.
- Run 3 (`65bcd98`, auth): formatting green, `Build and test` failed on exactly one test,
  `AuthEndpointTest.ThrottlesRepeatedLoginAttempts`. Not flaky — deterministically broken on
  any machine slower than the maintainer's; see the rate-limit row in §8. Fixed by narrowing
  the bucket instead of raising the attempt cap.

- Run 4 (`3e5cbb0`) is green end to end: 131 unit + 31 integration, and the throttle test now
  takes 0.53s there instead of failing after 30.

The `docker` job has been green from the start. The vcpkg host packages the workflow installs
turned out to be correct, so that standing suspicion is closed.

Run 5 (`6853d7f`, milestone 4) is green on all three jobs: `clang-format` 15s, `Build and test`
2m57s for 277 tests, `Docker image builds` 12m6s. Every run since has warned that
`actions/checkout@v4`, `actions/cache@v4` and the docker actions target Node.js 20, which the
runners now force onto Node 24 — harmless today, a hard failure whenever GitHub drops the
shim. Worth a `ci:` bump before it becomes urgent.

Milestone 5 pushed two commits close together, and the concurrency group cancelled the first
run two minutes in — the same behaviour as run 1, and not a failure: the surviving run covers
the same tree plus the later commit. Run 7 (`4d42e12`) is green on all three jobs, 322 tests.
When two pushes land back to back, watch the *newest* run and ignore the cancelled one.

### Milestone 4 — Catalog, Explore and build upload ✅
- ✅ Catalog API: create and patch games, versions, builds; slug derivation and validation
- ✅ Explore with title search, three sort orders and paging; drafts never listed
- ✅ Visibility rules (`draft` / `unlisted` / `public`), enforced in `CatalogService`; a game
  the caller may not see is a 404, never a 403
- ✅ Server-side library: idempotent add, remove, list
- ✅ Content-addressed `BlobStore` with staging, hash verification and atomic publish
- ✅ Blob negotiation, resumable tus-style upload sessions, offset reservation in the database
- ✅ Manifest ingestion in one statement; byte-exact canonical document served by `GET .../manifest`
- ✅ Cumulative upload quotas charged race-free at completion, refunded on dedup and failure
- ✅ Abandoned-session sweeper on a timer
- ✅ `0002_upload_sessions.sql`: `upload_sessions`, `blobs.uploaded_by_user_id`
- ✅ [Documentation/catalog.md](Documentation/catalog.md),
  [Documentation/builds-and-uploads.md](Documentation/builds-and-uploads.md)

### Verified on 2026-08-03
- 277/277 tests green (209 unit, 68 integration against a real PostgreSQL)
- `docker compose up -d --build` brings api, db and fileserver to healthy; `0001` and `0002`
  both applied at boot
- End-to-end against the running stack: register → devlist grant → game → version → build →
  blob negotiation → two-chunk resumable upload → manifest. The blob landed at
  `/data/blobs/66/91/6691…` owned by `launcher`, staging was empty afterwards, and the served
  manifest hashed to the recorded `manifestSha256`
- `clang-format` clean across `src/` and `tests/`

### Milestone 5 — Delta updates, signed downloads, integrity ✅
- ✅ `POST /api/v1/builds/{id}/download`: the plan to reach a build, from any older one or from
  nothing, with the delta computed over the two manifests on demand
- ✅ Signed download URLs (`secure_link`: `base64url(md5("<expires><uri> <secret>"))`), signing
  the path only so a hostname change invalidates nothing
- ✅ Full-download fallback past `updates.fullDownloadThresholdRatio`
- ✅ `copyFrom` hints for content that only moved, restricted to paths the update keeps, so a
  plan is order-independent
- ✅ `POST /api/v1/builds/{id}/verify`: missing / corrupt / unexpected, plus signed URLs that
  repair the difference
- ✅ `download_events` recorded per plan, with the version the client came from
- ✅ Build authorization rules moved into `domain/` and shared with the upload side
- ✅ [Documentation/downloads-and-deltas.md](Documentation/downloads-and-deltas.md)

### Verified on 2026-08-03
- 322/322 tests green (243 unit, 79 integration against a real PostgreSQL)
- End-to-end against `docker compose up -d --build`: two builds of one game published, the
  delta plan carried only the changed executable (21 of 77 bytes) and listed the shared asset
  as unchanged
- **The signed URL was fetched through nginx**: 200 with the right bytes, 206 for a `Range`
  request, 403 without a token and with a wrong one, 410 for a correctly signed but expired
  URL. This is the part no automated test can reach, since the suite has no file server
- `verify` confirmed an intact install and, for a tampered one, named the corrupt and missing
  files and handed back repair URLs
- `download_events` held one `full` (77 bytes, no source version) and one `delta` (21 bytes,
  from the previous version)
- `clang-format` clean across `src/` and `tests/`

### Next up
- ⬜ **M6** Client API layer: the launcher finally talks to the server
- ⬜ **M9** Localhost admin web GUI
- ⬜ **M10** `Documentation/` per module, security hardening, GDPR erasure

Deliberately **not** in M4, and worth stating so a later session does not assume they exist:
game media (cover art, screenshots) and patch notes have tables but no endpoints; blob garbage
collection is unwritten — `build_files` is `ON DELETE RESTRICT`, so nothing can remove a
*referenced* blob, but nothing sweeps unreferenced ones either.

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

### At the end of a milestone, additionally

5. **Push `dev` and see CI through to green** — see §10. Not something to ask about.
6. **Update `HANDOFF.md`**, which lives one directory above both repositories
   (`C:\Users\Luigi\Developing\Personal\GameLauncher\HANDOFF.md`) and is deliberately outside
   version control, so it never lands in a commit. It is the first thing the next session
   reads, before either `CLAUDE.md`. Bring these up to date:
   - **Stato** — which milestones are done, the current test count, what is pushed;
   - **Prossimo** — the next milestone, in enough detail to start without re-deriving it;
   - **Cosa esiste già lato server** — the endpoint sketch and the invariants a new surface
     must not break, so the next session inherits the contract rather than rediscovering it;
   - **Debiti aperti** — anything deferred, added or paid off.

   `HANDOFF.md` is a briefing, not a changelog: it says what is true now and what to do next,
   and everything already captured by `CLAUDE.md` or the `Documentation/` files belongs there
   instead, referenced by name.
