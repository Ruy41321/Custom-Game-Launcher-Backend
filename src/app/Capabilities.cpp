#include "app/Capabilities.h"

#include "domain/CrashReport.h"
#include "domain/Manifest.h"
#include "domain/Media.h"
#include "launcher/Version.h"
#include "repositories/IGameRepository.h"
#include "repositories/IPatchNoteRepository.h"

namespace launcher::app {

Json::Value capabilitiesDocument(const AppConfig& config) {
    Json::Value json;
    json["apiVersion"] = "v1";
    json["serverVersion"] = APP_VERSION;

    Json::Value uploads;
    // The one every client gets wrong: a chunk larger than this is refused, and nothing in the
    // refusal says how large it may be.
    uploads["maxChunkBytes"] = static_cast<Json::Int64>(config.uploads.maxChunkBytes);
    uploads["maxBlobBytes"] = static_cast<Json::Int64>(config.uploads.maxBlobBytes);
    uploads["maxOpenSessionsPerUser"] =
        static_cast<Json::Int64>(config.uploads.maxOpenSessionsPerUser);
    uploads["sessionTtlSeconds"] = static_cast<Json::Int64>(config.uploads.sessionTtlSeconds);
    // What a new account is given. Not what the caller has left: that is on the account, and
    // this document is the same for everybody, which is why it needs no authentication.
    uploads["defaultQuotaBytes"] = static_cast<Json::Int64>(config.uploads.defaultQuotaBytes);
    json["uploads"] = uploads;

    Json::Value manifest;
    manifest["maxPathLength"] = static_cast<Json::Int64>(domain::MAX_RELATIVE_PATH_LENGTH);
    manifest["maxFiles"] = static_cast<Json::Int64>(domain::MAX_MANIFEST_ENTRIES);
    json["manifest"] = manifest;

    Json::Value media;
    media["maxBytes"] = static_cast<Json::Int64>(config.media.maxBytes);
    media["maxScreenshotsPerGame"] = domain::MAX_SCREENSHOTS_PER_GAME;
    media["maxAltTextLength"] = static_cast<Json::Int64>(domain::MAX_ALT_TEXT_LENGTH);

    // Named formats rather than a count, because a client that has to pick a format for an
    // upload needs the list and not its length. Built from the same enum the sniffer answers
    // with, so a format added there appears here without anybody remembering to add it.
    Json::Value formats(Json::arrayValue);
    for (const auto format :
         {domain::ImageFormat::Png, domain::ImageFormat::Jpeg, domain::ImageFormat::WebP}) {
        formats.append(domain::contentTypeOf(format));
    }
    media["contentTypes"] = formats;
    json["media"] = media;

    Json::Value catalog;
    catalog["maxPageSize"] = repositories::MAX_GAME_PAGE_SIZE;
    catalog["defaultPageSize"] = repositories::DEFAULT_GAME_PAGE_SIZE;
    catalog["maxPatchNotePageSize"] = repositories::MAX_PATCH_NOTE_PAGE_SIZE;
    json["catalog"] = catalog;

    Json::Value crashReports;
    // Whether sending them is worth attempting at all. A launcher that reads false here stops
    // rather than posting into a 404 after every crash — and a deployment that turns the route
    // off is telling its users something, not hiding it.
    crashReports["enabled"] = config.crashReports.enabled;
    crashReports["maxMessageLength"] = static_cast<Json::Int64>(domain::MAX_CRASH_MESSAGE_LENGTH);
    crashReports["maxStackLength"] = static_cast<Json::Int64>(domain::MAX_CRASH_STACK_LENGTH);
    json["crashReports"] = crashReports;

    Json::Value updates;
    // Advisory, and worth publishing: it is why a client that asked for a delta is sometimes
    // handed a full download instead.
    updates["fullDownloadThresholdRatio"] = config.updates.fullDownloadThresholdRatio;
    json["updates"] = updates;

    return json;
}

} // namespace launcher::app
