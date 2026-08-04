#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Bring the whole backend stack up and wait until it is really serving.

.DESCRIPTION
    Wraps the compose invocations documented in CLAUDE.md section 7 so that starting the
    server is one command instead of four, and so that the two failures that cost the most
    time — a stopped Docker daemon and a missing .env — are reported as themselves rather
    than as a connection error further down.

    The script does not return until /api/v1/health answers, so whatever runs after it can
    assume the API is up.

.PARAMETER Rebuild
    Rebuild the images before starting. Needed after a change to any C++ source or to a
    Dockerfile; a plain start reuses what is already built.

.PARAMETER Down
    Stop the stack and return. Add -Volumes to drop the database and blob volumes with it.

.PARAMETER Volumes
    Only meaningful with -Down: also delete the named volumes, which resets the database and
    every stored blob.

.PARAMETER Logs
    Follow the API log after the stack is healthy. Ctrl-C stops following; it does not stop
    the containers.

.PARAMETER TimeoutSeconds
    How long to wait for the health endpoint before giving up.

.EXAMPLE
    ./scripts/dev.ps1
    Start the stack and wait for it.

.EXAMPLE
    ./scripts/dev.ps1 -Rebuild -Logs
    Rebuild, start, then tail the API log.

.EXAMPLE
    ./scripts/dev.ps1 -Down -Volumes
    Tear everything down, including the data.
#>
[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$Down,
    [switch]$Volumes,
    [switch]$Logs,
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Write-Step($message) { Write-Host "==> $message" -ForegroundColor Cyan }
function Write-Ok($message) { Write-Host "    $message" -ForegroundColor Green }

# The daemon being stopped is the single most common way this fails, and compose reports it
# as a pipe error that reads like a broken installation.
docker info --format '{{.ServerVersion}}' *> $null
if ($LASTEXITCODE -ne 0) {
    throw "The Docker daemon is not responding. Start Docker Desktop and try again."
}

Push-Location $repoRoot
try {
    if ($Down) {
        Write-Step 'Stopping the stack'
        if ($Volumes) { docker compose down -v } else { docker compose down }
        if ($LASTEXITCODE -ne 0) { throw "docker compose down failed." }
        Write-Ok 'Stopped.'
        return
    }

    if (-not (Test-Path '.env')) {
        Write-Step 'No .env found, copying .env.example'
        Copy-Item '.env.example' '.env'
        Write-Ok 'Created .env. Development defaults are usable as they are.'
    }

    if ($Rebuild) {
        Write-Step 'Building and starting api, db and fileserver'
        docker compose up -d --build
    } else {
        Write-Step 'Starting api, db and fileserver'
        docker compose up -d
    }
    if ($LASTEXITCODE -ne 0) { throw "docker compose up failed." }

    # Host ports are configurable, and reporting a URL that is not the one that was published
    # is worse than reporting none.
    $envFile = @{}
    Get-Content '.env' | ForEach-Object {
        if ($_ -match '^\s*([A-Z0-9_]+)\s*=\s*(.*?)\s*$') { $envFile[$Matches[1]] = $Matches[2] }
    }
    $apiPort = if ($envFile.ContainsKey('API_HOST_PORT') -and $envFile['API_HOST_PORT']) { $envFile['API_HOST_PORT'] } else { '8080' }
    $filePort = if ($envFile.ContainsKey('FILESERVER_HOST_PORT') -and $envFile['FILESERVER_HOST_PORT']) { $envFile['FILESERVER_HOST_PORT'] } else { '8081' }
    # /health/ready rather than /health: liveness answers as soon as the listener binds,
    # which is before the database connection is proven. Waiting for the readiness probe is
    # what lets a caller assume the next request will work.
    $healthUrl = "http://localhost:$apiPort/api/v1/health/ready"

    Write-Step "Waiting for $healthUrl"
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        try {
            $response = Invoke-RestMethod -Uri $healthUrl -TimeoutSec 5
            if ($response.status -eq 'ready') { $ready = $true; break }
        } catch {
            # The API is still applying migrations, or the container has not bound yet.
        }
        Start-Sleep -Seconds 2
    }

    if (-not $ready) {
        Write-Host ''
        docker compose logs --tail 40 api
        throw "The API did not become healthy within $TimeoutSeconds seconds. The last of its log is above."
    }

    Write-Ok "API          http://localhost:$apiPort/api/v1"
    Write-Ok "File server  http://localhost:$filePort/files"
    Write-Ok "Database     docker compose exec db psql -U launcher -d launcher"
    Write-Host ''
    Write-Host '    Stop it with:  ./scripts/dev.ps1 -Down' -ForegroundColor DarkGray

    if ($Logs) {
        Write-Host ''
        Write-Step 'Following the API log (Ctrl-C stops following, not the container)'
        docker compose logs -f api
    }
} finally {
    Pop-Location
}
