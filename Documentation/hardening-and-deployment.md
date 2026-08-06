# Hardening and deployment

What this server does to protect itself, and — in the second half — what it does **not** do
because a deployment has to do it. The split is the point of this page: everything in §1 to §5
is code with tests behind it, and everything in §6 is a decision somebody makes on a machine
this repository has never seen.

Related: [authentication.md](authentication.md) for the rate limit on sign-in itself,
[administration.md](administration.md) for the operator surface, and
[architecture.md](architecture.md) §Configuration for how these values are read.

---

## 1. What is already true without any configuration

| | Where |
|---|---|
| Per-address throttle on sign-in, registration and password reset | `filters/AuthRateLimitFilter` |
| Per-address throttle on crash submission, with its own numbers | `filters/CrashRateLimitFilter` |
| Per-account ceiling on every authenticated route | `filters/JwtAuthFilter` — §3 |
| Security headers on every response, including the ones filters refuse with | `app/SecurityHeaders` — §2 |
| Download URLs signed with an expiry, validated by nginx | [downloads-and-deltas.md](downloads-and-deltas.md) |
| The operator surface on a second listener, published to loopback only | [administration.md](administration.md) |
| A body cap for callers with no account, below the one a document route gets | §4 |
| A refusal to start on the placeholder secrets outside development | §5 |

Authorization itself is not on this list because it is not hardening: every check lives in a
service rather than in a controller, and a resource the caller may not see answers 404 rather
than 403. That is the design, described where each surface is.

---

## 2. Headers

