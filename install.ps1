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

# Run through "irm | iex", this script shares the console's scope: what it changes there is put back at the end.
$seekPrefs = @($ErrorActionPreference, $ProgressPreference)
try {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'  # Windows PowerShell's progress bar slows downloads a lot
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

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
        # what a client writes to stderr isn't the installer failing: Windows PowerShell would stop on it otherwise
        $ErrorActionPreference = 'Continue'
        $clients = @(
            @{ Name = 'Claude Code'; Command = 'claude'; Remove = @('mcp', 'remove', 'seek', '--scope', 'user'); Add = @('mcp', 'add', 'seek', '--scope', 'user', '--', $exe) },
            @{ Name = 'Codex'; Command = 'codex'; Remove = @('mcp', 'remove', 'seek'); Add = @('mcp', 'add', 'seek', '--', $exe) }
        )
        foreach ($c in $clients) {
            # on PATH, or where its installers put it: Claude Code's own installer doesn't add its folder to PATH everywhere
            $path = (Get-Command $c.Command -ErrorAction SilentlyContinue).Source
            if (-not $path) {
                $path = @("$env:USERPROFILE\.local\bin\$($c.Command).exe", "$env:APPDATA\npm\$($c.Command).cmd") | Where-Object { Test-Path $_ } | Select-Object -First 1
            }
            if (-not $path) {
                Write-Host "$($c.Name): not found on this PC."
                continue
            }
            & $path @($c.Remove) *> $null
            $out = & $path @($c.Add) 2>&1 | Out-String
            if ($LASTEXITCODE -eq 0) {
                Write-Host "Added to $($c.Name): its new sessions have the find, grep and recent tools." -ForegroundColor Green
            } else {
                Write-Host "$($c.Name): couldn't add Seek ($($out.Trim())). By hand: $($c.Command) $($c.Add -join ' ')" -ForegroundColor Yellow
            }
        }
    }

    $json = $exe -replace '\\', '\\'
    Write-Host ""
    Write-Host "For any other MCP client (Cursor, Cline, Claude Desktop...), add a stdio server:"
    Write-Host "  { `"mcpServers`": { `"seek`": { `"command`": `"$json`" } } }"
    Write-Host ""
    Write-Host "Docs and benchmarks: https://seek.synrgs.tech"
} finally {
    $ErrorActionPreference, $ProgressPreference = $seekPrefs
}
