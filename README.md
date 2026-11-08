# Seek

Fast file search for AI agents on Windows, as an [MCP](https://modelcontextprotocol.io) server. It lives next to [Jolt](../README.md), shares its fuzzy matching and its drive index, and runs on its own.

An agent's cost when looking for files is rarely the search itself. It is the round trips, the tokens spent reading long lists, and guessing what the user means by "the file I was working on". Seek is built around those three:

- **The whole PC, always warm.** Names on every fixed drive come from an index, so finding a file anywhere takes a fraction of a second instead of minutes of `Get-ChildItem -Recurse`. One background process holds the index for every agent at once.
- **What the user is doing.** Seek watches the drives while it runs and remembers what was created and changed, between runs too. It also knows which folders the user opens in Jolt. Results the user touched lately rank higher, and `recent` answers "what was I working on".
- **Short answers.** Each result says why it is there ("exact name, changed 2 h ago"). `summary` gives counts by area instead of thousands of paths. Long lists are cut and say how to see more.

## Measured

On the author's PC, with three drives and about ten million names. Seek's background process was already running. Tokens are characters divided by four.

| task | Seek | what an agent uses without it |
|---|---|---|
| find `fuzzy.inl`, location unknown | 0.7 s, every drive, 127 tokens | `Get-ChildItem -Recurse`: 98 s; `dir /s /b`: 85 s; both only over the user profile |
| how many `.blend` files there are, and where | 1.7 s, 378 tokens | `dir /s /b` over three drives: 567 s, 104,157 tokens |
| `RecTake` in Jolt's source | 0.07 s, 197 tokens | `Select-String`: 6.7 s; `findstr /s`: 2.8 s; both about 2.3 million tokens, because they read the build outputs too |
| what changed lately | 0.05 s | no equivalent |

## Tools

| tool | what it does |
|---|---|
| `find` | Files and folders by name, anywhere on the PC or below `path`. Fuzzy and typo-tolerant. Ranks exact names and programs first, then what the user changed lately, what is in folders they use in Jolt, and what is inside `near`; caches and package stores go last. Query syntax: words must appear in the name in order, `.png` or `ext:png,jpg` for the extension, `*` and `?` as wildcards, `!word` to leave names out, `type:file` or `type:folder`, and a fragment with a slash must be in the path. `summary` counts by area, folder, extension and drive. |
| `grep` | Text inside the files of a folder or one file: code and text in UTF-8, UTF-16 or the system code page, and documents through the text filters Windows has (PDF and RTF come with Windows; Word and Excel need Office or the Office filter pack). Follows `.gitignore`, also the ones above the folder up to the repository's root, and skips dependency and cache folders, binaries and files only in the cloud. Literal by default, `regex` for a pattern; case-insensitive unless the pattern has capitals. `glob` picks files (`*.cpp`, `!*.min.js`), `context` adds lines around, `limit` and `offset` page, `summary` counts per file. With no matches in the exact case it retries ignoring case, and with no text matches it suggests file names that look like the pattern. |
| `recent` | What the user created or changed lately and the folders they opened in Jolt, newest first. Machine-wide from what Seek saw change; with `path`, also the dates on disk below that folder. `query` filters names, `since` sets how far back (`30m`, `2h`, `3d`, `2w`). System places are left out unless `include_system`. |

## Setting it up

Build it with `seek\build.bat` (Visual Studio with C++). It produces `seek\build\seek.exe`, which is all there is to install.

**Claude Code**

```
claude mcp add seek --scope user -- "C:\path\to\seek\build\seek.exe"
```

**Codex**

```
codex mcp add seek -- "C:\path\to\seek\build\seek.exe"
```

**Any other MCP client** (Cursor, Cline, Claude Desktop…): add a stdio server whose command is the full path to `seek.exe`, with no arguments:

```json
{ "mcpServers": { "seek": { "command": "C:\\path\\to\\seek\\build\\seek.exe" } } }
```

A line for the project's `CLAUDE.md` or `AGENTS.md` helps the agent reach for it:

```
To find files by name anywhere on this PC, or search text in a folder, use the seek tools (find, grep, recent) instead of shell commands.
```

## How it runs

- `seek.exe` with no arguments is what the agent starts. It speaks MCP over stdin and stdout and hands every tool call to `seek.exe --daemon`, starting it the first time. If the daemon can't be reached, the call runs in the agent's process instead.
- The daemon keeps running for every agent. It accepts connections only from the same user on the same machine, and a daemon from an older build steps aside for a newer one.
- **The index:** with Jolt installed, Seek copies Jolt's index (`%LOCALAPPDATA%\Jolt\index-X.bin`) and picks up a newer one every 10 minutes. It copies instead of reading it in place because Jolt replaces that file when it reindexes. Without Jolt, Seek walks each drive once, which takes minutes on a big one, and saves the result. Either way, what is created after the index was taken comes from the drive watchers.
- **Its files** live in `%LOCALAPPDATA%\Jolt\seek`: its copies of the index, `activity.txt` with what it saw change, and `log.txt` with every call.
- No administrator rights are needed.

## Privacy

Seek only reads. Some places are never listed, searched or remembered:
- SSH, GPG, AWS, Azure, Kubernetes and Docker credentials
- Windows' credential and key stores
- browser profiles: Chrome, Edge, Brave, Thorium, Vivaldi, Opera, Firefox
- password managers: Bitwarden, 1Password, KeePass
- Telegram and Signal data
- the Recycle Bin, `System Volume Information`, and `.kdbx`, `.pfx`, `.p12` and `.ppk` files

More can be added in `%APPDATA%\Jolt\seek.txt`:

```
exclude = C:\Users\me\Private      a folder
exclude = *.key                     files with that extension
exclude = secrets                   any folder or file with that name
log = no                            don't write log.txt
```

`log.txt` records every call: when, which agent, which tool, the arguments, and how long it took. It is there so the user can see what agents looked for.

## Command line

```
seek --cli find "{\"query\":\"report .pdf\"}"     run a tool in this process
seek --call grep "{\"pattern\":\"TODO\",\"path\":\"C:\\\\code\"}"   through the background process
seek --stop                                        stop the background process
```

## Tests

`python -I seek\test.py` builds test files in `%TEMP%\seek-test` and checks the tools, the MCP protocol and the background process. Outside that folder it only reads.

## Limits

- Windows only, fixed drives. Other drives and network shares can still be searched by giving their folder as `path`; they are walked instead of looked up.
- `recent` only knows what changed while Seek was running, plus the dates on disk when given a `path`.
- `regex` uses the standard library's ECMAScript engine: correct but slower than literal search, and it reads only the first 4,096 characters of very long lines.