Every response carries `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
`Referrer-Policy: no-referrer` and a `Content-Security-Policy` of
`default-src 'none'; frame-ancestors 'none'`. An API that serves no pages can say the strictest
thing there is: nothing here loads anything, and nothing may frame it.

Two details are worth knowing before editing this.

**A header already present is never replaced.** The admin console states a far stricter policy
of its own — it is the one surface here a browser really renders — and overwriting it with the
API's would silently loosen the only page where a policy does any work.

**A response a filter rejects with never reaches post-handling advice.** Every 401, 403 and
throttle on this server is produced by a filter and handed straight back, so an advice-only
implementation would leave exactly the security-relevant responses bare. They are stamped in
`makeErrorResponse` instead, which is the one place every envelope passes through. The cached
not-found page is a third case: Drogon hands one shared object to every request that misses, so
it is stamped once at construction and never touched again — a later write to it is a write two
event loops can make at the same time.

`Strict-Transport-Security` is **off by default** and configured separately, because it is the
one header here that is a promise about the transport rather than a description of the payload.
See §6.

---

## 3. The ceiling an account has

`rateLimit.accountRequests` requests per `rateLimit.accountWindowSeconds`, keyed on the user id
in the verified token, refunded continuously like every other bucket here. Over it: **429** with
`Retry-After`.

It lives inside `JwtAuthFilter` rather than on each route's filter list, and that is the whole
point — every authenticated route on this server already runs through that filter, so there is
no route, present or future, that can be given a token-holding caller with no ceiling at all by
somebody forgetting a line. It runs *after* verification, because the key is the account and an
unverified token names nobody.

The default is deliberately loose (600/60s) and the reason is measurable rather than
aesthetic: the busiest legitimate caller is a build upload, which sends one request per chunk,
so ten requests a second means pushing `uploads.maxChunkBytes` ten times a second — 80 MB/s at
the defaults, faster than any link this project targets. The limit therefore never binds on
work anybody is doing and does bind on a loop. Tightness is the address bucket's job, where each
attempt costs an Argon2id hash.

Buckets are in-process, like the others, which is correct for the single-node deployment this
project targets and becomes a shared-store problem if the API is ever scaled out.

---

## 4. Body limits

Three numbers, and they answer different questions:

| Value | What it bounds |
|---|---|
| `uploads.maxChunkBytes` | One chunk of an upload |
| `server.maxDocumentBytes` | The largest document a route accepts — in practice a big build's manifest |
| `server.maxAnonymousBodyBytes` | What a request carrying no bearer token may send |

The framework's own limit is the larger of the first two, and its *memory* limit stays at the
chunk size: a chunk is read straight back so spilling it to a temporary file would be pure loss,
while the rare manifest that exceeds it is better on disk than held in RAM once per concurrent
request.

`maxDocumentBytes` exists because until it did, the framework limit came from the chunk size
alone — so how many files a build could contain was a silent consequence of a number about
something else entirely, and lowering the chunk size would have quietly lowered it.

The anonymous cap is keyed on the **absence of an `Authorization` header** rather than on a list
of routes, so it cannot go stale when a route is added: nothing anonymous here has a reason to
send a document, and sign-in, a refresh and a crash report are all small. Its honest limit is
worth stating: Drogon reads a request body *before* any of this code runs, so this bounds what a
route will process rather than what the server will buffer. What is buffered is bounded by the
framework limit and by nothing finer — Drogon has no per-route body cap, and a caller attaching
a token that turns out to be invalid is measured against the larger limit before its route
refuses it.

---

## 5. Secrets

Outside development the server refuses to start when `auth.jwtSecret` is shorter than 32
characters, when `storage.secureLinkSecret` or the database password is empty, **and when either
secret still holds a development placeholder**. Three more refusals are about mail and are in
§6.3, because what they need is not a secret but a relay.

The last one is not redundant. The placeholder `docker-compose.yml` falls back to is
thirty-eight characters long and committed to a public repository, so it passed the length check
while being secret from nobody: a deployment that forgot its `.env` signed every token with a
value anybody could read, and started up reporting nothing wrong at all. Refusing it by name
turns that into a start-up failure naming the variable to set.

Generate them with `openssl rand -base64 48`. Rotating either has a consequence worth planning
for: a new `JWT_SECRET` invalidates every access and refresh token in existence, so every
launcher signs in again, and a new `FILE_SECURE_LINK_SECRET` invalidates every download URL
already handed out, so downloads in flight fail and are retried against a fresh plan.

---

## 6. What a deployment has to do, and this repository does not

Everything below is **documented and not implemented**, deliberately. It is configuration for a
machine with a hostname and a certificate, and a compose file shipping TLS that nobody had ever
seen answer would be worse than a page saying what to do.

### 6.1 TLS, which is the big one

`docker-compose.yml` terminates nothing. The `fileserver` nginx serves blobs and artwork over
plain HTTP, and — the part that surprises people — **it does not sit in front of the API at
all**: `api` publishes port 8080 itself. So a deployment adds a reverse proxy and does three
things at once:

1. terminates TLS in front of both the API and the file server, with a certificate for the
   real hostname (Caddy or nginx with certbot; the choice is not this repository's);
2. stops publishing `8080` and `8081` to the world, publishing them to loopback and letting
   the proxy reach them over the compose network instead;
3. moves `FILE_PUBLIC_BASE_URL` and `MEDIA_PUBLIC_BASE_URL` to `https://` — safe to change at
   any time, because a download signature covers the path only and never the host
   (see [downloads-and-deltas.md](downloads-and-deltas.md)).

The admin listener needs nothing here: it is published to `127.0.0.1` and reached over an SSH
tunnel, which is already the strongest thing available.

### 6.2 The two settings that only make sense once §6.1 is done

**`HSTS_ENABLED=true`.** Browsers ignore the header over plain HTTP, so enabling it early is
harmless in production — but it is off in development for a reason that is not: a browser that
receives it from `http://localhost` is pinned to `https://localhost` with no way back short of
clearing browser state. `HSTS_MAX_AGE` defaults to 180 days. `includeSubDomains` and `preload`
are deliberately not offered: both are promises about names this server does not know it has,
and preload in particular is difficult to undo.

**`TRUSTED_PROXIES`.** This is the one that fails silently if it is forgotten. Behind a
terminator every request arrives from the proxy, so **every per-address bucket in this process
collapses onto one shared by every client** — one launcher crash-looping would lock everybody
out of signing in. Set it to the addresses the proxy reaches this server from, as a JSON array
of plain addresses or IPv4 CIDR blocks:

```
TRUSTED_PROXIES=["172.16.0.0/12"]
```

