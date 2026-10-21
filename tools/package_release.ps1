param(
    [string]$Version = "0.2.1"
)

$ErrorActionPreference = "Stop"

$project = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$source  = Join-Path $project "bin\RenderDragonShaderLoader"
$dist    = Join-Path $project "dist"
$stage   = Join-Path $dist ("package-" + $Version)
$zip     = Join-Path $dist ("RenderDragonShaderLoader-" + $Version + "-windows-x64.zip")
$toothZip = Join-Path $dist "RenderDragonShaderLoader-client-windows-x64.zip"

if (-not (Test-Path -LiteralPath (Join-Path $source "RenderDragonShaderLoader.dll"))) {
    throw "Missing built DLL under $source"
}
$manifest = Get-Content -LiteralPath (Join-Path $source "manifest.json") -Raw | ConvertFrom-Json
if ($manifest.version -ne $Version) {
    throw "Built manifest version '$($manifest.version)' does not match release '$Version'"
}

if (Test-Path -LiteralPath $stage) {
    $stageFull = [System.IO.Path]::GetFullPath($stage)
    $distFull  = [System.IO.Path]::GetFullPath($dist)
    if (-not $stageFull.StartsWith($distFull + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean stage outside dist: $stageFull"
    }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item -LiteralPath (Join-Path $source "RenderDragonShaderLoader.dll") -Destination $stage
Copy-Item -LiteralPath (Join-Path $source "manifest.json") -Destination $stage
Copy-Item -LiteralPath (Join-Path $source "lang") -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $source "toolchain") -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $source "config") -Destination $stage -Recurse
foreach ($name in @("README.md", "README_EN.md", "CUSTOM-PARAMETERS.md", "LICENSE", "THIRD_PARTY_NOTICES.md")) {
    $file = Join-Path $project $name
    if (Test-Path -LiteralPath $file) {
        Copy-Item -LiteralPath $file -Destination $stage
    }
}

Get-ChildItem -LiteralPath $stage -Recurse -Directory |
    Where-Object { $_.Name -eq "__pycache__" } |
    Remove-Item -Recurse -Force
Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Extension -in @(".pdb", ".pyc", ".log") } |
    Remove-Item -Force

$releaseNotes = @"
# RenderDragonShaderLoader $Version

Windows x64 pre-release for Minecraft Bedrock 1.26.51 and LeviLamina 26.51.x.

Highlights:
- Structured Native IO compatibility reports and exports.
- Native IO preflight is independent from loading: incompatible packs can still be applied.
- Attribute, buffer, and pass-output mapping support.
- Deferred and forward compatibility regressions with optional albite validation.
- A complete custom-parameter author guide in `CUSTOM-PARAMETERS.md`.
- Reproducible Windows build metadata.
- Configuration reset/delete fixes.

Known limitations:
- Configuration names currently accept English/ASCII input only.
- IME/non-English configuration-name input remains frozen.
- Java shader pack import and automatic resource-pack enable/disable remain out of scope.

Install by replacing the RenderDragonShaderLoader mod directory. Do not overwrite
player data or custom shader packs.
"@
$releaseNotes | Set-Content -LiteralPath (Join-Path $stage "RELEASE-NOTES.md") -Encoding utf8

$privacyPatterns = @(
    $env:USERPROFILE,
    $env:USERNAME,
    $project,
    $env:RDL_GAME_DIR,
    "private-archive"
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique

foreach ($pattern in $privacyPatterns) {
    $privacyHits = & rg -a -n -F -- $pattern $stage 2>$null
    if ($LASTEXITCODE -eq 0) {
        $privacyHits | Out-Host
        throw "Release package contains a local path or private identifier"
    }
    if ($LASTEXITCODE -gt 1) {
        throw "Privacy scan failed with exit code $LASTEXITCODE"
    }
}

$privacyRegexes = @(
    "[A-Za-z]:\\Users\\[^\\\r\n]+",
    "[A-Za-z]:\\(?:LeviLauncher|levilauncher|LeviLauncher_Data)"
)
foreach ($pattern in $privacyRegexes) {
    $textHits = & rg -n -i -e $pattern -- $stage 2>$null
    if ($LASTEXITCODE -eq 0) {
        $textHits | Out-Host
        throw "Release text contains a private local path: $pattern"
    }
    if ($LASTEXITCODE -gt 1) {
        throw "Text privacy scan failed with exit code $LASTEXITCODE"
    }
}

$secretPatterns = @(
    "github_pat_[A-Za-z0-9_]{20,}",
    "ghp_[A-Za-z0-9]{20,}",
    "BEGIN (RSA|OPENSSH|EC) PRIVATE KEY",
    "Authorization: Bearer [A-Za-z0-9._-]+"
)
foreach ($pattern in $secretPatterns) {
    $secretHits = & rg -a -n -i -e $pattern -- $stage 2>$null
    if ($LASTEXITCODE -eq 0) {
        $secretHits | Out-Host
        throw "Release package contains a credential-like value: $pattern"
    }
    if ($LASTEXITCODE -gt 1) {
        throw "Credential scan failed with exit code $LASTEXITCODE"
    }
}

if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash

if (Test-Path -LiteralPath $toothZip) { Remove-Item -LiteralPath $toothZip -Force }
Copy-Item -LiteralPath $zip -Destination $toothZip
$hashLines = @(
    "$hash  $([System.IO.Path]::GetFileName($zip))",
    "$hash  $([System.IO.Path]::GetFileName($toothZip))"
)
$hashLines | Set-Content -LiteralPath (Join-Path $dist ("SHA256SUMS-" + $Version + ".txt")) -Encoding ascii

if (Test-Path -LiteralPath $stage) {
    $stageFull = [System.IO.Path]::GetFullPath($stage)
    $distFull  = [System.IO.Path]::GetFullPath($dist)
    if (-not $stageFull.StartsWith($distFull + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean stage outside dist: $stageFull"
    }
    Remove-Item -LiteralPath $stageFull -Recurse -Force
}

Write-Host "release: $zip"
Write-Host "tooth:   $toothZip"
Write-Host "sha256: $hash"
Write-Host "size:   $((Get-Item -LiteralPath $zip).Length)"
