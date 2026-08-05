# Authentication and authorization

Covers registration, email verification, login, session refresh, password reset and the
authorization model. Implemented in `src/services/AuthService.*`, `src/filters/` and
`src/controllers/v1/AuthController.*`.

## Endpoints

All under `/api/v1/auth`. Every response — success or failure — carries `X-Request-Id`.

| Method | Path | Auth | Throttled | Purpose |
|---|---|---|---|---|
| POST | `/register` | – | yes | Create an account and send a verification link |
| POST | `/login` | – | yes | Exchange credentials for a session |
| POST | `/refresh` | refresh token in body | no | Rotate the session |
| POST | `/logout` | refresh token in body | no | Revoke one session |
| POST | `/verify-email` | – | no | Redeem a verification link |
| POST | `/password-reset/request` | – | yes | Start a reset |
| POST | `/password-reset/confirm` | – | yes | Finish a reset |
| GET | `/me` | Bearer access token | no | Current identity and permissions |

Plus one route outside `/auth`, on the account itself:

| Method | Path | Auth | Purpose |
|---|---|---|---|
| POST | `/api/v1/me/deletion` | Bearer access token **and** the password | Erase this account |

A session response looks like:

```json
{
  "accessToken": "eyJ…",
  "refreshToken": "3Yk…",
  "tokenType": "Bearer",
  "expiresIn": 900,
  "user": { "id": "…", "email": "…", "displayName": "…", "emailVerified": true,
            "uploadQuotaBytes": 5368709120, "uploadUsedBytes": 0 },
  "permissions": ["library.read", "game.download"]
}
```

## Tokens

**Access token** — JWT, HS256, ~15 minutes. Carries the subject, email and the flattened
permission list, so authorizing a request costs no database round trip. The price is that a
permission change only takes effect on the next refresh, which is precisely why the lifetime
is short.

HS256 is correct here because the only issuer and the only verifier are the same process.
Asymmetric keys would add key distribution and buy nothing. `allow_algorithm` pins HS256,
which is what closes the classic `alg: none` bypass.

jwt-cpp ships no default JSON backend; the jsoncpp traits are used because Drogon already
depends on jsoncpp.

**Refresh token** — 256 bits from libsodium's CSPRNG, base64url. **Only its SHA-256 is
stored**, so a database leak hands out no live sessions.

### Rotation and reuse detection

Every refresh issues a new token and revokes the presented one, inside a single transaction.
Tokens descending from one login share a `family_id`.

Presenting an *already rotated* token means one of two things: the token leaked and someone
is replaying it, or the legitimate client replayed its own. Neither can be distinguished, and
in both cases the family can no longer be trusted — so **the entire family is revoked**. The
attacker loses access, and the real user is forced to log in again, which is the correct
outcome.

```
login ──▶ T1 ──refresh──▶ T2 ──refresh──▶ T3        (family F)
             │
             └── replayed later ──▶ 401 + every token in F revoked
```

## Passwords

Argon2id via libsodium's `crypto_pwhash_str_alg`, with `ALG_ARGON2ID13` pinned rather than
left to `ALG_DEFAULT` — the default could change between versions and stored hashes must stay
verifiable.

Cost defaults to the INTERACTIVE profile (~64 MiB). MODERATE would be stronger but costs
256 MiB *per concurrent hash*, and a handful of simultaneous logins would OOM the small VPS
this project targets. `auth.argon2OperationsLimit` and `auth.argon2MemoryLimitBytes` raise it
on a bigger host; raising them transparently upgrades each stored hash at its owner's next
successful login.

Policy is length-only: 12–256 characters. Mandatory character classes push users towards
predictable substitutions without buying entropy (NIST 800-63B). The upper bound exists so a
multi-megabyte "password" cannot be turned into an Argon2id denial of service.

## Not leaking who has an account

| Path | Behaviour |
|---|---|
| Login, unknown address | Same status and message as a wrong password, **and** a dummy hash is computed so the timing matches |
| Password reset request | Always reports success, with or without a matching account |
| Logout, unknown token | Reports success |
| Token rejection | One message for expired, forged, malformed and wrong-issuer alike |
| Registration, existing address | **409 Conflict — deliberately distinguishable** |

Registration is the conscious exception. Telling someone "that address is already
registered" is the only way they can act on it, and the same fact is obtainable from the
reset flow anyway. The trade is accepted rather than overlooked.

## Authorization

Roles and permissions are rows (see `migrations/0001_initial_schema.sql`), never an enum, so
adding a role is an `INSERT`. The operator-managed *devlist* is membership in the `dev` role.

- `JwtAuthFilter` authenticates and publishes the verified claims onto the request.
- `filters::requirePermission(request, permission)` authorizes, throwing `Forbidden`.

The client performs the same checks so the UI does not offer actions that will fail, but
**that check is advisory**. Every privileged path re-checks server-side, regardless of what
the interface already hid.

## Erasing an account

```
POST /api/v1/me/deletion    {"password": "...", "reason": "optional, ≤ 500 chars"}   -> 204
```

