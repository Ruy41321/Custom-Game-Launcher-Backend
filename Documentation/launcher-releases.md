# Launcher releases and signature verification

How a release of the **launcher itself** — as opposed to a build of somebody's game — is
published, stored, served and checked. Implemented in `src/domain/LauncherRelease.*`,
`src/common/Signature.*`, `src/storage/ReleaseStore.*`, `src/services/LauncherReleaseService.*`,
`src/app/{ReleaseDocumentJson,ReleasePublish}.*`,
`src/repositories/postgres/PgLauncherReleaseRepository.*` and
`src/controllers/v1/LauncherController.*`.

The client half — checking, downloading and swapping — lives in the launcher repository and is
**not implemented yet**. This page describes the surface it will talk to.

Related: [builds-and-uploads.md](builds-and-uploads.md) for the content-addressed storage this
reuses, [downloads-and-deltas.md](downloads-and-deltas.md) for why *those* URLs are signed and
these are not, and [hardening-and-deployment.md](hardening-and-deployment.md) §6.5 for where the
private key lives.

---

## The one sentence that explains the whole design

**An automatic update is code a machine runs without anybody looking at it**, so if the check is
weak this is the worst attack surface in the project — a channel straight into every
installation that trusts it. Everything below follows from taking that seriously.

The property this buys, and the only one in this repository that holds after a full compromise:

> Somebody who takes this server, its database and its disks can stop launchers from updating.
> They cannot make them update to anything.

The private key is not here. It never was.

---

## The shape of it

```
 On the machine that cut the release          On the server
 ─────────────────────────────────────        ───────────────────────────────────
 build the artifact                     ──►
 hash it                                      launcher-api --publish-release
 write the release document                     · document is canonical?
 sign the document  (private key)                · signature verifies?     ← public key
                                                 · artifact hashes to it?
                                                 · row + blob

 A launcher                                    GET /api/v1/launcher/releases/latest
 ─────────────────────────────────────         → { document, signature, url }
 verify signature over document
 refuse anything not strictly newer
 fetch url, refuse bytes off-hash
 swap, relaunch
```

## Endpoints

| Method | Path | Permission | Purpose |
|---|---|---|---|
| GET | `/api/v1/launcher/releases/latest` | **none** | The newest live release for one channel/platform/architecture |

Query parameters: `channel` (optional, defaults to `stable`), `platform` and `arch` — both
**required**.

```json
{
  "document": "{\"schema\":1,\"channel\":\"stable\", … }",
  "signature": "MEUCIQD…",
  "url": "https://files.example.com/launcher/9f/86/9f86….zip"
}
```

There is deliberately **no route that creates a release.** Publishing is a command; see below.

### Why it takes no token

The crash-report route is unauthenticated because a crash on the sign-in screen is worth having.
This one is unauthenticated for a sharper reason: **the launcher that most needs an update is
the one that cannot sign in.** Pointed at a server it has never reached, holding an address
nobody confirmed, or carrying the very bug the update fixes. A route behind a token would be
missing exactly the installations an update mechanism exists for.

That is also why this is not part of the catalog. Every catalog route carries an `Actor` and
asks `mayViewGame`; reaching a release through it would mean an unauthenticated path inside
`CatalogService`, which is the one place this server has spent four milestones concentrating its
authorization rules. A table of its own has no such hole to open.

### 404 means three things, on purpose

No signing key configured, nothing published at all, nothing published for this platform. From a
client's side they are one situation — there is nothing to update to — and distinguishing them
would tell a stranger which platforms a deployment builds for.

---

## What is signed

**The document, not the artifact**, and the difference is the security argument rather than a
detail.

A signature over the bytes of a zip says only that somebody with the key once produced that zip.
It says nothing about which version it is, which channel it belongs to, or which platform it
runs on — so an attacker holding the database could serve a genuine, genuinely signed artifact
as something it is not: last year's build as the newest one, or the Linux build to a Windows
launcher. Binding all of it together in one signed document makes that impossible. It is the
same reasoning that keeps the build id *out* of a manifest: what a hash covers is a decision.

### The canonical document

```json
{"schema":1,"channel":"stable","version":"0.2.0","platform":"windows","arch":"x64","sha256":"9f86…","size":83442176,"releasedAt":"2026-08-07T10:00:00Z","notes":"Self-update, at last."}
```

