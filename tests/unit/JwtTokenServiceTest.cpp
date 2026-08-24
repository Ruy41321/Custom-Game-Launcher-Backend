#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "services/TokenService.h"

namespace {

using launcher::common::ErrorCode;
using launcher::services::AccessTokenClaims;
using launcher::services::JwtTokenService;
using launcher::services::TokenSettings;

constexpr const char* SECRET = "0123456789abcdef0123456789abcdef";

TokenSettings settings(std::chrono::seconds ttl = std::chrono::seconds{900}) {
    return TokenSettings{SECRET, "custom-game-launcher", ttl};
}

AccessTokenClaims sampleClaims() {
    return AccessTokenClaims{"4a3d0d5e-0e2a-4f2f-8f0a-6d5a5c1b2c3d",
                             "dev@example.com",
                             {"library.read", "game.download"}};
}

TEST(JwtTokenServiceTest, RoundTripsTheClaims) {
    const JwtTokenService service(settings());
    const auto claims = sampleClaims();

    const auto verified = service.verifyAccessToken(service.issueAccessToken(claims));

    ASSERT_TRUE(verified.ok()) << verified.error().detail;
    EXPECT_EQ(verified.value().userId, claims.userId);
    EXPECT_EQ(verified.value().email, claims.email);
    EXPECT_EQ(verified.value().permissions, claims.permissions);
}

TEST(JwtTokenServiceTest, IssuesADistinctTokenEachTime) {
    const JwtTokenService service(settings());
    const auto claims = sampleClaims();

    // A jti makes tokens distinguishable even when issued within the same second.
    EXPECT_NE(service.issueAccessToken(claims), service.issueAccessToken(claims));
}

TEST(JwtTokenServiceTest, ExposesPermissionLookup) {
    const auto claims = sampleClaims();

    EXPECT_TRUE(claims.hasPermission("game.download"));
    EXPECT_FALSE(claims.hasPermission("admin.users.manage"));
}

TEST(JwtTokenServiceTest, RejectsATokenSignedWithAnotherSecret) {
    const JwtTokenService issuer(settings());
    const JwtTokenService verifier(TokenSettings{
        "ffffffffffffffffffffffffffffffff", "custom-game-launcher", std::chrono::seconds{900}});

    const auto verified = verifier.verifyAccessToken(issuer.issueAccessToken(sampleClaims()));

    ASSERT_FALSE(verified.ok());
    EXPECT_EQ(verified.error().code, ErrorCode::Unauthenticated);
}

TEST(JwtTokenServiceTest, RejectsATokenFromAnotherIssuer) {
    const JwtTokenService issuer(TokenSettings{SECRET, "someone-else", std::chrono::seconds{900}});
    const JwtTokenService verifier(settings());

    EXPECT_FALSE(verifier.verifyAccessToken(issuer.issueAccessToken(sampleClaims())).ok());
}

TEST(JwtTokenServiceTest, RejectsATamperedPayload) {
    const JwtTokenService service(settings());
    auto token = service.issueAccessToken(sampleClaims());

    // Flip a character inside the payload segment; the signature must no longer match.
    const auto firstDot = token.find('.');
    ASSERT_NE(firstDot, std::string::npos);
    token[firstDot + 5] = token[firstDot + 5] == 'a' ? 'b' : 'a';

    EXPECT_FALSE(service.verifyAccessToken(token).ok());
}

// "alg: none" is the classic JWT bypass: a token with no signature must never verify.
TEST(JwtTokenServiceTest, RejectsAnUnsignedToken) {
    const JwtTokenService service(settings());

    const std::string unsigned_ =
        "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0."
        "eyJpc3MiOiJjdXN0b20tZ2FtZS1sYXVuY2hlciIsInN1YiI6ImF0dGFja2VyIn0.";

    EXPECT_FALSE(service.verifyAccessToken(unsigned_).ok());
}

TEST(JwtTokenServiceTest, RejectsGarbageAndEmptyInput) {
    const JwtTokenService service(settings());

    for (const auto* token : {"", "not-a-token", "a.b.c", "....", "Bearer something"}) {
        EXPECT_FALSE(service.verifyAccessToken(token).ok()) << token;
    }
}

TEST(JwtTokenServiceTest, RejectsAnExpiredToken) {
    const JwtTokenService service(settings(std::chrono::seconds{1}));
    const auto token = service.issueAccessToken(sampleClaims());

    ASSERT_TRUE(service.verifyAccessToken(token).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    const auto verified = service.verifyAccessToken(token);

    ASSERT_FALSE(verified.ok());
    EXPECT_EQ(verified.error().code, ErrorCode::Unauthenticated);
}

// Every rejection reports the same thing, so probing cannot distinguish "expired" from
// "forged".
TEST(JwtTokenServiceTest, GivesTheSameMessageForEveryRejection) {
    const JwtTokenService service(settings());
    const JwtTokenService other(TokenSettings{
        "ffffffffffffffffffffffffffffffff", "custom-game-launcher", std::chrono::seconds{900}});

    const auto forged = service.verifyAccessToken(other.issueAccessToken(sampleClaims()));
    const auto garbage = service.verifyAccessToken("nonsense");

    ASSERT_FALSE(forged.ok());
    ASSERT_FALSE(garbage.ok());
    EXPECT_EQ(forged.error().detail, garbage.error().detail);
}

TEST(JwtTokenServiceTest, CarriesAnEmptyPermissionListWithoutError) {
    const JwtTokenService service(settings());
    AccessTokenClaims claims = sampleClaims();
    claims.permissions.clear();

    const auto verified = service.verifyAccessToken(service.issueAccessToken(claims));

    ASSERT_TRUE(verified.ok()) << verified.error().detail;
    EXPECT_TRUE(verified.value().permissions.empty());
}

TEST(JwtTokenServiceTest, CarriesTheForcedPasswordChange) {
    const JwtTokenService service(settings());
    AccessTokenClaims claims = sampleClaims();
    claims.passwordChangeRequired = true;

    const auto verified = service.verifyAccessToken(service.issueAccessToken(claims));

    ASSERT_TRUE(verified.ok()) << verified.error().detail;
    EXPECT_TRUE(verified.value().passwordChangeRequired);
}

// An ordinary session, and — the case that matters for deploying this — a token minted before
// the claim existed. Both have to read as false, or rolling the flag out would refuse every
// request holding a token issued a minute earlier.
TEST(JwtTokenServiceTest, AnAbsentForcedChangeClaimIsNotAForcedChange) {
    const JwtTokenService service(settings());

    const auto verified = service.verifyAccessToken(service.issueAccessToken(sampleClaims()));

    ASSERT_TRUE(verified.ok()) << verified.error().detail;
    EXPECT_FALSE(verified.value().passwordChangeRequired);
}

} // namespace
