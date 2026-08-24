#include <gtest/gtest.h>

#include <json/json.h>

#include "app/Capabilities.h"
#include "domain/Manifest.h"
#include "domain/Media.h"

namespace {

using launcher::app::AppConfig;
using launcher::app::capabilitiesDocument;

AppConfig configWith(int64_t chunkBytes, int64_t mediaBytes) {
    AppConfig config;
    config.uploads.maxChunkBytes = chunkBytes;
    config.media.maxBytes = mediaBytes;
    return config;
}

// The whole point of the document: a deployment that narrowed a limit is describable, and the
// client stops having to guess from the defaults in this repository.
TEST(CapabilitiesTest, ReportsTheDeploymentsOwnLimitsRatherThanTheDefaults) {
    const auto json = capabilitiesDocument(configWith(1024 * 1024, 256 * 1024));

    EXPECT_EQ(json["uploads"]["maxChunkBytes"].asInt64(), 1024 * 1024);
    EXPECT_EQ(json["media"]["maxBytes"].asInt64(), 256 * 1024);
}

TEST(CapabilitiesTest, CarriesEverythingAPublisherNeedsBeforeUploading) {
    const auto json = capabilitiesDocument(AppConfig{});

    EXPECT_TRUE(json["uploads"]["maxBlobBytes"].asInt64() > 0);
    EXPECT_TRUE(json["uploads"]["maxOpenSessionsPerUser"].asInt64() > 0);
    EXPECT_TRUE(json["uploads"]["sessionTtlSeconds"].asInt64() > 0);
    EXPECT_EQ(json["manifest"]["maxPathLength"].asInt64(),
              static_cast<Json::Int64>(launcher::domain::MAX_RELATIVE_PATH_LENGTH));
    EXPECT_EQ(json["manifest"]["maxFiles"].asInt64(),
              static_cast<Json::Int64>(launcher::domain::MAX_MANIFEST_ENTRIES));
}

// A client picking a format for an upload needs the list, not its length.
TEST(CapabilitiesTest, NamesTheImageFormatsTheServerAccepts) {
    const auto json = capabilitiesDocument(AppConfig{});
    const auto& formats = json["media"]["contentTypes"];

    ASSERT_TRUE(formats.isArray());
    ASSERT_EQ(formats.size(), 3U);
    EXPECT_EQ(formats[0].asString(), "image/png");
    EXPECT_EQ(formats[1].asString(), "image/jpeg");
    EXPECT_EQ(formats[2].asString(), "image/webp");

    // SVG is refused by the sniffer and must never appear here: the list is what a client
    // would offer in a file picker.
    for (const auto& format : formats) {
        EXPECT_EQ(format.asString().find("svg"), std::string::npos);
    }
}

// The video half of the same argument, and the reason it is a separate key rather than a bigger
// `maxBytes`: a client that read one number would offer a trailer upload and watch it refused
// at the picture limit — or, worse, send one Drogon refuses with a bare 413 that carries no
// message anybody can show.
TEST(CapabilitiesTest, NamesTheVideoLimitsAndFormatsSeparatelyFromThePictureOnes) {
    auto config = AppConfig{};
    config.media.maxBytes = 256 * 1024;
    config.media.maxVideoBytes = 32LL * 1024 * 1024;
    const auto json = capabilitiesDocument(config);

    EXPECT_EQ(json["media"]["maxBytes"].asInt64(), 256 * 1024);
    EXPECT_EQ(json["media"]["maxVideoBytes"].asInt64(), 32LL * 1024 * 1024);
    EXPECT_EQ(json["media"]["maxVideosPerGame"].asInt(), launcher::domain::MAX_VIDEOS_PER_GAME);

    const auto& formats = json["media"]["videoContentTypes"];
    ASSERT_TRUE(formats.isArray());
    ASSERT_EQ(formats.size(), 2U);
    EXPECT_EQ(formats[0].asString(), "video/mp4");
    EXPECT_EQ(formats[1].asString(), "video/webm");

    // The picture list stays what it was: a client offering a screenshot picker must not be
    // handed a container it would then be refused for.
    const auto& pictures = json["media"]["contentTypes"];
    ASSERT_EQ(pictures.size(), 3U);
    for (const auto& picture : pictures) {
        EXPECT_EQ(picture.asString().rfind("image/", 0), 0U);
    }
}

TEST(CapabilitiesTest, ReportsTheGalleryAndAltTextLimits) {
    const auto json = capabilitiesDocument(AppConfig{});

    EXPECT_EQ(json["media"]["maxScreenshotsPerGame"].asInt(),
              launcher::domain::MAX_SCREENSHOTS_PER_GAME);
    EXPECT_EQ(json["media"]["maxAltTextLength"].asInt64(),
              static_cast<Json::Int64>(launcher::domain::MAX_ALT_TEXT_LENGTH));
}

TEST(CapabilitiesTest, ReportsThePagingCeilingsTheCatalogClamps) {
    const auto json = capabilitiesDocument(AppConfig{});

    EXPECT_EQ(json["catalog"]["maxPageSize"].asInt(), 100);
    EXPECT_EQ(json["catalog"]["defaultPageSize"].asInt(), 20);
    EXPECT_EQ(json["catalog"]["maxPatchNotePageSize"].asInt(), 100);
}

// Nothing here may depend on the caller, because the route is served without authentication.
// defaultQuotaBytes is what a new account is given, never what somebody has left.
TEST(CapabilitiesTest, CarriesNothingAboutAnyParticularAccount) {
    AppConfig config;
    config.uploads.defaultQuotaBytes = 42;
    config.database.password = "hunter2";
    config.auth.jwtSecret = "signing-secret";
    config.storage.secureLinkSecret = "link-secret";

    const auto json = capabilitiesDocument(config);
    Json::StreamWriterBuilder writer;
    const auto serialised = Json::writeString(writer, json);

    EXPECT_EQ(json["uploads"]["defaultQuotaBytes"].asInt64(), 42);
    EXPECT_EQ(serialised.find("hunter2"), std::string::npos);
    EXPECT_EQ(serialised.find("signing-secret"), std::string::npos);
    EXPECT_EQ(serialised.find("link-secret"), std::string::npos);
    EXPECT_FALSE(json.isMember("database"));
    EXPECT_FALSE(json.isMember("auth"));
}

// D40 again, on the one switch a client cannot infer without failing first: without this it
// offers "forgotten your password?" and learns from the 404 that the deployment sends nothing.
TEST(CapabilitiesTest, SaysWhetherThisDeploymentCanSendMail) {
    AppConfig sending;
    sending.mail.transport = launcher::app::MailTransport::Smtp;
    EXPECT_TRUE(capabilitiesDocument(sending)["mail"]["enabled"].asBool());

    AppConfig silent;
    silent.mail.transport = launcher::app::MailTransport::None;
    EXPECT_FALSE(capabilitiesDocument(silent)["mail"]["enabled"].asBool());
}

// The log transport is development's, and it does deliver — into the log. A launcher that
// hid the reset link there would make the one environment where the link is easiest to read
// the one where it cannot be asked for.
TEST(CapabilitiesTest, TheLogTransportCountsAsSending) {
    AppConfig config;
    config.mail.transport = launcher::app::MailTransport::Log;

    EXPECT_TRUE(capabilitiesDocument(config)["mail"]["enabled"].asBool());
}

TEST(CapabilitiesTest, SaysWhichApiVersionItDescribes) {
    const auto json = capabilitiesDocument(AppConfig{});

    EXPECT_EQ(json["apiVersion"].asString(), "v1");
    EXPECT_FALSE(json["serverVersion"].asString().empty());
}

} // namespace
