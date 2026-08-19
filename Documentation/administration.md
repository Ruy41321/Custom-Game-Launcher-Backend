# Administration

The operator surface: a second HTTP listener bound to the loopback interface, carrying a web
console and the endpoints behind it. Users, roles, upload quotas, download statistics and the
audit trail.

Companion documents: [architecture.md](architecture.md) for the layering,
[authentication.md](authentication.md) for the token scheme this surface reuses.

---

## 1. Why it is a second listener

The server operator needs to change quotas and hand out roles on a headless VPS. A desktop
application on the server would need X11 or VNC; a route on the public API would be one
permission check away from being world-reachable. So the API binds a second listener and the
console lives there:

```
ssh -L 9090:127.0.0.1:9090 user@your-vps
# then open http://localhost:9090/admin in a local browser
```

It is off by default. `ADMIN_ENABLED=true` turns it on.

### What actually restricts it

Three things, and it is worth being precise about which does what, because two of them are
easy to mistake for each other.

| Layer | What it stops | Where it lives |
|---|---|---|
| The published port | Anybody who is not on the host | `docker-compose.yml`: `127.0.0.1:9090:9090` |
| `AdminSurfaceFilter` | A request that arrived on the *public* listener | `src/filters/AdminSurfaceFilter.cpp` |
| `JwtAuthFilter` + `AdminOperatorFilter` | A caller who is not an operator | same directory |

**Drogon registers routes on the application, not on a listener.** Without the filter, every
administrative controller would answer on `:8080` exactly as it does on `:9090`, and the second
listener would be decoration. The filter compares `request->localAddr().toPort()` against
`server.adminPort` and answers anything else with **404**, word for word the framework's own
not-found page — a 403 would confirm the route exists to anyone who guessed the path.

**The peer address is deliberately not checked.** Inside the container the listener binds
`0.0.0.0`, and the restriction to the host's loopback is the Docker port publication. Every
legitimate request therefore arrives from the bridge gateway, never from `127.0.0.1`; a peer
check would reject the deployment the compose file documents. The network restriction stays on
the network side.

`AppConfig::validate()` refuses a configuration where `adminPort` equals `port`. The whole
separation rests on which listener a request arrived on, and collapsing the two would silently
make user management a public endpoint.

---

## 2. Signing in

The console mints its own tokens:

```
POST /admin/api/auth/login      {email, password}   -> session
POST /admin/api/auth/refresh    {refreshToken}      -> rotated session
POST /admin/api/auth/logout     {refreshToken}      -> 204
GET  /admin/api/session                             -> who is signed in
```

Not because tokens from `/api/v1/auth/login` are second class — they are the same tokens, and
one works here — but because an operator on the far end of `ssh -L 9090:...` has forwarded one
port and has no route to `:8080` at all.

It is `AuthService` underneath, plus one extra condition: the account must hold some `admin.*`
permission (`domain::mayUseAdminSurface`). A correct password from an ordinary player yields
**403**, and the session that correct password just opened is retired rather than left alive.

The check runs on **rotation** as well as at sign-in. Permissions live in the access token, so
revoking a role cannot reach one already issued; the refresh is where a demoted operator's
session ends instead of renewing indefinitely.

`AdminOperatorFilter` is a filter rather than a helper each controller calls, so that no route
added later can forget it. It answers the coarse question only — may this account address the
surface at all — and every individual action still checks the permission it needs.

---

## 3. Users, roles and quotas

```
GET    /admin/api/users?search=&inactive=&page=&pageSize=
GET    /admin/api/users/{userId}
PATCH  /admin/api/users/{userId}          {uploadQuotaBytes?, active?}
PUT    /admin/api/users/{userId}/roles/{roleKey}
DELETE /admin/api/users/{userId}/roles/{roleKey}
POST   /admin/api/users/{userId}/temporary-password
GET    /admin/api/roles
```

