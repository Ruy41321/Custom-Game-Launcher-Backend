#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "common/Error.h"

namespace launcher::common {

/// Return type for operations whose failure is expected and part of the contract.
/// Truly exceptional conditions throw ApiException instead.
template<typename T>
class Result {
  public:
    static Result success(T value) { return Result(std::move(value)); }

    static Result failure(Error error) { return Result(std::move(error)); }

    static Result failure(ErrorCode code, std::string detail) {
        return Result(Error{code, std::move(detail)});
    }

    bool ok() const noexcept { return std::holds_alternative<T>(data_); }

    explicit operator bool() const noexcept { return ok(); }

    const T& value() const& {
        assert(ok() && "Result::value() on a failed Result");
        return std::get<T>(data_);
    }

    T&& value() && {
        assert(ok() && "Result::value() on a failed Result");
        return std::get<T>(std::move(data_));
    }

    const Error& error() const& {
        assert(!ok() && "Result::error() on a successful Result");
        return std::get<Error>(data_);
    }

  private:
    explicit Result(T value)
        : data_(std::move(value)) {}

    explicit Result(Error error)
        : data_(std::move(error)) {}

    std::variant<T, Error> data_;
};

/// Specialisation for operations that either succeed or fail with no payload.
template<>
class Result<void> {
  public:
    static Result success() { return Result(); }

    static Result failure(Error error) { return Result(std::move(error)); }

    static Result failure(ErrorCode code, std::string detail) {
        return Result(Error{code, std::move(detail)});
    }

    bool ok() const noexcept { return !error_.has_value(); }

    explicit operator bool() const noexcept { return ok(); }

    const Error& error() const {
        assert(!ok() && "Result::error() on a successful Result");
        return *error_;
    }

  private:
    Result() = default;

    explicit Result(Error error)
        : error_(std::move(error)) {}

    std::optional<Error> error_;
};

using VoidResult = Result<void>;

} // namespace launcher::common
