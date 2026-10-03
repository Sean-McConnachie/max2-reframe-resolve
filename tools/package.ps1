# Builds a release zip in dist\: Max2Reframe-<version>-win64.zip
param([string]$Version = '1.2.2')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# stderr is merged inside cmd: vcvars prints harmless noise there, which would stop this script
cmd /c "`"$root\build.bat`" 2>&1" | Select-String ' error |fatal|Built'
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$name = "Max2Reframe-$Version-win64"
$stage = Join-Path $root "dist\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\Max2Reframe.ofx.bundle\Contents\Win64" | Out-Null

Copy-Item "$root\build\Max2ReframeCore.dll" $stage
Copy-Item "$root\build\Max2Reframe.ofx.bundle\Contents\Win64\Max2Reframe.ofx" "$stage\Max2Reframe.ofx.bundle\Contents\Win64\"
Copy-Item "$root\build\Max2Prepare.exe", "$root\tools\resolve\Import GoPro 360.lua", "$root\tools\resolve\Import GoPro 360 Folder Tree.lua" $stage
Copy-Item "$root\install.ps1", "$root\tools\Install.bat", "$root\tools\Uninstall.bat", "$root\LICENSE", "$root\THIRD-PARTY-NOTICES.md" $stage
Copy-Item "$root\tools\link-mp4.ps1" $stage

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
# Write entries by hand: on Windows PowerShell 5 both Compress-Archive and ZipFile use backslash entry names,
# which some unzip tools mishandle.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem $stage -Recurse -File) {
        $entry = $name + '/' + $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
Get-Item $zip | Select-Object FullName, Length