| Action | Permission |
|---|---|
| List, read, quota, activate/deactivate | `admin.users.manage` |
| Grant and revoke roles, list roles | `admin.roles.manage` |
| Audit trail, download analytics | `admin.settings.manage` |

The seeded `admin` role holds every permission, so in practice an operator has all three. They
are separate anyway, so that a deployment can cut a narrower role without a code change — which
is the point of the role graph being table driven.

A few behaviours that are decisions rather than accidents:

- **Lowering a quota below what is already stored is allowed.** It stops further uploads without
  deleting anything, which is the tool an operator reaches for when an account is filling the
  disk. The charge is a conditional statement, so an account over its quota simply cannot upload
  again.
- **Deactivating an account revokes its live refresh tokens**, in the same statement as the
  flag. Otherwise a disabled account keeps renewing access for as long as somebody holds a
  token, which is not what disabling an account means to anybody.
- **Granting a role twice succeeds** and records nothing the second time. The end state is what
  was asked for either way, and failing would make retrying a lost response dangerous for no
  reason.
- **A malformed user id is a 404**, not a 500. Without the guard the value reaches a `$n::uuid`
  comparison and PostgreSQL raises.
- **The password hash is never selected**, so no field added to the response later can leak it.

### Handing out a one-time password

```
POST /admin/api/users/{userId}/temporary-password    ->  200 {user, temporaryPassword}
```

For the deployment configured with `MAIL_TRANSPORT=none`, where there is no reset link because
there is nothing to deliver one with. [authentication.md](authentication.md) describes the whole
flow; what belongs here is what an operator sees and the two rules around it.

The response carries the password **once**. It is generated by the server — an operator choosing
one is an operator typing `hunter2` on a route whose entire purpose is a credential — and it is
never stored, logged or recoverable: only its hash reaches the database, and the audit entry
(`user.password.temporary_set`) records that it happened and to whom. An operator who loses it
issues another. The account list shows `passwordChangeRequired`, so it is visible who is still
sitting on one they never replaced.

The same statement revokes every session the account holds and burns any outstanding reset link.
It cannot revoke an **access** token already in somebody's hands: that stays valid for its
remaining minutes, exactly as it does after a deactivation, and for the same reason — the token
is what authorizes, and nothing reads the database to authorize.

**An operator cannot do this to their own account**, which is the third rule in the section
below and the sharpest of them: the flag refuses every route but the password change, and this
console has no such route.

### The way back in

Two rules exist so that the surface cannot lock everybody out of itself:

1. An operator cannot deactivate their **own** account.
2. While an account is the only **active** holder of `admin.users.manage`, neither its roles nor
   its active flag may be changed by anybody, including itself.
3. An operator cannot set a temporary password on their **own** account. The flag it raises is
   honoured by `JwtAuthFilter` on the *public* API, which refuses everything but the password
   change — and this console has no password-change route, so an operator who did it to
   themselves would be locked out of the surface they administer.

The second is deliberately blunt: it refuses to change *any* role on that account, not only the
one carrying the permission. A finer rule would have to reason about which permissions the
particular role being revoked grants, and being wrong about that once costs the operator every
route back — nothing but the command line can repair an empty administrator list.

---

## 4. Bootstrapping the first administrator

Granting a role through the surface requires `admin.roles.manage`, which on a fresh deployment
nobody holds. Something outside the permission system has to hand out the first one, and the
authority that makes sense is shell access to the machine — the same authority that reaches the
loopback listener at all.

```bash
docker compose exec api /app/launcher-api --grant-role you@example.com admin
```

This also retires the manual `INSERT INTO user_roles ...` that the setup notes carried since
the catalog work: the devlist is granted the same way.

```bash
docker compose exec api /app/launcher-api --grant-role friend@example.com dev
```

Running it twice is a success, not an error — it is how somebody checks. It writes an audit row
with a **null actor**, because a command line is not a user, and `metadata.via = "command-line"`
so a later reader is not left wondering who the missing actor was.

