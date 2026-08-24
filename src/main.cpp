#include <spdlog/spdlog.h>

#include <iostream>
#include <string>
#include <vector>

#include "app/Bootstrap.h"
#include "app/Config.h"
#include "app/ReleasePublish.h"
#include "app/RoleGrant.h"
#include "common/EnvInterpolation.h"
#include "common/Logging.h"
#include "domain/LauncherRelease.h"
#include "launcher/Version.h"

namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_USAGE = 2;

void printUsage() {
    std::cout << launcher::APP_NAME << ' ' << launcher::APP_VERSION << "\n\n"
              << "Usage: launcher-api [options]\n\n"
              << "Options:\n"
              << "  --migrate              Apply pending database migrations and exit\n"
              << "  --grant-role <email> <role>\n"
              << "                         Grant a role and exit. The only way to create the\n"
              << "                         first administrator, since granting one through the\n"
              << "                         admin surface needs a role nobody holds yet.\n"
              << "  --publish-release <document> --signature <file> --artifact <file>\n"
              << "                         Publish a launcher release and exit. The document is\n"
              << "                         signed on the machine that cut the release; this\n"
              << "                         server only ever verifies it, and holds no private\n"
              << "                         key at all.\n"
              << "  --retire-release <channel> <platform> <arch> <version>\n"
              << "                         Stop offering a release. Launchers already running it\n"
              << "                         stay where they are; nothing rolls backwards.\n"
              << "  --config <path>        Configuration file to load\n"
              << "  --version              Print the version and exit\n"
              << "  --help                 Show this message\n\n"
              << "Without one of the commands above the HTTP server is started.\n\n"
              << "Configuration is resolved from --config, then $LAUNCHER_CONFIG, then\n"
              << "config/config.$LAUNCHER_ENV.json (defaulting to development).\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    bool migrateOnly = false;
    std::string configPath;
    std::string grantEmail;
    std::string grantRole;
    launcher::app::ReleasePublishRequest publishRequest;
    bool publishRelease = false;
    bool retireRelease = false;
    std::string retireChannel;
    std::string retirePlatform;
    std::string retireArch;
    std::string retireVersion;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return EXIT_OK;
        }
        if (arg == "--version") {
            std::cout << launcher::APP_VERSION << '\n';
            return EXIT_OK;
        }
        if (arg == "--migrate") {
            migrateOnly = true;
            continue;
        }
        if (arg == "--grant-role") {
            if (i + 2 >= args.size()) {
                std::cerr << "--grant-role requires an email address and a role key\n";
                return EXIT_USAGE;
            }
            grantEmail = args[++i];
            grantRole = args[++i];
            continue;
        }
        if (arg == "--publish-release") {
            if (i + 1 >= args.size()) {
                std::cerr << "--publish-release requires the path of a signed release document\n";
                return EXIT_USAGE;
            }
            publishRequest.documentPath = args[++i];
            publishRelease = true;
            continue;
        }
        if (arg == "--signature") {
            if (i + 1 >= args.size()) {
                std::cerr << "--signature requires a path\n";
                return EXIT_USAGE;
            }
            publishRequest.signaturePath = args[++i];
            continue;
        }
        if (arg == "--artifact") {
            if (i + 1 >= args.size()) {
                std::cerr << "--artifact requires a path\n";
                return EXIT_USAGE;
            }
            publishRequest.artifactPath = args[++i];
            continue;
        }
        if (arg == "--retire-release") {
            if (i + 4 >= args.size()) {
                std::cerr << "--retire-release requires a channel, a platform, an architecture "
                             "and a version\n";
                return EXIT_USAGE;
            }
            retireChannel = args[++i];
            retirePlatform = args[++i];
            retireArch = args[++i];
            retireVersion = args[++i];
            retireRelease = true;
            continue;
        }
        if (arg == "--config") {
            if (i + 1 >= args.size()) {
                std::cerr << "--config requires a path\n";
                return EXIT_USAGE;
            }
            configPath = args[++i];
            continue;
        }
        std::cerr << "unknown option: " << arg << "\n\n";
        printUsage();
        return EXIT_USAGE;
    }

    // Bootstrap logging before the configuration is read, so that configuration errors
    // themselves are reported through the normal channel.
    launcher::common::initLogging({});

    const auto lookup = launcher::common::systemEnvLookup();
    const auto resolvedPath = launcher::app::resolveConfigPath(configPath, lookup);

    auto config = launcher::app::AppConfig::loadFromFile(resolvedPath, lookup);
    if (!config.ok()) {
        spdlog::critical("cannot load configuration from {}: {}",
                         launcher::common::escapeJson(resolvedPath.string()),
                         launcher::common::escapeJson(config.error().detail));
        return EXIT_USAGE;
    }

    const auto appConfig = std::move(config).value();
    launcher::common::initLogging(
        {appConfig.logging.level, appConfig.logging.directory, appConfig.logging.json});

    if (!grantEmail.empty()) {
        return launcher::app::runRoleGrant(appConfig, grantEmail, grantRole);
    }

    if (publishRelease) {
        if (publishRequest.signaturePath.empty() || publishRequest.artifactPath.empty()) {
            std::cerr << "--publish-release also needs --signature and --artifact\n";
            return EXIT_USAGE;
        }
        return launcher::app::runReleasePublish(appConfig, publishRequest);
    }

    if (retireRelease) {
        launcher::domain::ReleaseQuery query;
        const auto channel = launcher::domain::parseReleaseChannel(retireChannel);
        const auto platform = launcher::domain::parseReleasePlatform(retirePlatform);
        const auto arch = launcher::domain::parseReleaseArch(retireArch);
        if (!channel.has_value() || !platform.has_value() || !arch.has_value()) {
            std::cerr << "channel must be stable or beta, platform must be windows, linux or "
                         "macos, and arch must be x64 or arm64\n";
            return EXIT_USAGE;
        }
        query.channel = *channel;
        query.platform = *platform;
        query.arch = *arch;
        return launcher::app::runReleaseRetire(appConfig, query, retireVersion);
    }

    return migrateOnly ? launcher::app::runMigrations(appConfig)
                       : launcher::app::runServer(appConfig);
}
