#include <spdlog/spdlog.h>

#include <iostream>
#include <string>
#include <vector>

#include "app/Bootstrap.h"
#include "app/Config.h"
#include "app/RoleGrant.h"
#include "common/EnvInterpolation.h"
#include "common/Logging.h"
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
              << "  --config <path>        Configuration file to load\n"
              << "  --version              Print the version and exit\n"
              << "  --help                 Show this message\n\n"
              << "Without --migrate or --grant-role the HTTP server is started.\n\n"
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

    return migrateOnly ? launcher::app::runMigrations(appConfig)
                       : launcher::app::runServer(appConfig);
}
