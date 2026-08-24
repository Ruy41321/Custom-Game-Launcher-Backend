#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::services {

/// Cost parameters for Argon2id.
///
/// The defaults are libsodium's INTERACTIVE profile: roughly 64 MiB and a fraction of a
/// second per hash. MODERATE would be stronger but costs 256 MiB *per concurrent hash*,
/// which a handful of simultaneous logins would turn into an out-of-memory kill on the
/// small VPS this project targets. Raise them on a bigger host.
struct PasswordHashingSettings {
    unsigned long long operationsLimit{0}; ///< 0 selects libsodium's INTERACTIVE default
    std::size_t memoryLimitBytes{0};       ///< 0 selects libsodium's INTERACTIVE default
};

class IPasswordHasher {
  public:
    virtual ~IPasswordHasher() = default;

    /// Produces an encoded hash carrying its own salt and parameters, safe to store as-is.
    virtual common::Result<std::string> hash(std::string_view password) const = 0;

    /// Constant-time verification. Returns false for a malformed stored hash rather than
    /// throwing: a corrupt row must not become a way to bypass authentication.
    virtual bool verify(std::string_view password, const std::string& encodedHash) const = 0;

    /// True when the stored hash was produced with weaker parameters than the current
    /// settings, so the caller can transparently upgrade it on the next successful login.
    virtual bool needsRehash(const std::string& encodedHash) const = 0;

    /// Performs a hash of a throwaway value.
    ///
    /// Called on the "user does not exist" login path so that a missing account costs the
    /// same time as a wrong password. Without it, response latency tells an attacker which
    /// email addresses are registered.
    virtual void performDummyHash() const = 0;
};

class Argon2idPasswordHasher : public IPasswordHasher {
  public:
    explicit Argon2idPasswordHasher(PasswordHashingSettings settings = {});

    common::Result<std::string> hash(std::string_view password) const override;

    bool verify(std::string_view password, const std::string& encodedHash) const override;

    bool needsRehash(const std::string& encodedHash) const override;

    void performDummyHash() const override;

  private:
    unsigned long long operationsLimit_;
    std::size_t memoryLimitBytes_;
};

} // namespace launcher::services
