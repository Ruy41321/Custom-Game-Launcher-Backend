find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# ---------------------------------------------------------------------------
# Unit tests: no database, no network, no filesystem beyond temporary directories.
# ---------------------------------------------------------------------------
add_executable(launcher_unit_tests
    unit/ErrorTest.cpp
    unit/HashTest.cpp
    unit/EnvInterpolationTest.cpp
    unit/ConfigTest.cpp
    unit/MigrationPlanTest.cpp
    unit/ValidationTest.cpp
    unit/PasswordHasherTest.cpp
    unit/JwtTokenServiceTest.cpp
    unit/AuthServiceTest.cpp
    unit/RateLimiterTest.cpp
    unit/CatalogDomainTest.cpp
    unit/BlobStoreTest.cpp
    unit/CatalogServiceTest.cpp
    unit/UploadServiceTest.cpp
    unit/DeltaTest.cpp
    unit/DownloadUrlSignerTest.cpp
    unit/DownloadServiceTest.cpp
)

target_include_directories(launcher_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(launcher_unit_tests
    PRIVATE
        launcher_core
        launcher_warnings
        GTest::gtest
        GTest::gmock
        GTest::gtest_main)

gtest_discover_tests(launcher_unit_tests
    PROPERTIES LABELS unit
    DISCOVERY_TIMEOUT 30)

# ---------------------------------------------------------------------------
# Integration tests: require a reachable throwaway PostgreSQL instance.
# They self-skip when LAUNCHER_TEST_DB_HOST is unset, so `ctest` stays usable
# on a machine with no database.
# ---------------------------------------------------------------------------
add_executable(launcher_integration_tests
    integration/TestDatabase.cpp
    integration/AppHarness.cpp
    integration/MigrationRunnerIntegrationTest.cpp
    integration/HealthEndpointTest.cpp
    integration/AuthEndpointTest.cpp
    integration/CatalogEndpointTest.cpp
    integration/UploadEndpointTest.cpp
    integration/DownloadEndpointTest.cpp
)

target_include_directories(launcher_integration_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(launcher_integration_tests
    PRIVATE
        launcher_core
        launcher_warnings
        GTest::gtest
        GTest::gmock
        GTest::gtest_main)

target_compile_definitions(launcher_integration_tests
    PRIVATE LAUNCHER_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/migrations")

gtest_discover_tests(launcher_integration_tests
    PROPERTIES LABELS integration
    DISCOVERY_TIMEOUT 30)
