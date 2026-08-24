#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "common/Time.h"
#include "storage/DownloadUrlSigner.h"

namespace {

using launcher::storage::DownloadUrlSigner;
using launcher::storage::SignedUrlSettings;

constexpr const char* BLOB = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
constexpr const char* STORAGE_KEY =
    "ab/cd/abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
constexpr int64_t EXPIRES = 2145916800; // 2038-01-01T00:00:00Z

DownloadUrlSigner signer(std::string baseUrl = "http://localhost:8081/files") {
    SignedUrlSettings settings;
    settings.publicBaseUrl = std::move(baseUrl);
    settings.secret = "super-secret";
    settings.ttl = std::chrono::seconds{3600};
    return DownloadUrlSigner{std::move(settings)};
}

/// The value nginx will compute for this URI, produced independently the way the module's own
/// documentation does it:
///
///   printf '%s' '<expires><uri> <secret>' | openssl md5 -binary | openssl base64
///       | tr '+/' '-_' | tr -d '='
///
/// Hard-coded on purpose. The whole point of the signer is to agree with a program we cannot
/// call from a unit test, so the test has to carry that program's answer.
constexpr const char* NGINX_TOKEN = "Pl4vcdpomrS4V3nWxv_PpA";

TEST(DownloadUrlSignerTest, ProducesTheTokenNginxSecureLinkExpects) {
    const auto url = signer().sign(STORAGE_KEY, EXPIRES);

    EXPECT_EQ(url,
              std::string("http://localhost:8081/files/") + STORAGE_KEY + "?token=" + NGINX_TOKEN +
                  "&expires=2145916800");
}

// Only the path reaches nginx as $uri, so that is all the signature may cover: the same
// deployment must keep working when it is fronted by a different hostname or scheme.
TEST(DownloadUrlSignerTest, SignsThePathOnlyAndNotTheHost) {
    const auto direct = signer("http://localhost:8081/files").sign(STORAGE_KEY, EXPIRES);
    const auto behindTls = signer("https://cdn.example.com/files").sign(STORAGE_KEY, EXPIRES);

    EXPECT_NE(direct.find(NGINX_TOKEN), std::string::npos);
    EXPECT_NE(behindTls.find(NGINX_TOKEN), std::string::npos);
    EXPECT_EQ(behindTls.rfind("https://cdn.example.com/files/", 0), 0U);
}

TEST(DownloadUrlSignerTest, ATrailingSlashOnTheBaseUrlChangesNothing) {
    EXPECT_EQ(signer("http://localhost:8081/files/").sign(STORAGE_KEY, EXPIRES),
              signer("http://localhost:8081/files").sign(STORAGE_KEY, EXPIRES));
}

TEST(DownloadUrlSignerTest, AnotherSecretProducesAnotherToken) {
    SignedUrlSettings guessed;
    guessed.publicBaseUrl = "http://localhost:8081/files";
    guessed.secret = "super-secre7";

    EXPECT_NE(DownloadUrlSigner{guessed}.sign(STORAGE_KEY, EXPIRES),
              signer().sign(STORAGE_KEY, EXPIRES));
}

TEST(DownloadUrlSignerTest, EveryExpiryAndEveryBlobSignsDifferently) {
    const auto url = signer().sign(STORAGE_KEY, EXPIRES);

    EXPECT_NE(signer().sign(STORAGE_KEY, EXPIRES + 1), url);
    EXPECT_NE(signer().sign(std::string("ff/ee/") + BLOB, EXPIRES), url);
}

TEST(DownloadUrlSignerTest, ExpiryIsTheTtlAheadOfTheGivenInstant) {
    const auto now = std::chrono::system_clock::time_point{std::chrono::seconds{1000}};

    EXPECT_EQ(signer().expiryFor(now), 1000 + 3600);
}

TEST(DownloadUrlSignerTest, FormatsAnExpiryAsIso8601Utc) {
    EXPECT_EQ(launcher::common::formatUnixTimeUtc(EXPIRES), "2038-01-01T00:00:00Z");
    EXPECT_EQ(launcher::common::formatUnixTimeUtc(0), "1970-01-01T00:00:00Z");
}

} // namespace
