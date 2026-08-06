# Authentication and authorization

Covers registration, email verification, login, session refresh, password reset and the
authorization model. Implemented in `src/services/AuthService.*`, `src/filters/` and
`src/controllers/v1/AuthController.*`.

## Endpoints

All under `/api/v1/auth`. Every response — success or failure — carries `X-Request-Id`.

| Method | Path | Auth | Throttled | Purpose |
|---|---|---|---|---|
| POST | `/register` | – | address | Create an account and send a verification link |
| POST | `/login` | – | address | Exchange credentials for a session |
| POST | `/refresh` | refresh token in body | no | Rotate the session |
| POST | `/logout` | refresh token in body | no | Revoke one session |
| POST | `/verify-email` | – | no | Redeem a verification link |
| POST | `/verify-email/resend` | – | mail | Send a fresh verification link |
| POST | `/password-reset/request` | – | address + mail | Start a reset |
| POST | `/password-reset/confirm` | – | address | Finish a reset |
| GET | `/me` | Bearer access token | no | Current identity and permissions |

And two pages, outside `/api/v1/` because they are for a person rather than for a client:

| Method | Path | Purpose |
|---|---|---|
| GET | `/verify-email?token=…` | Where a verification link lands |
| GET | `/password-reset?token=…` | Where a reset link lands: the form that chooses the password |

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
| Password reset request | Always reports success, with or without a matching account — and sends nothing when there is none, which is the half a caller could otherwise measure |
| Resending a verification link | One sentence for an unknown address, an already confirmed one and a disabled account alike, and nothing is sent in any of the three |
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

The two routes that put a message in somebody's inbox — the resend and the reset request —
carry `MailRateLimitFilter` and a bucket of their own, three per fifteen minutes
(`mail.sendAttempts`). Not the authentication one, and not because a second bucket is tidier:
that one is tight because every attempt behind it costs a hash, while these cost no CPU at all
and spend something scarcer — a stranger's inbox, and the deployment's standing with its
relay. Sharing would mean one of the two sets of numbers was wrong, and would let a burst of
resend requests lock somebody out of signing in. The reset request carries **both**, because
it is a credential-adjacent unauthenticated endpoint *and* it sends a message.

The limiter is in-process. That is correct for the single-node deployment this project
targets and deliberately avoids adding Redis to the compose stack. If the API is ever scaled
out, the buckets become a shared-store problem.

A second, looser bucket applies to a caller that already holds a token: `JwtAuthFilter` charges
every authenticated request against the **account**, so a valid token is no longer a ceiling of
its own. It lives in the filter rather than on each route so no route can be added without it.
Numbers, reasoning and the interaction with an upload are in
[hardening-and-deployment.md](hardening-and-deployment.md) §3.

Which address a bucket is keyed on is not always the peer: behind a TLS terminator it has to
come from `X-Forwarded-For`, and only from a proxy the deployment named. That is §6.2 of the
same page, and getting it wrong collapses every per-address bucket here into one.

## Delivering the two links

Registration and password recovery are both a message arriving somewhere. `AuthService`
decides *when* one is due and holds an `IMailSender`, which is the whole of what it knows
about delivery; the sender lives in `services/` because the caller is a service, and every
rule around sending is therefore unit tested with no socket involved.

**The raw token never leaves `AuthService`.** It is generated, hashed into `user_tokens`,
composed into a message and dropped, inside one function. No response carries it in any
environment — the development affordance that used to return `devEmailVerificationToken` and
`devPasswordResetToken` is gone rather than moved, which is what makes it impossible to bring
back by accident.

### A message that does not go out does not undo a registration

The account is created, the answer says `verificationEmailSent: false`, and the failure is an
error in the log. Unwinding the registration was considered and rejected: it is not one
statement, so undoing it is a compensating delete that can fail on its own — and when it does,
the address is held by an account that cannot sign in and cannot be created again, which turns
an outage at the relay into a lost account. `POST /auth/verify-email/resend` is the way back,
and it exists precisely because this answer is "no" rather than "the whole thing failed".

The send is *awaited* rather than fired and forgotten, bounded by `mail.timeoutSeconds`, so
the answer can say which of the two happened. Telling somebody to check their inbox when the
message never left is a wait with no end.

