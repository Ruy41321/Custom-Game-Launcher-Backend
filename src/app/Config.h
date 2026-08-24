#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/EnvInterpolation.h"
#include "common/Result.h"

namespace launcher::app {

struct ServerConfig {
    std::string listenAddress{"0.0.0.0"};
    uint16_t port{8080};
    /// The admin surface is deliberately a second listener bound to the loopback interface;
    /// it is reached over an SSH tunnel and is never exposed publicly.
    std::string adminListenAddress{"127.0.0.1"};
    uint16_t adminPort{9090};
    bool adminEnabled{false};
    size_t threadCount{0}; ///< 0 = one event loop per hardware thread

    /// The largest *document* body any route accepts, as opposed to the largest upload chunk.
    ///
    /// The manifest of a large build is the only body that comes near it, and until this
    /// existed its ceiling was an accident: the framework's limit was set from
    /// `uploads.maxChunkBytes` alone, so lowering the chunk size silently lowered how big a
    /// build could be published. Declaring it separately makes that ceiling a decision.
    int64_t maxDocumentBytes{16LL * 1024 * 1024};

    /// The largest body a request carrying no bearer token may send.
    ///
    /// Sign-in, a refresh, a password reset and a crash report are all small and all reachable
    /// by anybody; nothing anonymous has a reason to send a document. Note what this does and
    /// does not do: the framework reads a body before any of our code runs, so this bounds what
    /// a route will *process*, while what the server will *buffer* is bounded only by
    /// `maxDocumentBytes` above.
    int64_t maxAnonymousBodyBytes{64 * 1024};

    /// Addresses whose `X-Forwarded-For` is believed, as plain addresses or IPv4 CIDR blocks.
    ///
    /// Empty — the default — means every request is attributed to the peer that sent it, which
    /// is right for a server clients reach directly and wrong the moment one sits behind a
    /// TLS-terminating proxy: every request then arrives from the proxy, and every per-address
    /// limit in the process collapses onto one bucket shared by every client.
    std::vector<std::string> trustedProxies;
};

/// Headers that describe the deployment rather than any one response.
struct SecurityConfig {
    /// Off by default, and deliberately not "on in development": HSTS on a plain-HTTP
    /// development machine pins the browser to `https://localhost` with no way back. Browsers
    /// ignore the header when it arrives over plain HTTP, so a production deployment may
    /// enable it before its TLS terminator exists without breaking anything.
    bool hsts{false};
    uint32_t hstsMaxAgeSeconds{15552000}; ///< 180 days
};

struct DatabaseConfig {
    std::string host{"localhost"};
    uint16_t port{5432};
    std::string name{"launcher"};
    std::string user{"launcher"};
    std::string password;
    size_t connectionCount{4};

    std::string connectionString() const;
};

struct LoggingConfig {
    std::string level{"info"};
    std::string directory;
    bool json{true};
};

struct StorageConfig {
    std::string blobRoot{"/data/blobs"};
    std::string publicBaseUrl{"http://localhost:8081/files"};
    std::string secureLinkSecret;
    uint32_t signedUrlTtlSeconds{3600};
};

struct MediaConfig {
    /// Deliberately a different root from storage.blobRoot: media is served from a public,
    /// unsigned location, and a public location over the build blobs would hand out every
    /// game's files to anyone who learned a hash.
    std::string root{"/data/media"};
    std::string publicBaseUrl{"http://localhost:8081/media"};
    /// A cover, not a texture pack. Enforced before any byte is written, and mirrored into
    /// Drogon's own body limit so an oversized upload is refused rather than buffered.
    int64_t maxBytes{5LL * 1024 * 1024};
    /// A trailer, not a film. Separate from `maxBytes` because the two are different orders of
    /// magnitude and a deployment has no reason to raise both together, and it is the number
    /// that decides the largest body this server accepts at all — see `configureBodyLimits`.
    int64_t maxVideoBytes{64LL * 1024 * 1024};
};

/// Where releases of the *launcher itself* live, and the key that says which ones are real.
struct LauncherReleaseConfig {
    /// A third root, separate from the blobs and from the artwork, for the reason the second one
    /// is separate: this one is served publicly and unsigned. A launcher binary is the most
    /// public thing a deployment holds, and the client asking for it has no token to be handed a
    /// signed URL with — the launcher that most needs an update is the one that cannot sign in.
    std::string root{"/data/launcher"};
    std::string publicBaseUrl{"http://localhost:8081/launcher"};