Like the migration runner, it talks to libpq directly: there is no event loop to run a coroutine
on, and destroying a Drogon `DbClient` can land on its own connection thread and abort the
process.

---

## 5. The audit trail

```
GET /admin/api/audit?actor=&action=&entityType=&entityId=&page=&pageSize=
```

`audit_log` has been in the schema since migration 0001 and nothing wrote to it until this
surface existed.

**Every entry is written by the same statement as the change it describes**, through a CTE whose
audit arm selects from the modifying arm:

```sql
WITH updated AS (
    UPDATE users SET upload_quota_bytes = $2 WHERE id = $1::uuid RETURNING *
),
logged AS (
    INSERT INTO audit_log (actor_user_id, action, entity_type, entity_id, metadata)
    SELECT NULLIF($3, '')::uuid, $4, $5, $6, $7::jsonb FROM updated
)
SELECT <columns> FROM updated u
```

This is the load-bearing part of the design, and it is worth saying why rather than treating it
as an implementation detail. An entry appended *after* the change can fail on its own, and what
it leaves behind is a change nobody can attribute — the one outcome an obligatory trail exists
to rule out. Writing both in one statement makes that impossible, and it gives three properties
for free:

- A change that matched no row records nothing.
- A change that was already in effect records nothing, because `ON CONFLICT DO NOTHING` returns
  no row for the audit arm to select.
- A refused change never reaches the statement at all.

Note the final `SELECT` reads the `UPDATE`'s own `RETURNING`, not the table. A data-modifying
CTE's effects are invisible to the rest of the statement, so selecting from `users` again would
hand back the row as it was **before** the change.

Actions recorded today are in `domain::auditActions`: `user.quota.changed`, `user.activated`,
`user.deactivated`, `user.role.granted`, `user.role.revoked`, and `user.erased`. The last is the
only one an ordinary account writes about itself — an erasure names the same id as actor and as
entity — and it is the clearest case for the rule above: an entry written after an irreversible
change, and failing, leaves a change nobody can attribute. See
[authentication.md](authentication.md).

`actor_user_id` is `ON DELETE SET NULL`, but an erasure never triggers it: the account is
anonymised rather than deleted, so the actor of an old entry resolves to `Deleted account`
instead of disappearing. Entries by *the same* erased operator therefore stay linked to each
other, which is what makes the trail still worth reading.

They are constants rather than
literals at the call sites because these strings are *queried*, and a typo in an audit trail is
invisible until the day it matters.

`metadata` is `jsonb` and travels as a real object rather than a string of JSON: it is the one
field whose shape varies by action.

---

## 6. Download analytics

```
GET /admin/api/analytics/downloads?days=30&topGames=10
```

`download_events` has been filled by the download planner since the delta work and read by
nothing. One report rather than three endpoints, so the three panels of the console cannot show
different windows — totals from the last month beside a chart of the last week is a bug that
looks like data.

```json
{ "days": 30,
  "totals": { "downloads": 0, "distinctUsers": 0, "bytesPlanned": 0,
              "fullDownloads": 0, "deltaDownloads": 0 },
  "daily":  [ { "day": "2026-08-04", "downloads": 0, "bytesPlanned": 0 } ],
  "topGames": [ { "gameId": "", "slug": "", "title": "",
                  "downloads": 0, "bytesPlanned": 0, "distinctUsers": 0 } ] }
```

Two things the numbers are honest about:

- **`bytesPlanned` is what the plans expected to transfer, not what clients pulled.** The file
  server answers the transfer itself and never reports back, so a client that cancelled halfway
  is counted in full. It is an upper bound.
- **`distinctUsers` is distinct accounts.** `download_events.user_id` goes null when an account
  is erased, so those rows stay in the count of downloads and leave the count of people.

The daily series comes from `generate_series` left-joined against the events, so a day with no
downloads is a zero rather than a gap: a chart that skipped empty days would draw a busier
picture than the truth. It runs from `today - days` through today inclusive, which is why a
seven-day window has eight entries.

