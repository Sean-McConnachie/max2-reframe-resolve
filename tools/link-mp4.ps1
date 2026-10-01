# Only needed if DaVinci Resolve refuses to import .360 files.
# Creates a hard link "NAME.360.mp4" next to every .360 file in a folder (recursively). A hard link is the same
# file under a second name: it uses no extra disk space, and the Max2 Reframe plugin still sees both lens streams.
#   .\link-mp4.ps1 "D:\GoPro\Trip"
param([Parameter(Mandatory)][string]$Folder)
$n = 0
Get-ChildItem -LiteralPath $Folder -Filter *.360 -Recurse -File | ForEach-Object {
    $link = "$($_.FullName).mp4"
    if (-not (Test-Path -LiteralPath $link)) {
        New-Item -ItemType HardLink -Path $link -Target $_.FullName | Out-Null
        $n++
    }
}
Write-Host "Created $n link(s)."