    /// base64 DER SubjectPublicKeyInfo for the P-256 key releases are signed with — the body of
    /// a PEM `BEGIN PUBLIC KEY` block.
    ///
    /// **Empty is the default and turns the surface off entirely.** There is deliberately no
    /// way to serve an unsigned release: an automatic update is code a machine runs without
    /// anybody looking at it, so a mechanism with the checking switched off would be a channel
    /// straight into every installation that trusts it — worse than having no mechanism.
    ///
    /// The private half is never here, never in this repository and never in its CI. A release
    /// is signed on the machine of whoever cuts it, and this server can only ever check. That is
    /// what makes an attacker holding this deployment able to stop updates and unable to forge
    /// one; see Documentation/hardening-and-deployment.md §6.5.
    std::string publicKey;

    bool enabled() const { return !publicKey.empty(); }
};

struct AuthConfig {
    std::string jwtSecret;
    std::string issuer{"custom-game-launcher"};
    uint32_t accessTokenTtlSeconds{900};
    uint32_t refreshTokenTtlSeconds{2592000};
    uint32_t emailVerificationTtlSeconds{86400};
    uint32_t passwordResetTtlSeconds{3600};
    /// Turned off in development, where there is no mail transport to deliver the link.
    bool requireVerifiedEmail{true};
    /// 0 selects libsodium's INTERACTIVE profile; see decision D14 in CLAUDE.md.
    uint64_t argon2OperationsLimit{0};
    uint64_t argon2MemoryLimitBytes{0};
};

/// How a deployment delivers the two messages the authentication flows cannot work without.
enum class MailTransport {
    /// A real relay, over SMTP. The only value a deployed environment accepts.
    Smtp,
    /// Write the message to the log instead of sending it. Development only, and refused
    /// elsewhere by `validate()`: the body of a reset message is a live credential.
    Log,
    /// Deliberately no mail at all. Then nothing can require a verified address and the routes
    /// that would send answer 404, so a launcher sees a server that does not have the feature.
    None,
};

enum class MailSecurity {
    /// Only ever right for a relay on the same host or the same private network.
    None,
    /// Connect in the clear and refuse to continue unless STARTTLS succeeds.
    StartTls,
    /// TLS from the first byte, the implicit form on port 465.
    Tls,
};

std::optional<MailTransport> parseMailTransport(std::string_view value);

std::optional<MailSecurity> parseMailSecurity(std::string_view value);

struct MailConfig {
    /// The harmless one is the default, and every deployed configuration says `smtp` out loud.
    /// A document that forgets the section entirely therefore fails to start outside
    /// development, with a sentence naming the variable to set, rather than starting and
    /// delivering nothing.
    MailTransport transport{MailTransport::Log};
    std::string host;
    uint16_t port{587};
    std::string username;
    std::string password;
    MailSecurity security{MailSecurity::StartTls};
    std::string fromAddress;
    std::string fromName{"Custom Game Launcher"};

    /// The origin the links in a message are built from.
    ///
    /// Configuration rather than the request's `Host` header, which is chosen by whoever is
    /// calling: building the link from it would let a stranger pick the domain that appears in
    /// a message delivered to somebody else's inbox.
    std::string linkBaseUrl{"http://localhost:8080"};
    std::string productName{"Custom Game Launcher"};

    /// Bounds the whole SMTP conversation. A registration waits for it before it can say
    /// whether the message went out, so it is short.
    uint32_t timeoutSeconds{10};

    /// Its own bucket, and not the authentication one, for the routes that send a message.
    ///
    /// The auth bucket is tight because every attempt there costs an Argon2id hash; these cost
    /// no CPU at all and spend something scarcer — a stranger's inbox, and the deployment's
    /// standing with its relay. Sharing the two would mean one set of numbers was wrong, and
    /// would let a burst of resend requests lock somebody out of signing in.
    uint32_t sendAttempts{3};
    uint32_t sendWindowSeconds{900};

    bool enabled() const { return transport != MailTransport::None; }
};

struct RateLimitConfig {
    /// Attempts allowed per client address before the bucket empties, and the window over
    /// which it refills. Applies to login, registration and password-reset requests.
    uint32_t authAttempts{10};
    uint32_t authWindowSeconds{60};