Implemented in `src/services/AccountService.*` over `IAccountRepository`, whose one method is
the erasing statement.

### Immediate, not deferred

There is no grace period and nothing to cancel. `account_deletion_requests` has carried a
`pending` status and a partial unique index enforcing one open request per account since the
first migration, and that design was deliberately **not** taken up: a window needs something to
close it, something to cancel it, and a decision about whether signing in during the window
counts as a change of mind — and, worse, for its whole length the account is *not yet erased*
while its owner has been told it will be. The row is still written, as `completed` with its
`processed_at`, so the compliance record exists and a later session that does want the deferred
form has the table and the status it needs.

### Anonymised, not deleted

The account row survives, holding:

| Column | After |
|---|---|
| `email` | `erased+<user id>@deleted.invalid` — unique by construction, and `.invalid` is reserved by RFC 2606 so nothing can be delivered to it or registered as it |
| `display_name` | `Deleted account` |
| `password_hash` | a value that is not a valid Argon2id encoding, so no password verifies against it |
| `email_verified_at`, `last_login_at` | null |
| `is_active` | false |

It has to survive: `games.publisher_user_id` is `ON DELETE RESTRICT`, so a `DELETE` on an account
that ever published would be refused by the database — and refusing an erasure because somebody
published a demo once is not an option either. What the rows pointing at it now point at is an
anonymous row, which is the whole point.

### What goes, what stays

| Goes | Stays |
|---|---|
| Every refresh token — a live session after an erasure is the one thing an erasure exists to rule out | Published games, their versions and builds, so other people's installs keep updating |
| Every pending verification and password-reset link | Patch notes, now written by "Deleted account" |
| The library (`user_games`) | Audit entries, whose actor is that anonymous row |
| The `user_id` on `download_events`, set to null | The `download_events` rows themselves, so the deployment's totals do not move |
| | `blobs.uploaded_by_user_id`, so the collector still knows whose quota to refund |
| | Open `upload_sessions`, which the sweeper expires on its timer — deleting the rows here would strand their staging files with nothing left to find them |

A publisher who wants their titles gone deletes them first; that is a separate, deliberate act,
and [storage-lifecycle.md](storage-lifecycle.md) describes it.

### The rules around it

- **The password is asked for again.** A valid access token says who is asking, not that the
  owner is the one at the keyboard, and this is the request with no undo.
- **All of it is one statement**, audit entry included, for the reason
  [administration.md](administration.md) gives at length: an entry written afterwards can fail on
  its own, and what it leaves is an irreversible change nobody can attribute.
- **A second attempt changes nothing.** The access token stays cryptographically valid for its
  remaining minutes, but the password behind it no longer exists, so the second call is a 401 —
  and the statement itself refuses an account that already holds its placeholder address, so no
  path writes a second audit row.
- **The last operator who can manage users cannot erase themselves**, for the reason D37 gives
  about deactivation, only more so: nothing but the command line repairs an empty administrator
  list, and unlike a revoked role this cannot be handed back. The answer is a 409 telling them to
  grant somebody else first.

### Why POST and not `DELETE /api/v1/me`

The request has to carry a password, so it needs a body, and a body on `DELETE` is the one place
HTTP declines to promise anything: no defined semantics, and intermediaries may drop it. The
path also leaves room for `GET` and `DELETE` on `/me/deletion` if the deferred flow is ever
built, without re-cutting the surface.

## Rate limiting

Login, registration and both reset endpoints carry `AuthRateLimitFilter`: a per-address token
bucket, 10 attempts per 60 seconds by default (`rateLimit.*`). Without it these endpoints are
both an online guessing oracle and — because each attempt costs an Argon2id hash — a cheap
way to burn the server's CPU. A throttled response is `429` with `Retry-After`.

The limiter is in-process. That is correct for the single-node deployment this project
targets and deliberately avoids adding Redis to the compose stack. If the API is ever scaled
out, the buckets become a shared-store problem.

## Development affordances

There is no mail transport yet. In `development` **only**, `/register` and
`/password-reset/request` return the raw token as `devEmailVerificationToken` /
`devPasswordResetToken`, so the flows are exercisable and the integration tests can drive
them. The fields are gated on the environment name, and `AppConfig::validate()` refuses to
start a deployed environment with development-grade secrets.

`auth.requireVerifiedEmail` can be turned off in development so logging in does not need the
verification round trip.

**Both must be revisited when mail delivery lands** — at that point the dev token fields
should go away entirely.

## Testing

| Area | Where |
|---|---|
| Validation, hashing, JWT, rate limiting | `tests/unit/` — pure, no I/O |
| AuthService rules | `tests/unit/AuthServiceTest.cpp` against `tests/support/FakeRepositories.h` |
| Endpoints end to end | `tests/integration/AuthEndpointTest.cpp` against a real server and database |

The fakes are hand-written rather than gmock: the repository interfaces return coroutines,
which gmock expresses awkwardly, and the fakes have to model real behaviour (unique emails,
single-use tokens, revocation) for the tests to mean anything.

Denial paths are tested at least as carefully as the happy path — that is where the security
properties actually live.
