// jwt-cpp ships no default JSON backend; a traits header has to be chosen explicitly.
// jsoncpp is used because Drogon already depends on it, so the alternative (picojson) would
// mean pulling in a second JSON library for one file.
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/traits.h>

#include <algorithm>
#include <utility>

#include "common/Random.h"
#include "services/TokenService.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;
using JwtTraits = jwt::traits::open_source_parsers_jsoncpp;
using JwtClaim = jwt::basic_claim<JwtTraits>;

constexpr const char* CLAIM_PERMISSIONS = "permissions";
constexpr const char* CLAIM_EMAIL = "email";
/// Absent is read as false, which is what keeps deploying this from invalidating the tokens
/// already in flight: one minted before the claim existed decodes as an ordinary session.
constexpr const char* CLAIM_PASSWORD_CHANGE_REQUIRED = "pwd_change";

/// One message for every rejection reason. An attacker probing with forged tokens learns
/// nothing about which part of the check failed.
Result<AccessTokenClaims> rejected() {
    return Result<AccessTokenClaims>::failure(ErrorCode::Unauthenticated,
                                              "the access token is missing, expired or invalid");
}

} // namespace

bool AccessTokenClaims::hasPermission(std::string_view permission) const {
    return std::find(permissions.begin(), permissions.end(), permission) != permissions.end();
}

JwtTokenService::JwtTokenService(TokenSettings settings)
    : settings_(std::move(settings)) {}

std::string JwtTokenService::issueAccessToken(const AccessTokenClaims& claims) const {
    const auto now = std::chrono::system_clock::now();

    Json::Value permissions(Json::arrayValue);
    for (const auto& permission : claims.permissions) {
        permissions.append(permission);
    }

    return jwt::create<JwtTraits>()
        .set_issuer(settings_.issuer)
        .set_type("JWT")
        .set_id(common::randomUuid())
        .set_subject(claims.userId)
        .set_issued_at(now)
        .set_not_before(now)
        .set_expires_at(now + settings_.accessTokenTtl)
        .set_payload_claim(CLAIM_EMAIL, JwtClaim(claims.email))
        .set_payload_claim(CLAIM_PERMISSIONS, JwtClaim(permissions))
        .set_payload_claim(CLAIM_PASSWORD_CHANGE_REQUIRED,
                           JwtClaim(Json::Value(claims.passwordChangeRequired)))
        .sign(jwt::algorithm::hs256{settings_.secret});
}

Result<AccessTokenClaims> JwtTokenService::verifyAccessToken(std::string_view token) const {
    if (token.empty()) {
        return rejected();
    }

    try {
        const auto decoded = jwt::decode<JwtTraits>(std::string(token));

        // allow_algorithm pins HS256, which is what closes the "alg: none" bypass: a token
        // presenting any other algorithm is rejected before its signature is even considered.
        jwt::verify<JwtTraits>()
            .allow_algorithm(jwt::algorithm::hs256{settings_.secret})
            .with_issuer(settings_.issuer)
            .verify(decoded);

        AccessTokenClaims claims;
        claims.userId = decoded.get_subject();

        if (decoded.has_payload_claim(CLAIM_EMAIL)) {
            claims.email = decoded.get_payload_claim(CLAIM_EMAIL).as_string();
        }

        if (decoded.has_payload_claim(CLAIM_PERMISSIONS)) {
            const Json::Value raw = decoded.get_payload_claim(CLAIM_PERMISSIONS).to_json();
            if (raw.isArray()) {
                for (const auto& entry : raw) {
                    if (entry.isString()) {
                        claims.permissions.push_back(entry.asString());
                    }
                }
            }
        }

        if (decoded.has_payload_claim(CLAIM_PASSWORD_CHANGE_REQUIRED)) {
            const Json::Value raw =
                decoded.get_payload_claim(CLAIM_PASSWORD_CHANGE_REQUIRED).to_json();
            // Anything that is not a JSON true reads as false, deliberately: this flag only
            // ever *adds* a restriction, so a malformed claim must not be able to impose one.
            claims.passwordChangeRequired = raw.isBool() && raw.asBool();
        }

        if (claims.userId.empty()) {
            return rejected();
        }

        return Result<AccessTokenClaims>::success(std::move(claims));
    } catch (const std::exception&) {
        // Covers a malformed token, a bad signature, a wrong issuer and expiry alike.
        return rejected();
    }
}

} // namespace launcher::services