Only then is `X-Forwarded-For` read at all, and it is read **from the right**: the left of that
header is whatever the original caller chose to send, so an implementation taking the first
entry would let anybody hand themselves a fresh bucket per request. With no proxies configured
the header is ignored outright, which is the right answer for a server clients reach directly.

### 6.3 A relay to send from, and the address the links point at

This is the other half of §6.1 and easy to meet late, because the server tells you: outside
development it **refuses to start** rather than accepting registrations nobody can finish.
Three shapes are refused, and each names the variable to set:

- `MAIL_TRANSPORT=smtp` with no `SMTP_HOST` or no `MAIL_FROM_ADDRESS`;
- `MAIL_TRANSPORT=log`, which is the development transport and writes the *body* of every
  message into the log — and the body of a password-reset message is a live credential;
- `MAIL_TRANSPORT=none` together with `REQUIRE_VERIFIED_EMAIL=true`, which is the combination
  that produces accounts nobody can ever sign in to. Turning mail off is legitimate; it just
  has to come with a deployment that does not wait for a link.

What a deployment supplies:

| Variable | What it is |
|---|---|
| `SMTP_HOST`, `SMTP_PORT` | The relay. Anything that speaks SMTP: a provider, or a local Postfix |
| `SMTP_USERNAME`, `SMTP_PASSWORD` | Left empty for a relay that authenticates by network |
| `SMTP_SECURITY` | `starttls` (default), `tls` for implicit TLS on 465, `none` only on a private network |
| `MAIL_FROM_ADDRESS` | The envelope sender. Whatever domain it names has to be one the relay is allowed to send as, or the messages land in spam — see below |
| `MAIL_LINK_BASE_URL` | The origin the links are built from |

Two things worth planning rather than discovering:

- **`MAIL_LINK_BASE_URL` must be the public HTTPS origin once §6.1 is done**, and it is the one
  setting here that is *not* derived from the request. That is deliberate — a `Host` header is
  chosen by whoever is calling, and building a link from it would let a stranger pick the domain
  that appears in somebody else's inbox — but it means an origin that changes has to change
  here too, or every link points at the old one. The pages at `/verify-email` and
  `/password-reset` are served by the API, so this origin must reach the API.
- **Deliverability is a DNS problem, not a code one.** SPF, DKIM and a reverse record for the
  sending host are what decide whether a verification link arrives or is filed as spam, and
  none of them can be configured from this repository. A relay that handles them for you — a
  transactional mail provider — is the shortest path; a VPS sending directly on port 25 is the
  longest.

`SmtpMailSender` verifies the relay's certificate and there is **no switch to turn that off**:
a private certificate authority belongs in the image's trust store. `starttls` is mandatory
when selected, so a relay that does not offer it fails the send rather than quietly carrying
credentials in the clear.

### 6.4 The rest, briefly

- **The host firewall**: nothing but 80 and 443 needs to be reachable. The database publishes no
  port at all and should stay that way.
- **Backups**: the database and `/data/blobs` are the state that matters; artwork is
  content-addressed and re-uploadable, blobs are not.
- **Secret storage**: `.env` on the host, mode 0600, not in version control.
- **Log rotation**: `/var/log/launcher` grows; spdlog rotates by size, the volume does not.
- **Automatic retention** of builds is a *product* gap rather than a deployment one, and is
  described in [storage-lifecycle.md](storage-lifecycle.md).

---

## 7. What this deliberately does not do

- **No WAF, no IP banning, no fail2ban integration.** The buckets refuse a request; nothing here
  remembers an address across a restart or bans one.
- **No CSRF tokens**, and none needed: authentication is a bearer token read from a header, never
  a cookie, so a cross-site request carries no credentials to begin with.
- **No CORS headers.** The client is a desktop application and the console is served from the
  server it talks to. A browser-based client would need a decision here rather than a default.
- **No per-route body limits.** Drogon reads a body before routing; see §4.
- **No secret rotation machinery.** Rotating means editing `.env` and restarting, with the
  consequences in §5.
- **Nothing watches what happens to a message after the relay accepts it.** There is no bounce
  handling, no suppression list and no delivery tracking: a send either was accepted by the
  relay or was not, and that is the whole of what this server knows. An address that starts
  bouncing is a thing an operator learns from their relay, not from here.