`days` and `topGames` are clamped rather than refused (1–365 and 1–50). The window comes from a
control on a page, and a stale bookmark deserves a report; the bound is what stops one request
walking a decade of events.

This is server-wide and takes the operator-wide permission, because the question spans every
publisher on the deployment. A publisher's view of their own game would be a different surface,
on the public API, and does not exist yet.

---

## 7. The console

`GET /admin` (and `/admin/`) serves one self-contained HTML page: sign in, then users, downloads,
crashes and audit.

It is **embedded in the binary** at build time from `src/admin/ui/index.html`. The deployed
image then has no path to mount, the page cannot get out of step with the API it talks to, and
the integration tests exercise the same bytes a deployment serves. `CMakeLists.txt` reads the
file and configures `cmake/AdminUi.cpp.in` into a string literal;
`CMAKE_CONFIGURE_DEPENDS` on the HTML is what stops a stale page staying compiled in after an
edit.

Vanilla JavaScript, no build step. In a C++ repository that is not a compromise: adding npm to
get a console that shows a handful of tables would cost more than the console is worth.

The crash screen is the one with two lists rather than one, because the routes behind it answer
two different questions (see [crash-reports.md](crash-reports.md)). The upper list is one row per
*bug*; **Reports** narrows the lower list to that fingerprint and **Latest** opens the newest
report behind it, using the `latestReportId` the group already carries so opening one costs no
search. The two page independently: narrowing the reports and walking them leaves the list of
bugs where the operator left it. A page number is only remembered once its request came back, so
a refused one — an expired session, a revoked `admin.crashes.read` — leaves the pager where it
was instead of skipping a page nobody saw.

Only the listener gate applies to the route. The page must load before anybody can sign in, and
it carries no data — every number on it arrives from an endpoint that does check — so an
unauthenticated caller on the loopback listener gets a login form.

The access token lives in a JavaScript variable and nowhere else: no browser storage, no cookie.
This page is reached over a tunnel from somebody's desktop, and a token that survives the tab
outlives the reason it was issued. Two integration tests hold that line by grepping the served
bytes — one for the names of the storage APIs, one for any absolute URL — alongside a
`Content-Security-Policy` of `default-src 'none'` with `connect-src 'self'` and
`frame-ancestors 'none'`. The page needs none of it relaxed, so a future edit reaching for a CDN
fails loudly in the browser instead of quietly adding a third party to an operator console.

---

## 8. Configuration

| Key | Environment | Default | Notes |
|---|---|---|---|
| `server.adminEnabled` | `ADMIN_ENABLED` | `false` | Off unless asked for |
| `server.adminListenAddress` | `ADMIN_HOST` | `127.0.0.1` | **`0.0.0.0` in the container** — see below |
| `server.adminPort` | `ADMIN_PORT` | `9090` | Must differ from `server.port` |

The compose file sets `ADMIN_HOST=0.0.0.0` on purpose. Binding `127.0.0.1` *inside* the
container makes the listener unreachable even through the published port, because the request
arrives on the container's bridge address. The publication `127.0.0.1:9090:9090` is what keeps
it on the host's loopback.

---

## 9. What this surface deliberately does not do

- **No content moderation.** `admin.games.manage` exists as a permission and
  `domain::mayViewGame` / `mayEditGame` already honour it across the catalog, artwork and
  devlog services, so an operator can already edit any publisher's game through the public API.
  There is no console screen for it. Deleting a game is possible on the public API — an
  operator holding `admin.games.manage` can delete any publisher's — but there is no button for
  it here. See [storage-lifecycle.md](storage-lifecycle.md).
- **No server settings screen.** `admin.settings.manage` currently guards reading rather than
  writing: configuration arrives from a file and the environment, and a screen that edited it
  would need a story about what happens on restart.
- **No retention policy.** Builds and versions are deleted by hand and the collector reclaims
  what they leave. Nothing decides on its own that a game's fifth-oldest build can go.
