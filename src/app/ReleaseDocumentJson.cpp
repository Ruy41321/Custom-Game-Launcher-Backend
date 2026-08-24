#include "app/ReleaseDocumentJson.h"

#include <json/json.h>

#include <memory>
#include <string>

namespace launcher::app {
namespace {

using common::ErrorCode;
using common::Result;

Result<domain::ReleaseDocument> refuse(std::string detail) {
    return Result<domain::ReleaseDocument>::failure(ErrorCode::InvalidInput, std::move(detail));
}

} // namespace

Result<domain::ReleaseDocument> parseReleaseDocument(std::string_view json) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(json.data(), json.data() + json.size(), &root, &errors) ||
        !root.isObject()) {
        return refuse("the release document is not a JSON object");
    }

    domain::ReleaseDocument document;

    if (!root["schema"].isInt()) {
        return refuse("schema must be a number");
    }
    document.schema = root["schema"].asInt();

    const auto channel = domain::parseReleaseChannel(root["channel"].asString());
    if (!channel.has_value()) {
        return refuse("channel must be 'stable' or 'beta'");
    }
    document.channel = *channel;

    auto version = domain::parseSemver(root["version"].asString());
    if (!version.ok()) {
        return refuse("version is not a version number: " + version.error().detail);
    }
    document.version = std::move(version).value();

    const auto platform = domain::parseReleasePlatform(root["platform"].asString());
    if (!platform.has_value()) {
        return refuse("platform must be 'windows', 'linux' or 'macos'");
    }
    document.platform = *platform;

    const auto arch = domain::parseReleaseArch(root["arch"].asString());
    if (!arch.has_value()) {
        return refuse("arch must be 'x64' or 'arm64'");
    }
    document.arch = *arch;

    document.artifactSha256 = root["sha256"].asString();

    if (!root["size"].isInt64()) {
        return refuse("size must be a whole number of bytes");
    }
    document.artifactSize = root["size"].asInt64();

    document.releasedAt = root["releasedAt"].asString();
    document.notes = root["notes"].asString();

    if (auto valid = domain::validateReleaseDocument(document); !valid.ok()) {
        return Result<domain::ReleaseDocument>::failure(valid.error());
    }

    // The check that makes the stored bytes trustworthy. Anything the parse above quietly
    // ignored — an unknown key, a reordering, insignificant whitespace, `1.0` where `1` was
    // meant — shows up here as a difference, because the canonical form is a function of what
    // was understood and the input is what was signed.
    const auto canonical = domain::canonicalReleaseDocument(document);
    if (canonical != json) {
        return refuse(
            "the release document is not in canonical form. It must be exactly the bytes the "
            "signature covers: the nine keys in order, no whitespace, nothing else. Expected:\n" +
            canonical);
    }

    return Result<domain::ReleaseDocument>::success(std::move(document));
}

} // namespace launcher::app
