# Turns a text file into a C string literal (one quoted line per source line), for #include in C++.
param([string]$In, [string]$Out)
$lines = Get-Content -LiteralPath $In
$body = foreach ($l in $lines) { '"' + ($l -replace '\\', '\\' -replace '"', '\"') + '\n"' }
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Set-Content -LiteralPath $Out -Value $body -Encoding ASCII
