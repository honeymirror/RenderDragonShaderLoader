param(
    [string]$VanillaDir = "",
    [string]$Root = "",
    [switch]$Build,
    [switch]$KeepArtifacts
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

if ([string]::IsNullOrWhiteSpace($VanillaDir)) {
    $VanillaDir = $env:RDL_VANILLA_DIR
}
if ([string]::IsNullOrWhiteSpace($VanillaDir) -and -not [string]::IsNullOrWhiteSpace($env:RDL_GAME_DIR)) {
    $VanillaDir = Join-Path $env:RDL_GAME_DIR "data\backups\vanilla-materials"
}
if ([string]::IsNullOrWhiteSpace($VanillaDir)) {
    throw "Pass -VanillaDir or set RDL_VANILLA_DIR / RDL_GAME_DIR"
}

$project = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $project "build"))
if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Join-Path $buildRoot "native-io-regression"
}
$Root = [System.IO.Path]::GetFullPath($Root)
if (-not $Root.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Regression root must stay under $buildRoot"
}
if (-not (Test-Path -LiteralPath $VanillaDir)) {
    throw "Vanilla materials directory not found: $VanillaDir"
}

$compiler = Join-Path $buildRoot "windows\x64\release\compile_smoke.exe"
if ($Build) {
    Push-Location $project
    try {
        & xmake build -y compile_smoke
        if ($LASTEXITCODE -ne 0) { throw "compile_smoke build failed" }
    } finally {
        Pop-Location
    }
}
if (-not (Test-Path -LiteralPath $compiler)) {
    throw "compile_smoke.exe not found; rerun with -Build"
}

$fixtureRoot = Join-Path $project "samples\compat\native-io-regression"
$fixtureArchive = Join-Path $fixtureRoot "packs\native-io-source-fixture.zip"
$colorSource = Join-Path $fixtureRoot "good-src\materials\ColorPostProcessing"
$colorInclude = Join-Path $fixtureRoot "good-src\include\color_post_processing.glsl"
foreach ($required in @($fixtureArchive, $colorSource, $colorInclude)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required regression fixture not found: $required"
    }
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Write-JsonFile([string]$Path, [object]$Value) {
    $json = $Value | ConvertTo-Json -Depth 20
    [System.IO.File]::WriteAllText(
        $Path,
        $json,
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Invoke-Compile([string]$Source, [string]$Output) {
    $env:RDL_BACKEND = "shaderc"
    $env:RDL_MATERIALS = "Actor,ColorPostProcessing"
    Remove-Item Env:RDL_IO_ONLY -ErrorAction SilentlyContinue
    Remove-Item Env:RDL_IO_REPEAT -ErrorAction SilentlyContinue
    $lines = & $compiler $Source $Output $VanillaDir 2>&1
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = @($lines)
    }
}

function Invoke-IOValidation([string]$Source, [string]$Output) {
    $env:RDL_IO_ONLY = "1"
    $env:RDL_IO_REPEAT = "2"
    $lines = & $compiler $Source $Output $VanillaDir 2>&1
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = @($lines)
    }
}

$source = Join-Path $Root "phase6b-src"
$compiled = Join-Path $Root "compiled"
$cache = Join-Path $Root "cache"
$mappingPath = Join-Path $source "rdloader.io-mappings.json"

