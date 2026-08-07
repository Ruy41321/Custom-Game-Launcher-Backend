# Contributing

The server half. If you have not read the client repository's
[CONTRIBUTING.md](https://github.com/Ruy41321/Custom-Game-Launcher-Frontend/blob/main/CONTRIBUTING.md)
yet, start there: it explains what the whole system is and the three mechanisms — content
addressing, refusing anything that arrived over the network on its own word, and signed launcher
releases — that both halves are built on. This page assumes you know that much and covers what is
particular to the C++ side.

If you want to **run** this rather than change it, you want
[DISTRIBUTING.md](https://github.com/Ruy41321/Custom-Game-Launcher-Frontend/blob/main/DISTRIBUTING.md)
in the client repository, which walks the whole deployment.

---

## 1. Getting it running

Docker with Compose v2 is the only requirement. The toolchain — compiler, CMake, Ninja, vcpkg
and every dependency — lives inside the image, so nothing is installed on your machine.

```bash
cp .env.example .env
```

```bash
./scripts/dev.ps1
```

Or, if PowerShell is not what you have:

```bash
docker compose up --build -d
```

**The first build compiles Drogon from source and takes tens of minutes.** Later ones are
incremental. When it is up:

```bash
curl -s http://localhost:8080/api/v1/health
```

The development stack also runs a mail catcher at <http://localhost:8025>, so registration and
password recovery work end to end with no relay of your own. Migrations run at container start.

### On Linux, and on a machine that has never seen this

`.env` is **not** in version control, which is the only thing a clone is missing here — copy
`.env.example` over it and development runs on the placeholders. The database, the blobs and the
artwork live in Docker volumes that are created empty on the first `up`, and migrations run at
container start, so there is nothing else to restore. Accounts and games are seeded by hand and
have to be seeded again.

The three `scripts/*.ps1` in the two repositories carry `#!/usr/bin/env pwsh` and run on Linux
once PowerShell 7 is installed. Without it, every command they wrap is in `CLAUDE.md` §7.

Docker here is Engine plus the compose plugin rather than Desktop, and your user has to be in the
`docker` group. Everything in `CLAUDE.md` §8 about `MSYS_NO_PATHCONV`, PowerShell quoting and BOMs
is about running the toolchain from Windows and does not apply.

### The fast loop

`docker compose build` on every change would be unbearable. The toolchain image carries
`vcpkg_installed`, so bind-mounting the working tree gives an incremental `build/` on the host —
roughly ten seconds per iteration instead of several minutes. `CLAUDE.md` §7 has the two
commands; `./scripts/test.ps1` wraps them.

```bash
./scripts/test.ps1
```

```bash
./scripts/test.ps1 -Unit -Filter Media
```

**Do not pipe the dev scripts through `2>&1` in Windows PowerShell 5.1.** `docker compose` writes
progress to stderr, the shell wraps each line in an ErrorRecord, and a run that succeeded reports
failure.

---

## 2. The layers, and the one direction

```
controllers/   HTTP surface. Parse and validate input, call a service, serialize the result.
     │         No business logic, no SQL, no error formatting.
     ▼
services/      Business logic and authorization rules. Depends only on repository
     │         *interfaces*, never on Drogon or on SQL. This is what unit tests target.
     ▼
repositories/  Data access. An interface per aggregate plus a PostgreSQL implementation.
     │         The only place SQL is allowed to appear.
     ▼
domain/        Entities and value objects. Pure C++, no dependencies at all.
```

Drogon instantiates controllers itself and offers no DI container, so `app/AppContext` is a
**composition root**: built once at start-up, it constructs the concrete PostgreSQL repositories,
injects them into services behind their interfaces, and exposes the services. Controllers pull
what they need from `AppContext::instance()`.

The point of that is a single sentence: **services never name a concrete repository**, so a unit
test constructs one with gmock fakes and touches no database.

### Where an authorization rule goes

**In the service, never in the controller** — so no route can be added that forgets it. And when
two services ask the same question about the same thing, the answer moves into `domain/` as a
pure function: `mayViewGame`, `mayEditGame`, `mayPublishBuild`, `mayReadBuild`. A security rule
with two copies is a security rule with two places to be wrong in, and it is how the 404-not-403
convention would eventually drift.

**A resource the caller may not see is 404, never 403.** Drafts, other publishers' builds, other
people's upload sessions. A 403 confirms the thing exists.

---

## 3. Adding an endpoint

1. **Migration**, if the schema changes. Numbered `NNNN_name.sql`, applied in order, **immutable
   once merged** — the runner verifies checksums and aborts on a mismatch. A change to a merged
   migration is a new migration.
2. **Domain type** in `domain/`, if there is a new concept. Pure C++.
3. **Repository interface** in `repositories/`, PostgreSQL implementation beside it. SQL lives
   here and nowhere else, always parameterized.
4. **Service** holding the rules, taking the interface.
5. **Controller** under `/api/v1/`, doing nothing but parse, call and serialize.
6. **Tests in the same commit**: unit against gmock repositories, integration against the real
   app and a throwaway database. **Security-relevant behaviour is tested for the denial path**,
   not only the happy one.
7. **The `Documentation/` page** for that module, in the same commit.

### Coroutines, and two rules that are correctness rather than style

Controllers run *on* Drogon's event loops, so `execSqlSync` there blocks a loop thread for the
whole query and a handful of slow ones stall every request the server is handling. Everything is
`co_await execSqlCoro` instead; tests drive coroutines with `drogon::sync_wait`.

**Coroutine parameters are taken by value.** A reference parameter dangles as soon as the
coroutine first suspends, because the caller's frame may be gone. This is a use-after-free that
only appears under load.

**A Drogon transaction commits asynchronously when its object is destroyed.** There is no
`commitCoro()`, so a coroutine that inserts inside a transaction and returns the new key can hand
that key to the client *before* the commit lands — which is exactly how refresh-token rotation
broke once. Prefer a single statement: a data-modifying CTE (`WITH inserted AS (INSERT …
RETURNING …)`) gives the same atomicity and is already durable when the query returns.

That last one is also why an **audit entry is written by the same statement as the change it
describes**. An entry appended afterwards can fail on its own, and what it leaves behind is a
change nobody can attribute.

---

## 4. Tests

Split by CTest label. `unit` needs nothing; `integration` boots the real Drogon app on an
ephemeral port against a throwaway database, and **skips itself** when one is not configured —
which is worth knowing, because a whole suite quietly skipping looks a lot like a pass.

```bash
docker compose --profile tools run --rm api-build ctest --test-dir build --output-on-failure
```

`LAUNCHER_TEST_DB_PASSWORD` must match `DB_PASSWORD` in `.env`, or every integration test skips
with a connection error printed once, before gtest's output, easy to scroll past.

Two more things the suite genuinely cannot do, so that you do not assume otherwise:

- **No test touches nginx.** The file server is verified by hand against the running stack. When
  you change a surface that writes or serves files, spend ten minutes driving it for real — this
  is not advice, it is the lesson from a bug that made every fresh deployment refuse artwork
  uploads for a day.
- **No test speaks SMTP**, and none can hold a private signing key the way a person does. Mail
  is exercised against the catcher; launcher releases are exercised by hand.

---

## 5. The rules that will bite you

`CLAUDE.md` §8 is the full table, and every row cost a debugging cycle. The ones you will meet
first:

- **Adding a line to `vcpkg.json` means rebuilding the toolchain image**, not just
  reconfiguring — the fast loop builds against the `vcpkg_installed` inside it, and the failure
  is `find_package` not finding something that is sitting in the manifest.
- **Do not detect unique violations by exception type.** Use `ON CONFLICT … DO NOTHING
  RETURNING` and treat an empty result as the conflict.
- **A data-modifying CTE's effects are invisible to the rest of its own statement.** Select from
  the CTE, not from the table, or you silently get the row as it was before.
- **Drogon routes belong to the application, not to a listener.** A controller registered
  anywhere answers on *every* port the server binds; `AdminSurfaceFilter` is what makes the admin
  listener mean anything. An admin route added without it is a public route.
- **A range-for over `bodyOf(response)["items"]` walks freed memory.** Bind the body to a named
  local first.
- **From Git Bash, `docker compose exec` and `cp` need `MSYS_NO_PATHCONV=1`** wherever an
  argument looks like an absolute path, or `/app/launcher-api` becomes
  `C:/Program Files/Git/app/launcher-api` and it reads like a broken image.
- **Everything is in English**, and `clang-format` is enforced.

---

## 6. Documentation

| | For | When |
|---|---|---|
| This file | A new contributor | Now |
| `Documentation/*.md` | One module each — eleven of them | Before touching that module |
| [CLAUDE.md](CLAUDE.md) | The full decision log, §4 | When you want to know *why* |

Each `Documentation/` page states what is deliberately **not** implemented, which is the part the
code cannot tell you. `CLAUDE.md` §4 is every technical decision with its rationale and the
alternatives rejected; **never delete a row** — if a decision is reversed, add one that supersedes
it and say why.

---

## 7. Workflow

Work happens on **`dev`**; `main` is merged by the maintainer. **CI runs on `main`**, so nothing
on GitHub catches a red suite on `dev` — the gate is local and not optional:

```bash
./scripts/test.ps1
```

```bash
./scripts/test.ps1 -Format
```

Check `git diff` is empty after the second. What they cannot cover is the `docker` job — a clean
image build and the ownership guard on the data volumes — so a change touching `docker/`,
`vcpkg.json` or the compose files is worth calling out as unverified.

**Write the commit message to a file and use `git commit -F`** from Windows PowerShell 5.1: a
message containing double quotes gets split into arguments git reports as missing pathspecs. And
`Out-File -Encoding utf8` writes a BOM, so the subject silently begins with three invisible
bytes — use `[System.IO.File]::WriteAllText(..., (New-Object System.Text.UTF8Encoding $false))`.
