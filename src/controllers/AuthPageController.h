#pragma once

#include <drogon/HttpSimpleController.h>

namespace launcher::controllers {

/// Where a verification link lands.
///
/// Outside `/api/v1/` on purpose: this is a page for a person, not a route for a client, and
/// the version in the path is a promise about a wire contract. It answers `GET` only, and
/// serves the same bytes whatever the token is — deciding anything about a token here would
/// mean a mail provider's link scanner could spend it.
class VerifyEmailPageController : public drogon::HttpSimpleController<VerifyEmailPageController> {
  public:
    PATH_LIST_BEGIN
    PATH_ADD("/verify-email", drogon::Get);
    PATH_LIST_END

    void
    asyncHandleHttpRequest(const drogon::HttpRequestPtr& request,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) override;
};

/// Where a password-reset link lands. Carries a form; the reset itself is the same JSON route
/// a client would call.
class PasswordResetPageController
    : public drogon::HttpSimpleController<PasswordResetPageController> {
  public:
    PATH_LIST_BEGIN
    PATH_ADD("/password-reset", drogon::Get);
    PATH_LIST_END

    void
    asyncHandleHttpRequest(const drogon::HttpRequestPtr& request,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) override;
};

} // namespace launcher::controllers
