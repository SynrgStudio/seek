#Requires -Version 5.1
<#
.SYNOPSIS
    Installs Seek: fast file search for AI agents on Windows, as an MCP server.
.DESCRIPTION
    Downloads seek.exe from the latest GitHub release into %LOCALAPPDATA%\Programs\Seek, checks it against the
    release's SHA-256, and adds it to Claude Code and Codex when they are installed. Running it again updates Seek.

        irm https://raw.githubusercontent.com/SynrgStudio/seek/main/install.ps1 | iex

    To pass options, save it first:  .\install.ps1 -Version 0.1.0 -NoRegister
.PARAMETER Version
    A release to install, like 0.1.0. Default: the latest.
.PARAMETER NoRegister
    Only install; don't add Seek to the MCP clients found.
#>
param(
    [string]$Version = $env:SEEK_INSTALL_VERSION,
    [switch]$NoRegister
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'  # Windows PowerShell's progress bar slows downloads a lot

$repo = 'SynrgStudio/seek'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\Seek'
$exe = Join-Path $dir 'seek.exe'
$base = if ($Version) { "https://github.com/$repo/releases/download/v$($Version.TrimStart('v'))" } else { "https://github.com/$repo/releases/latest/download" }

New-Item -ItemType Directory -Force -Path $dir | Out-Null
$download = "$exe.download"
Write-Host "Downloading seek.exe from $base"
Invoke-WebRequest -Uri "$base/seek.exe" -OutFile $download -UseBasicParsing
$raw = (Invoke-WebRequest -Uri "$base/SHA256SUMS.txt" -UseBasicParsing).Content
$sums = if ($raw -is [byte[]]) { [Text.Encoding]::ASCII.GetString($raw) } else { [string]$raw }
$want = ($sums -split "`n" | Where-Object { $_ -match 'seek\.exe' } | Select-Object -First 1) -replace '\s.*$', ''
$got = (Get-FileHash -Path $download -Algorithm SHA256).Hash.ToLower()
if (-not $want -or $want.Trim().ToLower() -ne $got) {
    Remove-Item $download -Force
    throw "seek.exe doesn't match the release's SHA-256 ($got against $want). Nothing was installed."
}

if (Test-Path $exe) { & $exe --stop | Out-Null }  # the background process lets go of the old file
Move-Item -Force -Path $download -Destination $exe
Write-Host "Installed $(& $exe --version) in $exe" -ForegroundColor Green

if (-not $NoRegister) {
    $added = @()
    if (Get-Command claude -ErrorAction SilentlyContinue) {
        & claude mcp remove seek --scope user *> $null
        & claude mcp add seek --scope user -- $exe *> $null
        if ($LASTEXITCODE -eq 0) { $added += 'Claude Code' }
    }
    if (Get-Command codex -ErrorAction SilentlyContinue) {
        & codex mcp remove seek *> $null
        & codex mcp add seek -- $exe *> $null
        if ($LASTEXITCODE -eq 0) { $added += 'Codex' }
    }
    if ($added) { Write-Host "Added to $($added -join ' and '): new sessions have the find, grep and recent tools." -ForegroundColor Green }
}

$json = $exe -replace '\\', '\\'
Write-Host ""
Write-Host "For any other MCP client (Cursor, Cline, Claude Desktop...), add a stdio server:"
Write-Host "  { `"mcpServers`": { `"seek`": { `"command`": `"$json`" } } }"
Write-Host ""
Write-Host "Docs and benchmarks: https://seek.synrgs.tech"
