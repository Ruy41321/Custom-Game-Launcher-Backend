# Crash reports

Where a launcher's crash goes when its owner has opted in to sending it, and what an operator
can do with the pile afterwards. Implemented in `src/services/CrashReportService.*`,
`src/domain/CrashReport.*`, `src/repositories/postgres/PgCrashReportRepository.*`,
`src/controllers/v1/CrashReportController.*` and
`src/controllers/admin/AdminCrashController.*`.

The client half — when a report is written, what is stripped out of it before it is stored, and
when it is sent — is in the launcher repository's `Documentation/logging-and-local-state.md`.

## Endpoints

| Method | Path | Auth | Purpose |
|---|---|---|---|
| POST | `/api/v1/crash-reports` | **none** | A launcher sends what it wrote down when it died |
| GET | `/admin/api/crashes` | `admin.crashes.read` | The distinct bugs, most recently seen first |
| GET | `/admin/api/crashes/reports?fingerprint=` | `admin.crashes.read` | The individual reports, optionally one bug's |
| GET | `/admin/api/crashes/reports/{id}` | `admin.crashes.read` | One report in full |

The three administrative routes live on the loopback listener like every other `/admin` route
and are 404 on the public one; see [administration.md](administration.md).

## Submitting needs no account, deliberately

A launcher crashes on the sign-in screen as readily as anywhere else — more readily, since that
is where a broken configuration shows — so a route that only a signed-in client could reach
would be missing exactly the failures worth having. Requiring a token would also mean the report
was *about* an account, which is the thing the next section exists to avoid.

What stands in for a token is three things:

- **`CrashRateLimitFilter`**, a per-address token bucket with its own numbers (20 per 10 minutes
  by default). Separate from the authentication bucket rather than a second use of it: that one
  is tight because each attempt costs an Argon2id hash, this one is loose because a launcher
  that crashed five times overnight legitimately sends five reports at once — and sharing would
  let a burst of crash reports lock somebody out of signing in.
- **A field-by-field size cap** (`domain::MAX_CRASH_*`). The stack trace is the only field that
  is legitimately long, and 16 KB is generous for a real one and small enough that a thousand
  reports are a few megabytes.
- **`crashReports.enabled`**, which when false makes the route answer **404** rather than a
  refusal. A deployment that does not collect crash reports should look to a launcher exactly
  like one too old to have the route, and the client handles both by giving up quietly.

A successful submission is **202 Accepted** carrying only the fingerprint. Not 201: there is no
resource the caller can go and read. Not an echo of what was sent either — that would make the
route a way for an anonymous caller to find out what the server keeps.

## No report names an account

`crash_reports` has no `user_id`, no installation id, and no foreign key to anything. This is
the decision the whole surface is shaped around, and it is worth stating what it costs: **an
operator cannot ask "which of my testers hit this"**. That is accepted.

The reasoning is that a crash report is a diagnostic about a program rather than a record about
a person, and the moment it names an account it becomes personal data — one more table for the
GDPR erasure in [authentication.md](authentication.md) to reason about, and one more thing a
later session has to remember. Nothing here has to be remembered: there is no column for a
person to be in.

The client does the other half, and the two fail differently on purpose rather than one being
trusted: it strips its own user profile, install and data directories out of the text **before
writing the file**, so the copy on disk is the copy that travels. A message can still carry
whatever a caller put in it, which is precisely why the server does not also record who sent it.

## The fingerprint

Computed **server-side**, from the exception type and the shape of the stack:

```
sha256( exceptionType + "\n" + normalize(stackTrace) )
```

`normalize` keeps the names of types, methods and namespaces and drops everything else — digits,
offsets, addresses and the punctuation around them. Two properties follow, and both are the
point:

- **A rebuild is the same bug.** Line numbers move with every compilation, and a fingerprint
  that noticed them would report every release as a new crash.
- **Two machines failing on two paths are one bug.** The message is deliberately *not* part of
  it: "could not open D:\Games\a.pak" and "could not open C:\...\b.pak" are one failure, and
  folding the message in would split it into as many bugs as there are machines.

It is computed here rather than accepted from the client for two reasons: two client versions
could otherwise disagree about what one bug is, and a client could choose its own grouping and
hide a crash among a thousand distinct ones.

## Reading them

`GET /admin/api/crashes` answers with **groups**, not reports:

```json
{ "items": [ { "fingerprint": "…", "exceptionType": "System.IO.IOException",
               "message": "…", "occurrences": 47,
               "firstSeenAt": "…", "lastSeenAt": "…", "latestReportId": "…" } ],
  "total": 9, "page": 1, "pageSize": 25 }
```

An operator asks two different questions and they want different answers. *What is going wrong*
is a list of distinct bugs; *what happened to this one* is the reports behind it. A single list
of every report answers the second badly and the first not at all — a thousand rows of the same
crash.

The summary shown for a group is the **most recent** report's, via `DISTINCT ON`: a bug's newest
occurrence is the one an operator is about to open, and picking an arbitrary one would show a
message from a version nobody runs. `latestReportId` rides along so the console can open it
without a second query to find one.

A malformed `fingerprint` filter is **refused**, not ignored: quietly dropping it would answer a
question nobody asked and the whole page would look like the answer. Paging is clamped rather
than refused, because a paging control is not worth a failed request.

## Retention

Reports are the only thing on this server nobody ever deletes by hand — a launcher sends one and
forgets it, and an operator reads a list rather than pruning it. So a timer does it:
`CrashReportService::sweepExpired` runs on `crashReports.sweepIntervalSeconds` and removes
anything received longer ago than `crashReports.retentionSeconds`.

Thirty days by default: long enough to notice a crash that only happens on Tuesdays, short
enough that a deployment is not accumulating diagnostics about a version nobody runs any more.

## Configuration

```json
"crashReports": {
  "enabled":              true,
  "submitAttempts":       20,
  "submitWindowSeconds":  600,
  "retentionSeconds":     2592000,
  "sweepIntervalSeconds": 3600
}
```

`enabled` and the two size caps are published by `GET /api/v1/capabilities`, so a launcher knows
whether sending is worth attempting before it tries — the same reasoning as every other value in
that document (D40). The client's fallback for `enabled` is **false**, unlike every other
capability default: the rest are limits on something the launcher was going to do anyway, and
this one is permission to send something about the user.

## What this deliberately does not do

- **No symbolication and no source mapping.** The stack arrives as the runtime formatted it.
- **No alerting.** Nothing emails an operator when a new fingerprint appears; the console has to
  be looked at. A deployment that wants alerts can poll `/admin/api/crashes`.
- **No deduplication on write.** Every report is stored, even the four hundredth of one bug —
  the count is the signal, and a `lastSeenAt` with no rows behind it could not answer "is this
  still happening on the new version".
- **No attachment of logs.** The launcher's rolling log stays on the machine; a crash report is
  one exception, not a session.

## Related documents

- [administration.md](administration.md) — the loopback listener these three routes live on
- [authentication.md](authentication.md) — the erasure this surface is shaped to stay out of
- [architecture.md](architecture.md) §Configuration — the capabilities document
