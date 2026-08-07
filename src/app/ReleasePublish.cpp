#include "app/ReleasePublish.h"

#include <libpq-fe.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "app/ReleaseDocumentJson.h"
#include "common/Signature.h"
#include "storage/ReleaseStore.h"

namespace launcher::app {
namespace {

using common::ErrorCode;
using common::Result;

using ConnectionPtr = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using ResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILURE_CODE = 1;

std::string trimmed(const char* message) {
    std::string text = message == nullptr ? "" : message;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

std::string withoutSurroundingWhitespace(std::string text) {
    const auto isSpace = [](char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };
    while (!text.empty() && isSpace(text.back())) {
        text.pop_back();
    }
    std::size_t start = 0;
    while (start < text.size() && isSpace(text[start])) {
        ++start;
    }
    return text.substr(start);
}

Result<std::string> readWholeFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Result<std::string>::failure(ErrorCode::InvalidInput,
                                            "cannot read " + path.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return Result<std::string>::success(buffer.str());
}

/// One statement, and the insert is gated on nothing succeeding twice. Detecting the duplicate
/// with `ON CONFLICT DO NOTHING RETURNING` rather than by catching a unique-violation is the
/// convention this repository already follows: it is race-free and does not depend on how a
/// driver spells its exceptions.
constexpr const char* PUBLISH_SQL = R"(
    INSERT INTO launcher_releases
        (channel, version, version_major, version_minor, version_patch, platform, arch,
         artifact_sha256, artifact_size, storage_key, document, signature, released_at)
    VALUES ($1, $2, $3::int, $4::int, $5::int, $6, $7, $8, $9::bigint, $10, $11, $12,
            $13::timestamptz)
    ON CONFLICT DO NOTHING
    RETURNING id::text
)";

constexpr const char* RETIRE_SQL = R"(
    UPDATE launcher_releases SET retired_at = now()
    WHERE channel = $1 AND platform = $2 AND arch = $3 AND version = $4 AND retired_at IS NULL
    RETURNING id::text
)";

} // namespace

Result<ReleasePublishOutcome> publishRelease(const AppConfig& config,
                                             const ReleasePublishRequest& request) {
    // Nothing can be published without the key that checks it. A deployment that has not set one
    // up would otherwise store a row it will refuse to serve, which is the kind of half-done
    // state that gets discovered from the client end.
    if (config.launcherReleases.publicKey.empty()) {
        return Result<ReleasePublishOutcome>::failure(
            ErrorCode::InvalidInput,
            "no release signing key is configured: set LAUNCHER_RELEASE_PUBLIC_KEY to the "
            "public half of the key you sign releases with");
    }

    auto documentBytes = readWholeFile(request.documentPath);
    if (!documentBytes.ok()) {
        return Result<ReleasePublishOutcome>::failure(documentBytes.error());
    }
    const auto document = std::move(documentBytes).value();

    auto signatureBytes = readWholeFile(request.signaturePath);
    if (!signatureBytes.ok()) {
        return Result<ReleasePublishOutcome>::failure(signatureBytes.error());
    }
    const auto signature = withoutSurroundingWhitespace(std::move(signatureBytes).value());

    // Parsed first, because the canonical-form check is what makes the stored bytes mean the
    // same thing as the columns beside them, and its message is the one an operator can act on.
    auto parsed = parseReleaseDocument(document);
    if (!parsed.ok()) {
        return Result<ReleasePublishOutcome>::failure(parsed.error());
    }
    const auto release = std::move(parsed).value();

    // Then the signature, before a byte is copied: a document nobody signed costs nothing.
    if (const auto verified =
            common::verifyP256Signature(config.launcherReleases.publicKey, signature, document);
        !verified.ok()) {
        return Result<ReleasePublishOutcome>::failure(
            ErrorCode::InvalidInput,
            "the signature does not check out against the configured public key: " +
                verified.error().detail);
    }

    // Then the artifact, which is refused unless its bytes hash to what the signed document
    // says. This is the step that ties the signature to the file: everything above proves
    // somebody signed a description, and only this proves the file is the thing described.
    const storage::ReleaseStore store{config.launcherReleases.root};
    auto stored = store.store(request.artifactPath, release.artifactSha256);
    if (!stored.ok()) {
        return Result<ReleasePublishOutcome>::failure(stored.error());
    }
    const auto storageKey = std::move(stored).value();

    const ConnectionPtr connection(PQconnectdb(config.database.connectionString().c_str()),
                                   &PQfinish);
    if (PQstatus(connection.get()) != CONNECTION_OK) {
        return Result<ReleasePublishOutcome>::failure(
            ErrorCode::DependencyFailure,
            "cannot connect to the database: " + trimmed(PQerrorMessage(connection.get())));
    }

    const std::string channel = domain::nameFor(release.channel);
    const std::string platform = domain::nameFor(release.platform);
    const std::string arch = domain::nameFor(release.arch);
    const auto major = std::to_string(release.version.major);
    const auto minor = std::to_string(release.version.minor);
    const auto patch = std::to_string(release.version.patch);
    const auto size = std::to_string(release.artifactSize);

    const char* parameters[] = {channel.c_str(),
                                release.version.text.c_str(),
                                major.c_str(),
                                minor.c_str(),
                                patch.c_str(),
                                platform.c_str(),
                                arch.c_str(),
                                release.artifactSha256.c_str(),
                                size.c_str(),
                                storageKey.c_str(),
                                document.c_str(),
                                signature.c_str(),
                                release.releasedAt.c_str()};

    const ResultPtr result(
        PQexecParams(connection.get(), PUBLISH_SQL, 13, nullptr, parameters, nullptr, nullptr, 0),
        &PQclear);

    if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        return Result<ReleasePublishOutcome>::failure(
            ErrorCode::DependencyFailure,
            "cannot record the release: " + trimmed(PQerrorMessage(connection.get())));
    }

    // The artifact is left where it is when the row was already there. It is the same bytes
    // under the same content address by definition, so there is nothing to undo.
    return Result<ReleasePublishOutcome>::success(PQntuples(result.get()) == 0
                                                      ? ReleasePublishOutcome::AlreadyPublished
                                                      : ReleasePublishOutcome::Published);
}

