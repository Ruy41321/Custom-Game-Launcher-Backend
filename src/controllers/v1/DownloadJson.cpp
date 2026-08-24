#include "controllers/v1/DownloadJson.h"

#include "common/Time.h"

namespace launcher::controllers::v1 {
namespace {

Json::Value entryToJson(const domain::ManifestEntry& entry) {
    Json::Value json;
    json["path"] = entry.relativePath;
    json["sha256"] = entry.blobSha256;
    json["size"] = static_cast<Json::Int64>(entry.sizeBytes);
    json["executable"] = entry.isExecutable;
    return json;
}

Json::Value fileToJson(const services::DownloadFile& file) {
    auto json = entryToJson(file.entry);
    json["url"] = file.url;
    if (!file.copyFromRelativePath.empty()) {
        // The bytes are already on the client's disk under this path, which the target build
        // keeps unchanged — so the copy is safe whenever the client makes it.
        json["copyFrom"] = file.copyFromRelativePath;
    }
    return json;
}

Json::Value filesToJson(const std::vector<services::DownloadFile>& files) {
    Json::Value array(Json::arrayValue);
    for (const auto& file : files) {
        array.append(fileToJson(file));
    }
    return array;
}

Json::Value pathsToJson(const std::vector<std::string>& paths) {
    Json::Value array(Json::arrayValue);
    for (const auto& path : paths) {
        array.append(path);
    }
    return array;
}

} // namespace

Json::Value downloadPlanToJson(const services::DownloadPlan& plan) {
    Json::Value json;
    json["buildId"] = plan.buildId;
    json["gameId"] = plan.gameId;
    json["versionId"] = plan.gameVersionId;
    json["kind"] = domain::toString(plan.kind);
    json["manifestSha256"] = plan.manifestSha256;
    json["entrypoint"] = plan.entrypointRelativePath;
    json["launchArgs"] = plan.defaultLaunchArgs;

    json["files"] = filesToJson(plan.files);

    Json::Value unchanged(Json::arrayValue);
    for (const auto& entry : plan.unchanged) {
        unchanged.append(entryToJson(entry));
    }
    json["unchanged"] = unchanged;
    json["remove"] = pathsToJson(plan.remove);

    json["downloadBytes"] = static_cast<Json::Int64>(plan.downloadBytes);
    json["totalBytes"] = static_cast<Json::Int64>(plan.totalBytes);
    json["urlsExpireAt"] = common::formatUnixTimeUtc(plan.urlsExpireAt);
    return json;
}

Json::Value integrityReportToJson(const services::IntegrityReport& report) {
    Json::Value json;
    json["buildId"] = report.buildId;
    json["manifestSha256"] = report.manifestSha256;
    json["intact"] = report.intact;
    json["missing"] = pathsToJson(report.missing);
    json["corrupt"] = pathsToJson(report.corrupt);
    json["unexpected"] = pathsToJson(report.unexpected);
    json["repair"] = filesToJson(report.repair);
    json["repairBytes"] = static_cast<Json::Int64>(report.repairBytes);
    json["urlsExpireAt"] = common::formatUnixTimeUtc(report.urlsExpireAt);
    return json;
}

} // namespace launcher::controllers::v1
