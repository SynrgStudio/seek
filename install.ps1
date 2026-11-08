#Requires -Version 5.1
<#
.SYNOPSIS
    Installs Seek: fast file search for AI agents on Windows, as an MCP server.
.DESCRIPTION
    Downloads seek.exe from the latest GitHub release into %LOCALAPPDATA%\Programs\Seek, checks it against the
    release's SHA-256, adds that folder to the user's PATH, and adds Seek to Claude Code and Codex when they are
    installed. Running it again updates Seek.

        irm https://raw.githubusercontent.com/SynrgStudio/seek/main/install.ps1 | iex

    To pass options, save it first:  .\install.ps1 -Version 0.1.0 -NoRegister
.PARAMETER Version
    A release to install, like 0.1.0. Default: the latest.
.PARAMETER NoRegister
    Only install; don't add Seek to the MCP clients found.
.PARAMETER NoPath
    Don't add Seek's folder to the user's PATH.
#>
param(
    [string]$Version = $env:SEEK_INSTALL_VERSION,
    [switch]$NoRegister,
    [switch]$NoPath
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

    # The background process lets go of the old file, but each open agent session runs a seek.exe of its own, and
    # Windows doesn't let a running program be replaced. It can be renamed, though: the old one is moved aside and
    # deleted once nothing runs it, now or on the next install. Those sessions start the background process again
    # from seek.exe, so they get the new one from their next call.
    if (Test-Path $exe) { & $exe --stop | Out-Null }
    Get-ChildItem -Path $dir -Filter 'seek.exe.old-*' | Remove-Item -Force -ErrorAction SilentlyContinue
    $old = $null
    if (Test-Path $exe) {
        $old = "$exe.old-$(Get-Date -Format 'yyyyMMddHHmmss')"
        Move-Item -Path $exe -Destination $old
    }
    Move-Item -Path $download -Destination $exe
    if ($old) { Remove-Item $old -Force -ErrorAction SilentlyContinue }
    Write-Host "Installed $(& $exe --version) in $exe" -ForegroundColor Green
    if ($old -and (Test-Path $old)) { Write-Host 'Agent sessions already open use it from their next call; restart them to see what is new in the tools.' }

    # On the user's PATH, so that "seek --status" and "seek --version" work in a console. The value is read and written
    # as the registry keeps it, with its %VARIABLES% unexpanded, and the shell is told, so new consoles have it.
    if (-not $NoPath) {
        $envKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
        try {
            $userPath = [string]$envKey.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $entries = @($userPath -split ';' | Where-Object { $_ })
            $there = $entries | Where-Object { [Environment]::ExpandEnvironmentVariables($_).TrimEnd('\') -ieq $dir }
            if (-not $there) {
                $kind = if ($envKey.GetValueNames() -contains 'Path') { $envKey.GetValueKind('Path') } else { [Microsoft.Win32.RegistryValueKind]::ExpandString }
                $envKey.SetValue('Path', (@($entries) + $dir) -join ';', $kind)
                if (-not ('SeekInstall.Win' -as [type])) {
                    Add-Type -Namespace SeekInstall -Name Win -MemberDefinition '[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, UIntPtr wParam, string lParam, uint flags, uint timeout, out UIntPtr result);'
                }
                $r = [UIntPtr]::Zero
                [SeekInstall.Win]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$r) | Out-Null  # WM_SETTINGCHANGE
                Write-Host "Added $dir to your PATH: seek --status works in consoles opened from now on."
            }
        } finally {
            $envKey.Close()
        }
        if (-not ($env:Path -split ';' | Where-Object { $_.TrimEnd('\') -ieq $dir })) { $env:Path = "$env:Path;$dir" }  # and in this one
    }

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