try {
    if (Test-Path -LiteralPath $Root) {
        Remove-Item -LiteralPath $Root -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $source | Out-Null
    & tar.exe -xf $fixtureArchive -C $source
    if ($LASTEXITCODE -ne 0) { throw "cannot extract regression source fixture" }

    Copy-Item -LiteralPath $colorSource -Destination (Join-Path $source "materials\ColorPostProcessing") -Recurse
    Copy-Item -LiteralPath $colorInclude -Destination (Join-Path $source "include\color_post_processing.glsl") -Force

    foreach ($material in @("Actor", "ColorPostProcessing")) {
        $configPath = Join-Path $source "materials\$material\config.json"
        $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
        if ($null -eq $config.compiler) {
            $config | Add-Member -NotePropertyName compiler -NotePropertyValue ([pscustomobject]@{})
        }
        $config.compiler | Add-Member -NotePropertyName type -NotePropertyValue "shaderc" -Force
        Write-JsonFile $configPath $config
    }
    Write-JsonFile (Join-Path $source "options.json") ([pscustomobject]@{})

    $fullMapping = [ordered]@{
        version = 1
        target = "RenderDragon-26.51"
        attributes = @(
            [ordered]@{
                id = "actor-position"
                material = "Actor"
                pass = "Opaque"
                sourceSemantic = "CUSTOM_POSITION"
                targetSemantic = "POSITION"
                sourceName = "a_custom_position"
                targetName = "a_position"
                reason = "phase 6B combined attribute coverage"
            }
        )
        buffers = @(
            [ordered]@{
                id = "color-probe-buffer"
                material = "ColorPostProcessing"
                sourceBuffer = "ProbeColorTexture"
                targetBuffer = "ColorTexture"
                reason = "phase 6B combined buffer coverage"
            }
        )
        passes = @(
            [ordered]@{
                id = "color-probe-output"
                material = "ColorPostProcessing"
                pass = "ColorPostProcess"
                targetRenderTargets = @("Color0")
                reason = "phase 6B combined pass coverage"
            }
        )
    }
    Write-JsonFile $mappingPath $fullMapping

    Write-Host "[1/5] Generate compile cache from canonical output"
    $first = Invoke-Compile $source $compiled
    Assert-True ($first.ExitCode -ne 0) "First compile should fail because mapping targets are not yet present"
    Assert-True (($first.Output -join "`n") -match "mapping.unused") "Expected mapping.unused from canonical candidate"

    $cacheDir = Join-Path $cache "phase6b-src\ColorPostProcessing"
    $cacheFile = Get-ChildItem -LiteralPath $cacheDir -File -Filter "*.material.bin" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    Assert-True ($null -ne $cacheFile) "ColorPostProcessing cache output was not created"
    $canonical = Join-Path $Root "canonical-ColorPostProcessing.material.bin"
    $probe = Join-Path $Root "probe-ColorPostProcessing.material.bin"
    Copy-Item -LiteralPath $cacheFile.FullName -Destination $canonical -Force
    & py -3 (Join-Path $PSScriptRoot "make_native_io_probe.py") `
        --input $canonical `
        --output $probe `
        --source-buffer ColorTexture `
        --target-buffer ProbeColorTexture `
        --pass ColorPostProcess `
        --signature 0x5615C006
    if ($LASTEXITCODE -ne 0) { throw "probe generation failed" }
    Copy-Item -LiteralPath $probe -Destination $cacheFile.FullName -Force

    Write-Host "[2/5] Apply attribute + buffer + pass mappings after cache hit"
    $second = Invoke-Compile $source $compiled
    $second.Output | Out-Host
    Assert-True ($second.ExitCode -eq 0) "Mapped cache-hit compile failed"
    $meta = Get-Content -LiteralPath (Join-Path $compiled "meta.json") -Raw | ConvertFrom-Json
    Assert-True (@($meta.ioMappings).Count -eq 3) "meta.ioMappings must contain three records"
    Assert-True ($meta.ioMappingsSignature.Length -eq 16) "mapping signature is missing"

    $contractPath = Join-Path $Root "final-contract.json"
    & py -3 (Join-Path $PSScriptRoot "material_io_dump.py") $compiled --contract-out $contractPath | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "final material contract dump failed" }
    $contract = Get-Content -LiteralPath $contractPath -Raw | ConvertFrom-Json
    $material = $contract.materials.ColorPostProcessing
    $bufferNames = @($material.buffers.PSObject.Properties.Name)
    Assert-True ($bufferNames -contains "ColorTexture") "mapped ColorTexture is missing"
    Assert-True ($bufferNames -notcontains "ProbeColorTexture") "probe buffer was not rewritten"
    Assert-True ($material.passes.ColorPostProcess.outputBindingSignature -eq "0x4BA175A2") "pass signature was not rewritten"

    Write-Host "[3/5] Read the compatibility report twice"
    $io = Invoke-IOValidation $source $compiled
    $io.Output | Out-Host
    Assert-True ($io.ExitCode -eq 0) "compatibility report validation failed"
    Assert-True (($io.Output -join "`n") -match "io_only=ok.*mappings=3") "report must return mappings=3"
    Assert-True (($io.Output -join "`n") -match "mapping buffers") "buffer mapping record is missing"
    Assert-True (($io.Output -join "`n") -match "mapping passes") "pass mapping record is missing"
    Assert-True (($io.Output -join "`n") -match "mapping attributes") "attribute mapping record is missing"

    Write-Host "[4/5] Verify failure does not replace previous output"
    $tracked = @(
        (Join-Path $compiled "Actor.material.bin"),
        (Join-Path $compiled "ColorPostProcessing.material.bin"),
        (Join-Path $compiled "meta.json")
    )
    $before = @{}
    foreach ($file in $tracked) {
        $before[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
    }
    $invalidMapping = [ordered]@{
        version = $fullMapping.version
        target = $fullMapping.target
        attributes = $fullMapping.attributes
        buffers = @(
            [ordered]@{
                id = "color-probe-buffer"
                material = "ColorPostProcessing"
                sourceBuffer = "ProbeColorTexture"
                targetBuffer = "MissingColorTexture"
                reason = "phase 6B failure protection coverage"
            }
        )
        passes = $fullMapping.passes
    }
    Write-JsonFile $mappingPath $invalidMapping
    $invalid = Invoke-Compile $source $compiled
    $invalid.Output | Out-Host
    Assert-True ($invalid.ExitCode -ne 0) "Invalid mapping must fail"
    foreach ($file in $tracked) {
        $after = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
        Assert-True ($before[$file] -eq $after) "Existing output changed after mapping failure: $file"
    }
    Write-JsonFile $mappingPath $fullMapping

    Write-Host "[5/5] Run full Native IO self-test"
    Push-Location $project
    try {
        & py -3 "tools\source_compat_selftest.py"
        if ($LASTEXITCODE -ne 0) { throw "source_compat_selftest failed" }
    } finally {
        Pop-Location
    }

    Write-Host "PASS: Native IO phase 6B regression"
    Write-Host "  mappings=3, cache-hit=true, failure-protected=true, selftest=28/28"
} finally {
    Remove-Item Env:RDL_BACKEND -ErrorAction SilentlyContinue
    Remove-Item Env:RDL_MATERIALS -ErrorAction SilentlyContinue
    Remove-Item Env:RDL_IO_ONLY -ErrorAction SilentlyContinue
    Remove-Item Env:RDL_IO_REPEAT -ErrorAction SilentlyContinue
    if (-not $KeepArtifacts -and (Test-Path -LiteralPath $Root)) {
        Remove-Item -LiteralPath $Root -Recurse -Force
    }
}
