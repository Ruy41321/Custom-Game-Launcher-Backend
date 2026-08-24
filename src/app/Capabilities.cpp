#include "app/Capabilities.h"

#include "domain/CrashReport.h"
#include "domain/LauncherRelease.h"
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

    // Videos get their own limit, their own cap and their own list, because a client that read
    // only `maxBytes` would offer a trailer upload and watch it refused at 5 MiB — and because a
    // server too old to carry these keys is one that cannot store a video at all. That is the
    // asymmetry with `mail.enabled`, whose absence means "an ordinary server that sends mail":
    // here absence means the feature does not exist, so a client reading nothing here is right
    // to offer nothing.
    media["maxVideoBytes"] = static_cast<Json::Int64>(config.media.maxVideoBytes);
    media["maxVideosPerGame"] = domain::MAX_VIDEOS_PER_GAME;

    Json::Value videoFormats(Json::arrayValue);
    for (const auto format : {domain::VideoFormat::Mp4, domain::VideoFormat::WebM}) {
        videoFormats.append(domain::contentTypeOf(format));
    }
    media["videoContentTypes"] = videoFormats;
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

    Json::Value mail;
    // Whether this deployment can send a message at all. A launcher reads it to decide whether
    // to offer "forgotten your password?", which is otherwise a button that answers 404 — the
    // routes that send are already switched off with the transport, and a client left to infer
    // that from a failed request shows the offer first and the failure second. Where it is
    // false the way back in is an operator handing out a one-time password, so the sentence
    // that replaces the link is "contact the administrator" rather than an error.
    mail["enabled"] = config.mail.enabled();
    json["mail"] = mail;

    Json::Value updates;
    // Advisory, and worth publishing: it is why a client that asked for a delta is sometimes
    // handed a full download instead.
    updates["fullDownloadThresholdRatio"] = config.updates.fullDownloadThresholdRatio;
    json["updates"] = updates;

    Json::Value launcherReleases;
    // Whether this deployment publishes releases of the launcher at all — false when no signing
    // key is configured. A launcher reads it at start-up and stops asking, which is the
    // difference between "this server does not do that" and one failed request per run that
    // looks like an outage. Nothing secret is exposed by saying so: the answer is the same for
    // everybody, which is what makes this whole document readable with no token.
    launcherReleases["enabled"] = config.launcherReleases.enabled();

    // The channel names, from the same enum the route parses, so a channel added there appears
    // here without anybody remembering to add it — the reasoning `media.contentTypes` uses.
    Json::Value channels(Json::arrayValue);
    for (const auto channel : {domain::ReleaseChannel::Stable, domain::ReleaseChannel::Beta}) {
        channels.append(domain::nameFor(channel));
    }
    launcherReleases["channels"] = channels;
    json["launcherReleases"] = launcherReleases;

    return json;
}

} // namespace launcher::app
