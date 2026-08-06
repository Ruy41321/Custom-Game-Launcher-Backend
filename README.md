# Custom Game Launcher — Backend

REST API, PostgreSQL schema and build file server for the
[Custom Game Launcher](https://github.com/Ruy41321/Custom-Game-Launcher-Frontend), an open-source,
self-hostable game launcher for indie and hobbyist developers who need to get demos and
in-development builds to a handful of testers without zip files on Discord.

Written in C++20 with [Drogon](https://github.com/drogonframework/drogon). The whole stack —
API, database and file server — comes up with one `docker compose` command and is meant to
run comfortably on a cheap VPS.

> **Status:** early development. Scaffolding, the database schema and the full authentication
> surface are in place; the catalog and build-upload APIs are next. See
> [CLAUDE.md](CLAUDE.md#11-progress) for the current state.

## Features

- JWT authentication with rotating refresh tokens and Argon2id password hashing
- Table-driven roles and permissions, extensible without destructive migrations
- Content-addressed build storage: files are stored once by SHA-256, so the delta between
  any two versions is a manifest diff and unchanged files are never re-uploaded
- Resumable downloads via signed URLs and HTTP `Range`
- Per-user cumulative upload quotas
- Download analytics per title
- Structured JSON logging and a single error envelope across every endpoint

## Requirements

- **Docker** with Compose v2 — the only requirement for running the stack
- For a local (non-container) build: a C++20 compiler, CMake ≥ 3.22, Ninja and a
  bootstrapped [vcpkg](https://github.com/microsoft/vcpkg)

## Quick start

```bash
cp .env.example .env
```

Generate the two secrets and put them in `.env` (they are optional in development, required
everywhere else):

```bash
openssl rand -hex 32
```

Bring the stack up. The first build compiles Drogon from source and takes a while; later
builds are incremental.

```bash
docker compose up --build -d
```

Check that it is alive:

```bash
curl -s http://localhost:8080/api/v1/health
```

Migrations run automatically at container start. To apply them by hand:

```bash
docker compose exec api /app/launcher-api --migrate
```

Open a psql shell:

```bash
docker compose exec db psql -U launcher -d launcher
```

Tear down (add `-v` to also delete the database and blob volumes):

```bash
docker compose down
```

## Tests

The suite is split by CTest label: `unit` needs nothing, `integration` needs a PostgreSQL
instance and skips itself when one is not configured.

Everything, inside the toolchain container:

```bash
docker compose --profile tools run --rm api-build ctest --test-dir build --output-on-failure
```

Unit tests only:

```bash
docker compose --profile tools run --rm api-build ctest --test-dir build -L unit --output-on-failure
```

Every feature must ship with its tests, and the **whole** suite must pass before a change is
considered done.

## Local build without Docker

```bash
cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug
```

`VCPKG_ROOT` must point at a bootstrapped vcpkg. On Windows, CMake and Ninja ship inside
Visual Studio 2022 but are not on `PATH`; use the `windows-msvc` preset.

## Configuration

Per-environment JSON lives in `config/`, is selected by `LAUNCHER_ENV`, and contains no
secrets — only defaults and `${VAR}` / `${VAR:-default}` placeholders resolved from the
environment at start-up. A `${VAR}` with no value and no default is a hard error, so a blank
JWT secret can never reach production silently.

| Variable | Purpose |
|---|---|
| `LAUNCHER_ENV` | `development`, `staging` or `production` |
| `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER`, `DB_PASSWORD` | Database connection |
| `JWT_SECRET` | Access-token signing key (≥ 32 chars outside development) |
| `FILE_SECURE_LINK_SECRET` | Shared with the file server to sign download URLs |
| `ADMIN_ENABLED` | Enables the loopback-only admin listener |
| `LOG_LEVEL`, `LOG_JSON`, `LOG_DIR` | Logging |

See [.env.example](.env.example) for the full list.

## Administration

The admin surface is a second listener bound to loopback and published only on the host's
`127.0.0.1`. It is never exposed publicly; reach it through an SSH tunnel:

```bash
ssh -L 9090:127.0.0.1:9090 user@your-vps
```

## Documentation

One document per module, in `Documentation/`. Together they describe every endpoint this
server has and, more usefully, why each one works the way it does.

| Document | What it covers |
|---|---|
| [architecture.md](Documentation/architecture.md) | Layers, the composition root, config and logging |
| [authentication.md](Documentation/authentication.md) | Argon2id, JWT, refresh rotation, rate limiting |
| [catalog.md](Documentation/catalog.md) | Games, versions, Explore, visibility and the library |
| [builds-and-uploads.md](Documentation/builds-and-uploads.md) | Content-addressed blobs, resumable uploads, manifests, quota |
| [downloads-and-deltas.md](Documentation/downloads-and-deltas.md) | Download plans, signed URLs, integrity verification |
| [artwork-and-devlog.md](Documentation/artwork-and-devlog.md) | Covers and screenshots, and a game's devlog |
| [storage-lifecycle.md](Documentation/storage-lifecycle.md) | Deleting builds, collecting unreferenced blobs, quota refunds |
| [administration.md](Documentation/administration.md) | The loopback operator console, roles, quotas, audit |
| [crash-reports.md](Documentation/crash-reports.md) | Receiving launcher crashes, fingerprinting and grouping them |
| [hardening-and-deployment.md](Documentation/hardening-and-deployment.md) | Headers, per-account limits, body caps — and what TLS leaves to a deployment |

## Project layout

| Path | Contents |
|---|---|
| `src/controllers/` | HTTP surface, versioned under `/api/v1` |
| `src/services/` | Business logic and authorization rules |
| `src/repositories/` | Data access — the only place SQL appears |
| `src/domain/` | Entities and value objects, dependency-free |
| `src/common/` | Errors, `Result<T>`, hashing, logging |
| `migrations/` | Numbered SQL migrations, immutable once merged |
| `tests/` | `unit/` and `integration/` |
| `filters/` | Authentication and rate-limiting middleware |
| `Documentation/` | One document per module — see the table above |

Architecture, conventions and the running list of technical decisions live in
[CLAUDE.md](CLAUDE.md).

## Contributing

Development happens on `dev`; `main` is merged by the maintainer once work is validated.
Commits are atomic and use conventional prefixes (`feat:`, `fix:`, `test:`, `docs:`, …).
Code, comments and commit messages are in English. CI runs formatting, the full test suite
and the Docker image builds on every push and pull request to `dev`.

## Licence

[MIT](LICENSE) © 2026 Luigi Pennisi
