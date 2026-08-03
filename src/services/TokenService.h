#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "common/Result.h"

namespace launcher::services {

/// What an access token asserts. Permissions are embedded so that authorizing a request
/// costs no database round trip; the price is that a permission change only takes effect
/// when the short-lived access token is refreshed, which is why it is short-lived.
struct AccessTokenClaims {
    std::string userId;
    std::string email;
    std::vector<std::string> permissions;

    bool hasPermission(std::string_view permission) const;
};

struct TokenSettings {
    std::string secret;
    std::string issuer{"custom-game-launcher"};
    std::chrono::seconds accessTokenTtl{900};
};

class ITokenService {
  public:
    virtual ~ITokenService() = default;

    virtual std::string issueAccessToken(const AccessTokenClaims& claims) const = 0;

    /// Verifies signature, issuer and expiry. Any failure is reported as Unauthenticated
    /// with a generic message: telling a caller *why* a token was rejected helps forgery.
    virtual common::Result<AccessTokenClaims> verifyAccessToken(std::string_view token) const = 0;

    virtual std::chrono::seconds accessTokenTtl() const = 0;
};

/// HS256 implementation. Symmetric signing is correct here because the only issuer and the
/// only verifier are the same process; asymmetric keys would buy nothing and add key
/// distribution. Revisit if tokens ever have to be verified by a separate service.
class JwtTokenService : public ITokenService {
  public:
    explicit JwtTokenService(TokenSettings settings);

    std::string issueAccessToken(const AccessTokenClaims& claims) const override;

    common::Result<AccessTokenClaims> verifyAccessToken(std::string_view token) const override;

    std::chrono::seconds accessTokenTtl() const override { return settings_.accessTokenTtl; }

  private:
    TokenSettings settings_;
};

} // namespace launcher::services
