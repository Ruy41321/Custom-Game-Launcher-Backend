#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Compile and run the test suite in the toolchain container, incrementally.

.DESCRIPTION
    This is the fast edit/build/test loop from CLAUDE.md section 7, with the three ways it
    silently misbehaves handled instead of documented:

      * `build/` is configured on first use rather than assumed to exist;
      * the integration database password is read from .env, so the tests cannot skip
        themselves with "LAUNCHER_TEST_DB_HOST is not set" while looking like a pass;
      * the compose database is started first, because the tests need it on the same network.

    The build directory lives on the host and is bind-mounted, so a second run compiles only
    what changed — seconds rather than the several minutes a `docker compose build` takes.

.PARAMETER Unit
    Run only the tests labelled `unit`. They do no I/O and need no database.

.PARAMETER Integration
    Run only the tests labelled `integration`.

.PARAMETER Filter
    Pass a regular expression to ctest -R to run a subset by name.

.PARAMETER Reconfigure
    Delete build/CMakeCache.txt and configure again. Needed after adding or removing a
    source file if the generator does not pick it up.

.PARAMETER Format
    Run clang-format over src/ and tests/ in place, then exit. CI fails on any deviation,
    so this is worth running before every commit.

.PARAMETER BuildOnly
    Compile without running anything.

.EXAMPLE
    ./scripts/test.ps1
    Build and run the whole suite.

.EXAMPLE
    ./scripts/test.ps1 -Unit -Filter Catalog
    Build, then run only the unit tests whose name matches "Catalog".
#>
[CmdletBinding()]
param(
    [switch]$Unit,
    [switch]$Integration,
    [string]$Filter,
    [switch]$Reconfigure,
    [switch]$Format,
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$image = 'custom-game-launcher-api-build'
$network = 'custom-game-launcher_default'

function Write-Step($message) { Write-Host "==> $message" -ForegroundColor Cyan }

docker info --format '{{.ServerVersion}}' *> $null
if ($LASTEXITCODE -ne 0) {
    throw "The Docker daemon is not responding. Start Docker Desktop and try again."
}

Push-Location $repoRoot
try {
    # `docker compose build` tags the image after the project and the service.
    $existing = docker images -q $image
    if (-not $existing) {
        Write-Step "Building the toolchain image (first run only, this compiles vcpkg's dependencies)"
        docker compose --profile tools build api-build
        if ($LASTEXITCODE -ne 0) { throw "Building the toolchain image failed." }
    }

    if ($Format) {
        Write-Step 'Formatting src/ and tests/'
        docker run --rm -v "${repoRoot}:/work" -w /work $image `
            sh -c "find src tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i"
        if ($LASTEXITCODE -ne 0) { throw "clang-format failed." }
        Write-Host '    Formatted.' -ForegroundColor Green
        return
    }

    if ($Reconfigure -and (Test-Path 'build/CMakeCache.txt')) {
        Remove-Item 'build/CMakeCache.txt' -Force
    }

    if (-not (Test-Path 'build/CMakeCache.txt')) {
        Write-Step 'Configuring build/'
        # VCPKG_INSTALLED_DIR points into the image, where the dependencies were installed;
        # VCPKG_MANIFEST_INSTALL is off so configuring does not try to install them again.
        docker run --rm -v "${repoRoot}:/work" -w /work $image `
            cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
                -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake `
                -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed `
                -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_MANIFEST_FEATURES=tests `
                -DLAUNCHER_BUILD_TESTS=ON
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
    }

    Write-Step 'Starting the database'
    docker compose up -d db
    if ($LASTEXITCODE -ne 0) { throw "Could not start the database." }

    # Reading the password rather than defaulting it: a mismatch makes every integration
    # test skip itself, and the skip reads like a pass.
    $dbUser = 'launcher'
    $dbName = 'launcher'
    $dbPassword = 'launcher'
    if (Test-Path '.env') {
        Get-Content '.env' | ForEach-Object {
            if ($_ -match '^\s*DB_USER\s*=\s*(.+?)\s*$') { $dbUser = $Matches[1] }
            if ($_ -match '^\s*DB_NAME\s*=\s*(.+?)\s*$') { $dbName = $Matches[1] }
            if ($_ -match '^\s*DB_PASSWORD\s*=\s*(.+?)\s*$') { $dbPassword = $Matches[1] }
        }
    }

    $ctest = 'ctest --test-dir build --output-on-failure'
    if ($Unit -and -not $Integration) { $ctest += ' -L unit' }
    if ($Integration -and -not $Unit) { $ctest += ' -L integration' }
    if ($Filter) { $ctest += " -R '$Filter'" }

    $command = if ($BuildOnly) { 'cmake --build build --parallel' }
               else { "cmake --build build --parallel && $ctest" }

    Write-Step $(if ($BuildOnly) { 'Building' } else { 'Building and testing' })
    docker run --rm --network $network `
        -e LAUNCHER_TEST_DB_HOST=db `
        -e LAUNCHER_TEST_DB_PORT=5432 `
        -e "LAUNCHER_TEST_DB_USER=$dbUser" `
        -e "LAUNCHER_TEST_DB_PASSWORD=$dbPassword" `
        -e "LAUNCHER_TEST_DB_ADMIN=$dbName" `
        -v "${repoRoot}:/work" -w /work $image `
        sh -c $command
    if ($LASTEXITCODE -ne 0) { throw "The build or the suite failed." }

    Write-Host '    Green.' -ForegroundColor Green
} finally {
    Pop-Location
}