Nine keys, fixed order, no insignificant whitespace, hand-written serialiser — the discipline
`canonicalManifestDocument` already follows, for the same reason: this is a wire contract and it
must not shift if jsoncpp ever changes how it emits an escape. The escaper is shared between the
two (`domain/CanonicalJson.h`), because two documents whose hashes depend on identical output
must not be able to drift apart.

The route serves **exactly these bytes**, and a client checks the signature over what arrived.
Neither side reproduces a canonical form of its own.

Four rules the validator applies, each because of a specific failure:

* **`version` must be the full three-component form.** `0.2` and `0.2.0` are one version
  written two ways, and the unique index cannot see them as one — a re-publish under the other
  spelling would silently become a second row racing to be the newest.
* **`releasedAt` is exactly `YYYY-MM-DDTHH:MM:SSZ`.** A signature covers bytes, so an instant
  that can be spelled several ways is several documents.
* **`sha256` is 64 lowercase hex.** Uppercase would be a second content address for one file.
* **`size` is a positive whole number** under 4 GiB, so a mistyped one cannot describe an
  artifact no disk holds.

### The round-trip check

`parseReleaseDocument` parses, re-serialises, and **refuses anything that is not byte for byte
what it would have written.** One line, and it is load-bearing: without it the row would store
bytes whose meaning had only partly been captured — an extra key, a reordering, a trailing
newline from a text editor — and the columns derived from that parse would describe a document
subtly unlike the one the signature covers.

With it, `document` and the columns beside it cannot mean two different things, and the operator
finds out at publish time rather than never.

---

## The algorithm: ECDSA P-256 with SHA-256

Pinned, not read out of whatever key is configured.

**Ed25519 is the better modern choice and was rejected**, for a reason that lives entirely on
the other side of the wire. libsodium is already linked here, so Ed25519 would cost this
repository nothing — but .NET 9 has no Ed25519 in its base class library, so the launcher would
have to carry either a native binding across four self-contained runtime identifiers or a
managed crypto library. That is a poor trade in a client whose maintainers have refused a
dependency over thirty lines of test code.

P-256 costs nothing on either side: OpenSSL is already linked here for the download-URL
signatures, and `System.Security.Cryptography.ECDsa` is in the client's runtime. No new vcpkg
port, no rebuild of the toolchain image, no new NuGet package.

The two known weaknesses of ECDSA do not reach this use. Nonce quality is a property of
*signing*, which happens on somebody's own machine a few times a year; malleability matters when
a signature is an identifier, which this one never is.

**Pinning matters as much as the choice.** If the algorithm came from the key, a deployment
could be given an RSA key that this server verifies happily and the client does not understand
at all — a launcher that stops updating for a reason nothing reports. Anything that is not a
P-256 key is refused when the configuration is read, so it is a start-up failure naming the
variable.

---

## Publishing

A command, not an endpoint:

```bash
launcher-api --publish-release release.json --signature release.json.sig --artifact launcher.zip
launcher-api --retire-release stable windows x64 0.2.0
```

The authority that fits is shell access to the machine, exactly as it is for `--grant-role`
(D38) and for the same reason turned inside out: there is **no route through which a stolen
token could ever create a release.** Two consequences fall out and both are wanted — the
artifact never has to fit inside an HTTP body limit, and the surface that serves releases cannot
create one, which is why `ILauncherReleaseRepository` has no write method at all.

Like the migration runner and the role grant, it talks to libpq directly: there is no event loop
to run a coroutine on, and destroying a Drogon `DbClient` can land on its own connection thread
and abort the process.

### The order of the checks, and why it is that order

1. **Parse and canonicalise.** Its message is the one an operator can act on, and it costs
   nothing.
2. **Verify the signature**, before a byte is copied. A document nobody signed costs nothing.
3. **Hash the artifact and store it.** This is the step that ties the signature to the file:
   everything above proves somebody signed a *description*, and only this proves the file is the
   thing described. A mismatch is a refusal, never a rename.
4. **Write the row**, with `ON CONFLICT DO NOTHING RETURNING` — race-free, and independent of
   how a driver spells its exceptions.

Publishing the same channel/platform/architecture/version twice is refused. Re-publishing a
number reaches nobody anyway: every client declines a release that is not strictly newer than
what it is running, so a corrected build has to carry a new number.

