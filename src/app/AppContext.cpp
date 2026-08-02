#include "app/AppContext.h"

#include <stdexcept>
#include <utility>

namespace launcher::app {

AppContext& AppContext::instance() {
    static AppContext context;
    return context;
}

void AppContext::initialize(AppConfig config, drogon::orm::DbClientPtr database) {
    config_ = std::move(config);
    database_ = std::move(database);
    initialized_ = true;
}

bool AppContext::initialized() const noexcept {
    return initialized_;
}

const AppConfig& AppContext::config() const {
    if (!initialized_) {
        throw std::logic_error("AppContext used before initialize()");
    }
    return config_;
}

const drogon::orm::DbClientPtr& AppContext::database() const {
    if (!initialized_) {
        throw std::logic_error("AppContext used before initialize()");
    }
    return database_;
}

void AppContext::reset() {
    initialized_ = false;
    database_.reset();
    config_ = AppConfig{};
}

} // namespace launcher::app