    /// Requests one *account* may make across every authenticated route, whatever address they
    /// arrive from, and the window the allowance refills over.
    ///
    /// Loose on purpose, and for a measurable reason: the busiest legitimate caller is a build
    /// upload, which sends one request per chunk. At ten a second a client would have to push
    /// `maxChunkBytes` ten times a second — 80 MB/s at the default — to come near it, which no
    /// real link does. So this never binds on work anybody is actually doing, and does bind on
    /// a loop. Tight is the address bucket's job, where each attempt costs an Argon2id hash.
    uint32_t accountRequests{600};
    uint32_t accountWindowSeconds{60};
};

struct UpdateConfig {
    /// When deltaBytes / fullBytes exceeds this ratio the server advises a full download
    /// instead of a delta. See CLAUDE.md §3.
    double fullDownloadThresholdRatio{0.7};
};

struct UploadConfig {
    int64_t defaultQuotaBytes{5LL * 1024 * 1024 * 1024};
    /// Largest single file in a build.
    int64_t maxBlobBytes{2LL * 1024 * 1024 * 1024};
    /// Largest body one upload request may carry. Drogon buffers a request body before the
    /// handler sees it, so this also fixes the framework's own limit — see Bootstrap.
    int64_t maxChunkBytes{8LL * 1024 * 1024};
    /// How long an interrupted upload stays resumable before its staging file is reclaimed.
    uint32_t sessionTtlSeconds{86400};
    /// Bounds the staging disk one account can hold with nothing finished.
    int64_t maxOpenSessionsPerUser{16};
    uint32_t sweepIntervalSeconds{600};
};

struct CrashReportConfig {
    /// Whether launchers may send crash reports at all. On by default: a deployment that does
    /// not want them turns the route off rather than relying on nobody opting in.
    bool enabled{true};
    /// Reports allowed per client address before the bucket empties, and the window it refills
    /// over. The route is unauthenticated, so this is the only thing standing between it and a
    /// stranger filling the table.
    uint32_t submitAttempts{20};
    uint32_t submitWindowSeconds{600};
    /// How long a report is kept. Long enough to notice a crash that only happens on Tuesdays,
    /// short enough that a deployment is not hoarding diagnostics about a version nobody runs.
    uint32_t retentionSeconds{30U * 24 * 3600};
    uint32_t sweepIntervalSeconds{3600};
};

struct RetentionConfig {
    /// How long a blob must have existed before the collector may take it.
    ///
    /// Correctness, not tuning: every blob of a build is uploaded *before* the manifest that
    /// names them, so during a publish live content is referenced by nothing at all. This has
    /// to outlast the slowest publish a deployment expects, which is why it matches the day an
    /// upload session is given.
    uint32_t blobGraceSeconds{86400};
    uint32_t sweepIntervalSeconds{3600};
    /// Blobs one pass may collect, so a large reclaim is spread over several passes instead of
    /// holding an event loop for the whole of it.
    int32_t sweepBatchSize{500};
};

struct AppConfig {
    std::string environment{"development"};
    std::string migrationsDirectory{"migrations"};
    ServerConfig server;
    SecurityConfig security;
    DatabaseConfig database;
    LoggingConfig logging;
    StorageConfig storage;
    MediaConfig media;
    LauncherReleaseConfig launcherReleases;
    AuthConfig auth;
    MailConfig mail;
    RateLimitConfig rateLimit;
    UpdateConfig updates;
    UploadConfig uploads;
    RetentionConfig retention;
    CrashReportConfig crashReports;

    bool isProduction() const;

    /// Parses configuration from a JSON document, expanding `${VAR}` placeholders through
    /// `lookup`. Kept separate from file loading so it can be unit tested without touching
    /// the filesystem or the process environment.
    static common::Result<AppConfig> parse(std::string_view json, const common::EnvLookup& lookup);

    static common::Result<AppConfig> loadFromFile(const std::filesystem::path& path,
                                                  const common::EnvLookup& lookup);

    /// Rejects configurations that are unsafe to run, e.g. a production deployment with no
    /// JWT secret. Called by both loaders.
    common::VoidResult validate() const;
};

/// Resolves which configuration file to use: an explicit path wins, otherwise
/// `$LAUNCHER_CONFIG`, otherwise `config/config.<$LAUNCHER_ENV or development>.json`.
std::filesystem::path resolveConfigPath(const std::string& explicitPath,
                                        const common::EnvLookup& lookup);

} // namespace launcher::app
