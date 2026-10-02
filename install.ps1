# Installs the Max2 Reframe OFX plugin.
#  - The plugin itself goes to %LOCALAPPDATA%\Max2Reframe\core\Max2ReframeCore.dll (no admin needed, so updates
#    are just this copy).
#  - A tiny loader goes into Resolve's standard plugin folder,
#    C:\Program Files\Common Files\OFX\Plugins\Max2Reframe.ofx.bundle, and forwards to the core. Installing or
#    changing the loader needs admin (one UAC prompt); it rarely changes.
#  - The "Import GoPro 360" script goes into Resolve's Scripts menu (Workspace > Scripts), and its PowerShell
#    helper into %APPDATA%\Blackmagic Design\DaVinci Resolve\Support\Max2Reframe.
# Restart Resolve afterwards. Uninstall: .\install.ps1 -Uninstall
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$base = Join-Path $env:LOCALAPPDATA 'Max2Reframe'
$coreDir = Join-Path $base 'core'
$core = Join-Path $coreDir 'Max2ReframeCore.dll'
$bundle = 'C:\Program Files\Common Files\OFX\Plugins\Max2Reframe.ofx.bundle'
$stub = Join-Path $bundle 'Contents\Win64\Max2Reframe.ofx'
$scriptDir = Join-Path $env:APPDATA 'Blackmagic Design\DaVinci Resolve\Support\Fusion\Scripts\Utility'
$importScript = 'Import GoPro 360.lua'
$helperName = 'Import GoPro 360.ps1'
$helperDir = Join-Path $env:APPDATA 'Blackmagic Design\DaVinci Resolve\Support\Max2Reframe'

# Files come from a release zip (next to this script) or from a source checkout (build\ and tools\resolve\).
if (Test-Path (Join-Path $PSScriptRoot 'Max2ReframeCore.dll')) {
    $srcCore = Join-Path $PSScriptRoot 'Max2ReframeCore.dll'
    $builtStub = Join-Path $PSScriptRoot 'Max2Reframe.ofx.bundle\Contents\Win64\Max2Reframe.ofx'
    $srcScript = Join-Path $PSScriptRoot $importScript
    $srcHelper = Join-Path $PSScriptRoot $helperName
} else {
    $srcCore = Join-Path $PSScriptRoot 'build\Max2ReframeCore.dll'
    $builtStub = Join-Path $PSScriptRoot 'build\Max2Reframe.ofx.bundle\Contents\Win64\Max2Reframe.ofx'
    $srcScript = Join-Path $PSScriptRoot "tools\resolve\$importScript"
}

# Resolve keeps its plugin cache in memory and rewrites it on exit, so changes made while it runs are lost.
if (Get-Process Resolve -ErrorAction SilentlyContinue) {
    throw "Close DaVinci Resolve, then run the installer again."
}

function Remove-Locked($path) {
    # A running Resolve keeps DLLs locked; a locked file can still be renamed out of the way.
    if (-not (Test-Path $path)) { return }
    try { Remove-Item $path -Force }
    catch { Rename-Item $path ((Split-Path $path -Leaf) + '.old' + (Get-Date -Format yyyyMMddHHmmss)) }
}

function Invoke-Elevated($cmdLine) {
    $p = Start-Process cmd.exe -ArgumentList "/c $cmdLine" -Verb RunAs -Wait -PassThru -WindowStyle Hidden
    return $p.ExitCode -eq 0
}

# Clean up earlier install methods: the OFX_PLUGIN_PATH entry and the old full bundle in LOCALAPPDATA.
$oldRoot = Join-Path $base 'OFX'
$cur = [Environment]::GetEnvironmentVariable('OFX_PLUGIN_PATH', 'User')
if ($cur) {
    $rest = @($cur -split ';' | Where-Object { $_ -and $_ -ne $oldRoot })
    [Environment]::SetEnvironmentVariable('OFX_PLUGIN_PATH', $(if ($rest) { $rest -join ';' } else { $null }), 'User')
}
if (Test-Path $oldRoot) { try { Remove-Item $oldRoot -Recurse -Force } catch {} }

$isJunction = (Test-Path $bundle) -and ((Get-Item $bundle -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)

if ($Uninstall) {
    Remove-Locked $core
    Remove-Item (Join-Path $scriptDir $importScript), (Join-Path $scriptDir 'Import GoPro 360.py') -ErrorAction SilentlyContinue
    Remove-Item $helperDir -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $bundle) { Invoke-Elevated "rmdir /s /q `"$bundle`"" | Out-Null }
    Write-Host "Uninstalled. Restart DaVinci Resolve."
    return
}

New-Item -ItemType Directory -Force $coreDir | Out-Null
Get-ChildItem $coreDir -Filter '*.old*' | ForEach-Object { try { Remove-Item $_.FullName -Force } catch {} }
Remove-Locked $core
Copy-Item $srcCore $core

function Get-LoaderVersion($path) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    $m = [regex]::Match($text, 'MAX2REFRAME_LOADER_VERSION=(\d+)')
    if ($m.Success) { [int]$m.Groups[1].Value } else { 1 }
}
$needStub = $isJunction -or -not (Test-Path $stub) -or ((Get-LoaderVersion $builtStub) -gt (Get-LoaderVersion $stub))
if ($needStub) {
    Write-Host "Installing the loader into $bundle (needs admin: approve the UAC prompt)"
    $cmd = ''
    if ($isJunction) { $cmd += "rmdir `"$bundle`" & " }
    $cmd += "mkdir `"$bundle\Contents\Win64`" 2>nul & copy /y `"$builtStub`" `"$stub`""
    Invoke-Elevated $cmd | Out-Null
    if (-not (Test-Path $stub) -or (Get-LoaderVersion $stub) -lt (Get-LoaderVersion $builtStub)) {
        throw "Could not install the loader (is Resolve still running?)"
    }
}

New-Item -ItemType Directory -Force $scriptDir | Out-Null
Remove-Item (Join-Path $scriptDir 'Import GoPro 360.py') -ErrorAction SilentlyContinue  # replaced by the Lua version
Copy-Item $srcScript (Join-Path $scriptDir $importScript) -Force
# Resolve's script Lua cannot show a folder dialog or make links, so a PowerShell helper does both.
New-Item -ItemType Directory -Force $helperDir | Out-Null
Copy-Item $srcHelper (Join-Path $helperDir $helperName) -Force

# Resolve remembers plugins that failed to load and will not retry them; drop our entries so it rescans.
$cache = Join-Path $env:APPDATA 'Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml'
if (Test-Path $cache) {
    $x = Get-Content $cache -Raw
    $x2 = [regex]::Replace($x, '(?s)<bundle>\s*<binary bundle_path="[^"]*Max2Reframe[^"]*".*?</bundle>\s*', '')
    if ($x2 -ne $x) { Set-Content $cache $x2 -NoNewline -Encoding UTF8 }
}

Write-Host "Installed $core"
Write-Host "Restart DaVinci Resolve to load it."
