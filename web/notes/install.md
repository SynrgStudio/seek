---
title: Installing, and setting it up for agents
status: stable
updated: 2026-11-11
---

One file, no dependencies, no administrator rights. In PowerShell (the command is `install` in `[project]`):

```
irm https://raw.githubusercontent.com/SynrgStudio/seek/main/install.ps1 | iex
```

The installer:

- downloads `seek.exe` from the latest release into `%LOCALAPPDATA%\Programs\Seek`, and checks it against the release's SHA-256;
- stops the running Seek first, wherever it runs from;
- puts the folder on the user's PATH, so `seek --status` and `seek --version` work in a console;
- adds Seek to Claude Code and Codex when they are installed.

Running it again updates Seek, with agent sessions open too: they get the new version from their next call.

**By hand:**

```
# Claude Code
claude mcp add seek --scope user -- "%LOCALAPPDATA%\Programs\Seek\seek.exe"

# Codex
codex mcp add seek -- "%LOCALAPPDATA%\Programs\Seek\seek.exe"
```

Any other MCP client (Cursor, Cline, Claude Desktop) needs a stdio server whose command is the full path to `seek.exe`, with no arguments:

```json
{ "mcpServers": { "seek": { "command": "C:\\Users\\me\\AppData\\Local\\Programs\\Seek\\seek.exe" } } }
```

A line in a project's `CLAUDE.md` or `AGENTS.md` helps the agent reach for it:

```
To find files by name anywhere on this PC, or search text in a folder, use the seek tools (find, grep, recent, sizes) instead of shell commands.
```

**Installed before 0.3.3, or by hand?** The README has a PowerShell snippet that puts Seek's folder on the PATH. It leaves the rest of the PATH as it is.