### Retiring

`retired_at` is set; the row stays, because it is the record of what was once handed out, and
the artifact stays on disk, because a launcher that started the download before the withdrawal
is better off finishing it than failing halfway.

The route stops offering it, so clients fall back to the previous release and then decline it
for being older than what they are running. **Standing still is the correct outcome of
withdrawing a bad build** — rolling a fleet backwards is a bigger action than the one asked for.

---

## Storage

A **third root**, `/data/launcher`, content-addressed as `ab/cd/<sha256>.zip`, served by nginx
**public and unsigned**.

The rule this follows is the one artwork established (D27): *what a root is served as decides
which root it is.* Blobs are behind `secure_link` because a build belongs to whoever published
it. Artwork is public because a cover is public. A launcher binary is the most public thing the
deployment holds — MIT-licensed software everybody downloads — and the client asking for it has
no token to spend on being handed a signed URL.

Serving it unsigned weakens nothing, and this is worth being precise about: the integrity
guarantee comes from the signature over the document and the content address inside it, neither
of which a URL takes part in. An attacker who could rewrite the `url` field entirely would still
have to produce bytes hashing to a value somebody signed. What a signed URL protects is
*confidentiality*, and there is none here to protect.

`ReleaseStore` differs from `MediaStore` in one way: an image arrives whole in a request body
and is hashed in memory, while a launcher is tens of megabytes and arrives as a file, so it is
hashed by streaming and copied through staging inside the root — a rename within one filesystem,
which is atomic.

---

## Configuration

| Key | Default | Meaning |
|---|---|---|
| `launcherReleases.publicKey` | *(empty)* | base64 DER SubjectPublicKeyInfo, P-256. **Empty turns the surface off** |
| `launcherReleases.root` | `/data/launcher` | Where artifacts are stored |
| `launcherReleases.publicBaseUrl` | `http://localhost:8081/launcher` | Where the file server answers |

There is deliberately **no way to serve an unsigned release**, and no flag that relaxes the
check. A deployment that has not set up signing publishes nothing, and a launcher whose own
embedded key is absent asks for nothing — the same answer arrived at from both ends.

`GET /api/v1/capabilities` carries `launcherReleases.enabled` and the channel list, so a
launcher reads it at start-up and stops asking rather than treating every run as a failed check.

---

## What the client must do, and why

Written down here because the client half does not exist yet and these are the rules that make
the server's care worth anything:

1. **Verify the signature over the bytes as they arrived**, before parsing. A document that is
   not the one that was published must never become the one that gets installed — D19's rule for
   manifests, applied to the thing that replaces the launcher itself.
2. **Refuse anything not strictly newer** than the running version. This is the only defence
   against a replayed *correctly signed* old document, which a signature cannot answer by
   itself.
3. **Refuse bytes that do not hash to the content address in the document.** The `url` is a
   convenience, not something to trust.
4. **Never let a failed check stop the launcher from starting.** A launcher that will not open
   because it could not reach the update route would be the worst possible outcome of this
   feature — the reasoning D50 applies to the crash-report uploader, which holds identically
   here.
5. **Keep the public key in the binary, not in `launcher.config.json`.** The file the updater
   overwrites must not be the file that authorizes the update.

---

## What this deliberately does not do

- **No delta updates.** A self-contained launcher changes almost every file between .NET
  builds, so blob negotiation would cost a round trip to learn that everything is needed. One
  archive per platform.
- **No minimum-version enforcement.** A server that can tell a launcher it is too old to talk
  to is a remote kill switch, and it is a one-way door: one row would brick every installation.
  It belongs to the moment a wire contract actually breaks, with its own decision taken then —
  and the field is not reserved either, because an unused column is a thing a later session has
  to be told is unused on purpose.
- **No collection of retired artifacts.** Nothing sweeps `/data/launcher`. Releases are a
  handful of files a year rather than a growing set, and a collector would need the grace-period
  reasoning `storage-lifecycle.md` sets out for blobs.
- **No release notes localisation**, for the reason the mail templates are English: this server
  has no translation system and no locale column.
- **No signing anywhere in this repository.** Nothing in `src/` can produce a signature. The
  only private key in the tree is a test fixture in `tests/support/ReleaseSigning.cpp`, which
  signs nothing outside the test binary.
