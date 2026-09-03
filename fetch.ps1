# fetch.ps1 -- download, VERIFY, and extract the pinned corvid FFI release
# for this host (Windows). macOS/Linux: fetch.sh.
#
# Binding rules (docs/PLAN.md):
#   - the engine pin is EXACT and lives in ONE variable: $CorvidVersion;
#   - artifacts come only from the tag's GitHub release and are sha256-
#     verified against the release's checksums.txt before extraction;
#   - deps/ is gitignored -- no vendored binaries, ever.
#
# Deterministic + idempotent: re-running with the same pin is a no-op;
# stale engine versions (extracted dirs and old archives) are always
# discarded.

$ErrorActionPreference = "Stop"

# THE pin. Bump here and nowhere else.
$CorvidVersion = "v0.4.1"
$Repo          = "corvid-db/corvid"

$Root = $PSScriptRoot
$Deps = Join-Path $Root "deps"
$Dl   = Join-Path $Deps "dl"

# ---- host platform -> release target ------------------------------------
$Target = switch ($true) {
    { $env:PROCESSOR_ARCHITECTURE -eq "ARM64" } { "aarch64-pc-windows-msvc"; break }
    default                                    { "x86_64-pc-windows-msvc" }
}

$Archive   = "corvid-ffi-$CorvidVersion-$Target.zip"
$BaseUrl   = "https://github.com/$Repo/releases/download/$CorvidVersion"
$Extracted = Join-Path $Deps "corvid-ffi-$CorvidVersion-$Target"

Write-Host "fetch: corvid $CorvidVersion for $Target"

New-Item -ItemType Directory -Force -Path $Dl  | Out-Null
New-Item -ItemType Directory -Force -Path $Deps | Out-Null

# ---- stale-version cleanup: always discard anything not the current pin --
# CMake's configure error tells the user to re-run fetch because "it
# discards stale versions and keeps exactly one" -- so actually do that on
# every run, not only when we're about to download.
Get-ChildItem -Path $Deps -Directory -Filter "corvid-ffi-*" |
    Where-Object { $_.Name -ne "corvid-ffi-$CorvidVersion-$Target" } |
    Remove-Item -Recurse -Force

# ---- download checksums + (if needed) the archive ------------------------
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri "$BaseUrl/checksums.txt" -OutFile (Join-Path $Dl "checksums.txt")

if (Test-Path $Extracted) {
    Write-Host "fetch: $Extracted already present -- verifying stamp only"
} else {
    $ArchivePath = Join-Path $Dl $Archive
    Invoke-WebRequest -Uri "$BaseUrl/$Archive" -OutFile $ArchivePath

    # ---- verify: sha256 against the release's checksums.txt -------------
    $Expected = (Select-String -Path (Join-Path $Dl "checksums.txt") `
        -Pattern "^([0-9a-f]{64})\s+$([regex]::Escape($Archive))\s*$").Matches[0].Groups[1].Value
    if (-not $Expected) {
        Write-Error "fetch: $Archive is not listed in the release checksums.txt"
    }
    $Actual = (Get-FileHash -Algorithm SHA256 -Path $ArchivePath).Hash.ToLower()
    if ($Actual -ne $Expected) {
        Write-Error "fetch: sha256 MISMATCH for ${Archive}: expected $Expected, actual $Actual"
    }
    Write-Host "fetch: sha256 ok ($Actual)"

    # ---- extract ----------------------------------------------------------
    Expand-Archive -Path $ArchivePath -DestinationPath $Deps
}

if (-not (Test-Path (Join-Path $Extracted "corvid.h")) -or
    -not (Test-Path (Join-Path $Extracted "corvid.dll")) -or
    -not (Test-Path (Join-Path $Extracted "corvid.dll.lib"))) {
    Write-Error "fetch: $Extracted is missing corvid.h / corvid.dll / corvid.dll.lib -- bad archive?"
}
$golden = Get-ChildItem -Path (Join-Path $Extracted "golden") -Filter "*.txt"
if (-not $golden) {
    Write-Error "fetch: $Extracted/golden holds no fixtures"
}

# ---- the stamp CMake reads (single source of truth for the version) ------
Set-Content -Path (Join-Path $Deps "version.txt") -Value $CorvidVersion -NoNewline
Write-Host "fetch: deps/corvid-ffi-$CorvidVersion-$Target ready (corvid.h, corvid.dll(+.lib), golden/)"
