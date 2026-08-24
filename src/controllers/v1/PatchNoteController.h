#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>

namespace launcher::controllers::v1 {

/// A game's devlog.
///
/// Its own surface rather than a field of the game detail: a devlog grows without bound and
/// pages, while the detail response is a fixed-size description of one game. It is also the
/// one part of the catalog whose drafts a publisher edits repeatedly, so it gets the same
/// create/patch/delete shape as artwork rather than living inside a PATCH on the game.
class PatchNoteController : public drogon::HttpController<PatchNoteController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PatchNoteController::listForGame,
                  "/api/v1/games/{1}/patch-notes",
                  drogon::Get,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(PatchNoteController::create,
                  "/api/v1/games/{1}/patch-notes",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(PatchNoteController::update,
                  "/api/v1/patch-notes/{1}",
                  drogon::Patch,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(PatchNoteController::remove,
                  "/api/v1/patch-notes/{1}",
                  drogon::Delete,
                  "launcher::filters::JwtAuthFilter");
    METHOD_LIST_END

    drogon::Task<> listForGame(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback,
                               std::string idOrSlug);

    drogon::Task<> create(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string idOrSlug);

    drogon::Task<> update(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string noteId);

    drogon::Task<> remove(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string noteId);
};

} // namespace launcher::controllers::v1