A password reset cannot say the same thing: its response is identical whether or not the
address belongs to an account, so a failed send there is a log line and nothing else. There is
nothing it could report without reporting that the address exists.

### The pages the links land on

Every route above is a JSON `POST`, and a link in an email is opened by a browser — so without
a page a verification link is a URL nobody can follow. `/verify-email` and `/password-reset`
are two self-contained pages, embedded in the binary from `src/auth/ui/` exactly as the admin
console is, each stating its own `Content-Security-Policy` and `Cache-Control: no-store`,
because the URL carries a single-use token.

**Neither changes anything by being opened.** A mail provider's link scanner fetches every URL
in a message, so a page that confirmed on load would spend the token before its owner clicked
and show them "this link is invalid" for having done nothing. Both pages carry a button that
calls the same JSON route a client would call.

### Configuration

Everything lives under `mail`, and the secrets arrive from the environment like every other
one. `mail.transport` is the switch:

| Transport | What it does |
|---|---|
| `smtp` | A real relay, through libcurl. Requires `mail.host` and `mail.fromAddress` |
| `log` | Writes the message to the log instead of sending it. Development only |
| `none` | This deployment sends no mail at all |

`AppConfig::validate()` refuses three shapes rather than letting them start and deliver
nothing, which is what the debt this replaced actually was:

- `smtp` with no relay or no sender address, in any environment;
- `log` outside development — it writes the *body*, and the body of a reset message is a live
  credential, so a deployment that chose it by accident would be filing credentials into a log
  with a retention policy and an operator audience;
- `none` together with `auth.requireVerifiedEmail`, because then nothing delivers the link and
  nothing lets anybody in without it: every account created would be one that can never sign
  in. With `none` the routes that send answer **404**, the way a disabled crash-report route
  does, so a launcher sees a server without the feature rather than one withholding it.

`mail.linkBaseUrl` is the origin the links are built from, and it comes from configuration and
never from the request's `Host` header: that header is chosen by whoever is calling, and
building the link from it would let a stranger pick the domain that appears in somebody else's
inbox.

`SmtpMailSender` uses libcurl rather than a client written here, chiefly because it verifies
the relay's certificate. There is no switch to turn that off; a private certificate authority
belongs in the image's trust store. `starttls` is *mandatory* when selected — a relay that
does not offer it fails the send rather than carrying credentials in the clear. The
conversation is blocking and runs on a thread of its own, so messages leave one at a time and
a slow relay occupies nothing else.

### The text

English, plain text, no template engine — `services/MailTemplates.*`, two pure functions.
This server has no localisation of any kind and no idea what language an account reads:
nothing stores a locale and no route sends one. Translating two messages would mean a
migration, a change to the registration contract and a second translation system living on
this side, which is a larger thing than the feature it would serve. That is a decision rather
than an oversight.

### `requireVerifiedEmail`

Still configurable, and still off in the development stack so a developer does not open a mail
catcher before every sign-in. In a deployed environment it is on and now *means* something,
because the link it waits for is actually delivered.

## Testing

| Area | Where |
|---|---|
| Validation, hashing, JWT, rate limiting | `tests/unit/` — pure, no I/O |
| AuthService rules | `tests/unit/AuthServiceTest.cpp` against `tests/support/FakeRepositories.h` |
| The message bodies and their links | `tests/unit/MailTemplatesTest.cpp` — pure functions |
| Endpoints end to end | `tests/integration/AuthEndpointTest.cpp` against a real server and database |
| The two pages | `tests/integration/AuthPageEndpointTest.cpp`, over the bytes a deployment serves |

Both suites drive the flows through `tests/support/FakeMailSender.h`, which keeps the message
instead of sending it: with no token in any response, the message is the only place a link
exists, and a test reads it exactly where a person would. Its refusal switch is the more
important half — that a registration survives a send that failed is a behaviour, and this is
what makes it assertable.

**No test speaks SMTP.** There is no mail server in this suite, so `SmtpMailSender` is
verified by hand against a catcher in `docker-compose.override.yml`, the way nginx is
(open debt 8). When you change it, spend the ten minutes.

The fakes are hand-written rather than gmock: the repository interfaces return coroutines,
which gmock expresses awkwardly, and the fakes have to model real behaviour (unique emails,
single-use tokens, revocation) for the tests to mean anything.

Denial paths are tested at least as carefully as the happy path — that is where the security
properties actually live.
