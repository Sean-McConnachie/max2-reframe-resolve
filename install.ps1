# Installs the Max2 Reframe OFX plugin for the current user (no admin rights needed).
# Copies the bundle to %LOCALAPPDATA%\Max2Reframe\OFX and adds that folder to the user's OFX_PLUGIN_PATH,
# which DaVinci Resolve scans at startup. Restart Resolve afterwards.
# Uninstall: .\install.ps1 -Uninstall
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$root = Join-Path $env:LOCALAPPDATA 'Max2Reframe\OFX'
$dest = Join-Path $root 'Max2Reframe.ofx.bundle\Contents\Win64'
$target = Join-Path $dest 'Max2Reframe.ofx'

function Remove-PluginFile($path) {
    # A running Resolve keeps the DLL locked; a locked file can still be renamed out of the way.
    if (-not (Test-Path $path)) { return }
    try { Remove-Item $path -Force }
    catch { Rename-Item $path ("Max2Reframe.ofx.old" + (Get-Date -Format yyyyMMddHHmmss)) }
}

$cur = [Environment]::GetEnvironmentVariable('OFX_PLUGIN_PATH', 'User')
$parts = @($cur -split ';' | Where-Object { $_ })

if ($Uninstall) {
    Remove-PluginFile $target
    $rest = $parts | Where-Object { $_ -ne $root }
    [Environment]::SetEnvironmentVariable('OFX_PLUGIN_PATH', $(if ($rest) { $rest -join ';' } else { $null }), 'User')
    Write-Host "Uninstalled. Restart DaVinci Resolve."
    return
}

New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem $dest -Filter 'Max2Reframe.ofx.old*' | ForEach-Object { try { Remove-Item $_.FullName -Force } catch {} }
Remove-PluginFile $target
Copy-Item (Join-Path $PSScriptRoot 'build\Max2Reframe.ofx.bundle\Contents\Win64\Max2Reframe.ofx') $target

# Resolve remembers plugins that failed to load and will not retry them; drop our entry so it rescans.
$cache = Join-Path $env:APPDATA 'Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml'
if (Test-Path $cache) {
    $x = Get-Content $cache -Raw
    $x2 = [regex]::Replace($x, '(?s)<bundle>\s*<binary bundle_path="[^"]*Max2Reframe[^"]*".*?</bundle>\s*', '')
    if ($x2 -ne $x) { Set-Content $cache $x2 -NoNewline -Encoding UTF8 }
}

if ($parts -notcontains $root) {
    [Environment]::SetEnvironmentVariable('OFX_PLUGIN_PATH', (@($parts) + $root) -join ';', 'User')
    Write-Host "Added $root to OFX_PLUGIN_PATH (user)."
}
Write-Host "Installed $target"
Write-Host "Restart DaVinci Resolve to load it."
