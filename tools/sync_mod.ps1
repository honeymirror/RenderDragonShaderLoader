# Sync build output into the game's mods folder.
# `xmake -y` calls this automatically; you can also run it by hand.
# The game locks its DLL while running, so close the game first for a full sync.
#
# Usage:
#   powershell -File tools\sync_mod.ps1
#   powershell -File tools\sync_mod.ps1 -Dest "E:\...\mods\RenderDragonShaderLoader"
#   powershell -File tools\sync_mod.ps1 -List        # list available snapshots
#   powershell -File tools\sync_mod.ps1 -Rollback     # restore the newest snapshot
#
# Rollback: before every sync, the files that are about to be overwritten are
# snapshotted into <Dest>\_backup\<timestamp>\, keeping the newest KeepBackups
# copies. Use -Rollback to go back one step. Snapshots only cover build output
# (dll/pdb/manifest/lang/toolchain/...), never data\ (that is player data).
#
# NOTE: keep this file ASCII-only. xmake runs it through Windows PowerShell 5.1,
# which reads BOM-less files as ANSI and chokes on non-ASCII bytes.

param(
    [string]$Source = (Join-Path $PSScriptRoot "..\bin\RenderDragonShaderLoader"),
    [string]$Dest   = "",
    [switch]$Rollback,
    [switch]$List,
    [string]$RollbackTo = "",
    [int]$KeepBackups = 8
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Dest)) {
    throw "Dest is required. Pass -Dest, or configure xmake with --mods_dir=<game mods directory>."
}

function Get-BackupRoot([string]$DestPath) { return (Join-Path $DestPath "_backup") }

# config\config.json is the "first install" default config. Once installed it belongs to the
# player, so later syncs / rollbacks must not touch it (otherwise their uiBackdrop and hotkey
# would be reset on every build).
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 decodes BOM-less files as ANSI,
# so non-ASCII comments would corrupt the script.
$KeepUserFiles = @("config\config.json")

function Test-KeepUserFile([string]$RelPath) { return $KeepUserFiles -contains $RelPath }

function Get-Backups([string]$DestPath) {
    $root = Get-BackupRoot $DestPath
    if (-not (Test-Path $root)) { return @() }
    return @(Get-ChildItem -Directory $root | Sort-Object Name -Descending)
}

# Snapshot only the files this run will overwrite (never touches data\).
function Save-Backup([string]$SourcePath, [string]$DestPath) {
    $root  = Get-BackupRoot $DestPath
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $dir   = Join-Path $root $stamp
    $count = 0

    foreach ($item in Get-ChildItem -Recurse -File $SourcePath) {
        $rel = $item.FullName.Substring($SourcePath.Length).TrimStart('\')
        if (Test-KeepUserFile $rel) { continue } # not overwritten, so no snapshot needed
        $cur = Join-Path $DestPath $rel
        if (-not (Test-Path $cur)) { continue }
        $snap = Join-Path $dir $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $snap -Parent) | Out-Null
        Copy-Item -LiteralPath $cur -Destination $snap -Force
        ++$count
    }

    if ($count -eq 0) { return $null }
    Write-Host ("backup: {0} file(s) -> {1}" -f $count, $dir)

    $all = Get-Backups $DestPath
    while ($all.Count -gt $KeepBackups) {
        $old = $all[$all.Count - 1]
        Remove-Item -LiteralPath $old.FullName -Recurse -Force
        Write-Host ("backup: pruned {0}" -f $old.Name)
        $all = Get-Backups $DestPath
    }
    return $dir
}

function Restore-Backup([string]$DestPath, [string]$BackupDir) {
    Write-Host "rollback: $BackupDir -> $DestPath"
    $restored = 0
    foreach ($item in Get-ChildItem -Recurse -File $BackupDir) {
        $rel = $item.FullName.Substring($BackupDir.Length).TrimStart('\')
        $dst = Join-Path $DestPath $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null
        Copy-Item -LiteralPath $item.FullName -Destination $dst -Force
        Write-Host ("  restored {0}" -f $rel)
        ++$restored
    }
    Write-Host ("rollback done: {0} file(s). Restart the game to take effect." -f $restored)
}

$Source = (Resolve-Path $Source).Path
if (-not (Test-Path $Dest)) {
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
}
$Dest = (Resolve-Path $Dest).Path

if ($Rollback -or $List -or $RollbackTo -ne "") {
    $all = Get-Backups $Dest
    if ($all.Count -eq 0) {
        Write-Warning "no backup found under $(Get-BackupRoot $Dest)"
        exit 0
    }
    if ($List) {
        Write-Host "backups (newest first):"
        foreach ($b in $all) { Write-Host ("  {0}" -f $b.Name) }
        exit 0
    }
    $chosen = $all[0]
    if ($RollbackTo -ne "") {
        $match = $all | Where-Object { $_.Name -eq $RollbackTo } | Select-Object -First 1
        if ($null -eq $match) {
            Write-Warning ("no backup named '{0}'; use -List to see them" -f $RollbackTo)
            exit 1
        }
        $chosen = $match
    }
    Restore-Backup $Dest $chosen.FullName
    exit 0
}

Write-Host "sync: $Source -> $Dest"

# A failed snapshot must not abort the sync itself: warn and continue (no rollback point this run).
try {
    Save-Backup $Source $Dest | Out-Null
} catch {
    Write-Warning ("backup failed (continuing without a rollback point): {0}" -f $_.Exception.Message)
}

$locked = @()
foreach ($item in Get-ChildItem -Recurse -File $Source) {
    $rel = $item.FullName.Substring($Source.Length).TrimStart('\')
    $dst = Join-Path $Dest $rel
    if ((Test-KeepUserFile $rel) -and (Test-Path $dst)) {
        Write-Host ("  keep {0} (existing user config)" -f $rel)
        continue
    }
    $dir = Split-Path $dst -Parent
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    try {
        Copy-Item -LiteralPath $item.FullName -Destination $dst -Force
        Write-Host ("  ok   {0}" -f $rel)
    } catch {
        $locked += $rel
        Write-Warning ("  skip {0} (close the game first): {1}" -f $rel, $_.Exception.Message)
    }
}

if ($locked.Count -gt 0) {
    Write-Warning ("sync failed for: {0} (the game is probably running)" -f ($locked -join ", "))
    Write-Host "sync incomplete."
    exit 0
}
Write-Host "sync done."
