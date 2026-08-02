#include "integration/TestDatabase.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace launcher::testing {
namespace {

std::optional<std::string> env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::string uniqueDatabaseName() {
    static std::atomic<unsigned> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "launcher_test_" << static_cast<unsigned long long>(stamp) << '_'
         << counter.fetch_add(1);
    return name.str();
}

std::string errorOf(PGconn* connection) {
    const char* message = PQerrorMessage(connection);
    std::string text = message == nullptr ? std::string{} : std::string(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

std::optional<TestDatabaseSettings> TestDatabaseSettings::fromEnvironment() {
    const auto host = env("LAUNCHER_TEST_DB_HOST");
    if (!host.has_value()) {
        return std::nullopt;
    }

    TestDatabaseSettings settings;
    settings.host = *host;
    if (const auto port = env("LAUNCHER_TEST_DB_PORT"); port.has_value()) {
        settings.port = static_cast<uint16_t>(std::stoi(*port));
    }
    settings.user = env("LAUNCHER_TEST_DB_USER").value_or(settings.user);
    settings.password = env("LAUNCHER_TEST_DB_PASSWORD").value_or(settings.password);
    settings.adminDatabase = env("LAUNCHER_TEST_DB_ADMIN").value_or(settings.adminDatabase);
    return settings;
}

std::string TestDatabaseSettings::connectionStringFor(const std::string& database) const {
    std::ostringstream out;
    out << "host=" << host << " port=" << port << " dbname=" << database << " user=" << user
        << " password=" << password;
    return out.str();
}

std::unique_ptr<TestDatabase> TestDatabase::createOrNull() {
    const auto settings = TestDatabaseSettings::fromEnvironment();
    if (!settings.has_value()) {
        return nullptr;
    }

    const auto name = uniqueDatabaseName();

    PGconn* admin = PQconnectdb(settings->connectionStringFor(settings->adminDatabase).c_str());
    if (PQstatus(admin) != CONNECTION_OK) {
        std::cerr << "cannot reach the test database server: " << errorOf(admin) << '\n';
        PQfinish(admin);
        return nullptr;
    }

    PGresult* created = PQexec(admin, ("CREATE DATABASE \"" + name + "\"").c_str());
    const bool ok = PQresultStatus(created) == PGRES_COMMAND_OK;
    if (!ok) {
        std::cerr << "cannot create test database: " << errorOf(admin) << '\n';
    }
    PQclear(created);
    PQfinish(admin);

    if (!ok) {
        return nullptr;
    }

    PGconn* connection = PQconnectdb(settings->connectionStringFor(name).c_str());
    if (PQstatus(connection) != CONNECTION_OK) {
        std::cerr << "cannot connect to test database: " << errorOf(connection) << '\n';
        PQfinish(connection);
        return nullptr;
    }

    return std::unique_ptr<TestDatabase>(new TestDatabase(*settings, name, connection));
}

TestDatabase::TestDatabase(TestDatabaseSettings settings, std::string name, PGconn* connection)
    : settings_(std::move(settings)),
      name_(std::move(name)),
      connection_(connection) {}

TestDatabase::~TestDatabase() {
    client_.reset();

    if (connection_ != nullptr) {
        PQfinish(connection_);
        connection_ = nullptr;
    }

    PGconn* admin = PQconnectdb(settings_.connectionStringFor(settings_.adminDatabase).c_str());
    if (PQstatus(admin) == CONNECTION_OK) {
        // FORCE terminates any connection the test left behind, so the drop cannot fail
        // just because a pooled client has not finished closing.
        PGresult* dropped =
            PQexec(admin, ("DROP DATABASE IF EXISTS \"" + name_ + "\" WITH (FORCE)").c_str());
        if (PQresultStatus(dropped) != PGRES_COMMAND_OK) {
            std::cerr << "warning: could not drop test database " << name_ << ": "
                      << errorOf(admin) << '\n';
        }
        PQclear(dropped);
    }
    PQfinish(admin);
}

const drogon::orm::DbClientPtr& TestDatabase::client() {
    if (!client_) {
        client_ = drogon::orm::DbClient::newPgClient(settings_.connectionStringFor(name_), 2);
    }
    return client_;
}

void TestDatabase::exec(const std::string& sql) {
    if (!tryExec(sql)) {
        throw std::runtime_error("test setup statement failed: " + errorOf(connection_) +
                                 "\n  sql: " + sql);
    }
}

bool TestDatabase::tryExec(const std::string& sql, const std::vector<std::string>& parameters) {
    PGresult* result = nullptr;
    if (parameters.empty()) {
        result = PQexec(connection_, sql.c_str());
    } else {
        std::vector<const char*> values;
        values.reserve(parameters.size());
        for (const auto& parameter : parameters) {
            values.push_back(parameter.c_str());
        }
        result = PQexecParams(connection_,
                              sql.c_str(),
                              static_cast<int>(values.size()),
                              nullptr,
                              values.data(),
                              nullptr,
                              nullptr,
                              0);
    }

    const auto status = PQresultStatus(result);
    const bool ok = status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
    PQclear(result);
    return ok;
}

std::vector<std::vector<std::string>> TestDatabase::query(const std::string& sql) {
    PGresult* result = PQexec(connection_, sql.c_str());
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        const auto message = errorOf(connection_);
        PQclear(result);
        throw std::runtime_error("test query failed: " + message + "\n  sql: " + sql);
    }

    const int rows = PQntuples(result);
    const int columns = PQnfields(result);

    std::vector<std::vector<std::string>> table;
    table.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        std::vector<std::string> values;
        values.reserve(static_cast<std::size_t>(columns));
        for (int column = 0; column < columns; ++column) {
            values.emplace_back(PQgetvalue(result, row, column));
        }
        table.push_back(std::move(values));
    }

    PQclear(result);
    return table;
}

std::string TestDatabase::scalar(const std::string& sql) {
    const auto table = query(sql);
    if (table.empty() || table.front().empty()) {
        throw std::runtime_error("test query returned no value\n  sql: " + sql);
    }
    return table.front().front();
}

bool TestDatabase::scalarBool(const std::string& sql) {
    return scalar(sql) == "t";
}

int TestDatabase::scalarInt(const std::string& sql) {
    return std::stoi(scalar(sql));
}

}  // namespace launcher::testing