int runReleasePublish(const AppConfig& config, const ReleasePublishRequest& request) {
    auto result = publishRelease(config, request);
    if (!result.ok()) {
        std::cerr << result.error().detail << '\n';
        return EXIT_FAILURE_CODE;
    }

    switch (result.value()) {
    case ReleasePublishOutcome::Published:
        std::cout << "published " << request.documentPath.filename().string() << '\n';
        return EXIT_OK;
    case ReleasePublishOutcome::AlreadyPublished:
        std::cerr << "that channel, platform, architecture and version is already published. "
                     "Raise the version: every launcher refuses a release that is not strictly "
                     "newer than the one it is running, so republishing a number reaches "
                     "nobody.\n";
        return EXIT_FAILURE_CODE;
    }
    return EXIT_FAILURE_CODE;
}

Result<ReleaseRetireOutcome> retireRelease(const AppConfig& config,
                                           const domain::ReleaseQuery& query,
                                           const std::string& version) {
    const ConnectionPtr connection(PQconnectdb(config.database.connectionString().c_str()),
                                   &PQfinish);
    if (PQstatus(connection.get()) != CONNECTION_OK) {
        return Result<ReleaseRetireOutcome>::failure(ErrorCode::DependencyFailure,
                                                     "cannot connect to the database: " +
                                                         trimmed(PQerrorMessage(connection.get())));
    }

    const std::string channel = domain::nameFor(query.channel);
    const std::string platform = domain::nameFor(query.platform);
    const std::string arch = domain::nameFor(query.arch);
    const char* parameters[] = {channel.c_str(), platform.c_str(), arch.c_str(), version.c_str()};

    const ResultPtr result(
        PQexecParams(connection.get(), RETIRE_SQL, 4, nullptr, parameters, nullptr, nullptr, 0),
        &PQclear);

    if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        return Result<ReleaseRetireOutcome>::failure(ErrorCode::DependencyFailure,
                                                     "cannot retire the release: " +
                                                         trimmed(PQerrorMessage(connection.get())));
    }

    return Result<ReleaseRetireOutcome>::success(PQntuples(result.get()) == 0
                                                     ? ReleaseRetireOutcome::NotLive
                                                     : ReleaseRetireOutcome::Retired);
}

int runReleaseRetire(const AppConfig& config,
                     const domain::ReleaseQuery& query,
                     const std::string& version) {
    auto result = retireRelease(config, query, version);
    if (!result.ok()) {
        std::cerr << result.error().detail << '\n';
        return EXIT_FAILURE_CODE;
    }

    switch (result.value()) {
    case ReleaseRetireOutcome::Retired:
        std::cout << "retired " << domain::nameFor(query.channel) << ' '
                  << domain::nameFor(query.platform) << '/' << domain::nameFor(query.arch) << ' '
                  << version << ". Launchers already running it stay where they are.\n";
        return EXIT_OK;
    case ReleaseRetireOutcome::NotLive:
        std::cerr << "no live release matches that channel, platform, architecture and version\n";
        return EXIT_FAILURE_CODE;
    }
    return EXIT_FAILURE_CODE;
}

} // namespace launcher::app
