# Seek

Fast file search for AI agents on Windows, as an [MCP](https://modelcontextprotocol.io) server. It lives next to [Jolt](../README.md), shares its fuzzy matching and its drive index, and runs on its own.

An agent's cost when looking for files is rarely the search itself. It is the round trips, the tokens spent reading long lists, and guessing what the user means by "the file I was working on". Seek is built around those three:

- **The whole PC, always warm.** Names on every fixed drive come from an index, so finding a file anywhere takes a fraction of a second instead of minutes of `Get-ChildItem -Recurse`. One background process holds the index for every agent at once.
- **What the user is doing.** Seek watches the drives while it runs and remembers what was created and changed, between runs too. It also knows which folders the user opens in Jolt. Results the user touched lately rank higher, and `recent` answers "what was I working on".
- **Short answers.** Each result says why it is there ("exact name, changed 2 h ago"). `summary` gives counts by area instead of thousands of paths. Long lists are cut and say how to see more.

## Measured

On the author's PC: a Ryzen 7 5800X with 32 GB, two NVMe drives and a hard disk, 4.9 million files and folders. Seek and [fff](https://github.com/dmtrKovalenko/fff) ran as MCP servers, already warm. [Everything](https://www.voidtools.com) was running. The command-line tools were started for each search, the way an agent's shell starts them. Times are medians, and tokens are characters divided by four. `python -I seek\bench.py` measures the same on any PC.

**Names, on the whole PC**

| task | Seek | Everything | Windows Search | fd, ripgrep | an agent's shell |
|---|---|---|---|---|---|
| find `fuzzy.inl`, place unknown | 0.05 s | 0.09 s | 0.22 s, not found: that folder isn't indexed | 4.6 s and 6.1 s with the drives' folders in memory; fd took over 15 minutes right after the drives were last read | `Get-ChildItem -Recurse`: 98 s; `dir /s`: 85 s; both over the user's profile only |
| the same with a typo, `fuzy.inl` | 0.05 s, found | not found | | not found | not found |
| every `.blend`: how many, where | 0.05 s, 378 tokens: 8,200, by folder | 0.07 s for the count; the list is 196,000 tokens | 0.24 s, 9 of the 8,200 | 5.0 s and 7.6 s; the list is 194,000 tokens | `dir /s`: 567 s |
| what changed lately | 0.01 s: the user's files and the folders they opened | 0.10 s: the newest files, which are logs, caches and browser data | | | |

**Text, in folders**

| task | Seek | fff | ripgrep, what Claude Code's Grep runs | git grep |
|---|---|---|---|---|
| `RecTake` in Jolt's repository, 64 files | 0.002 s | 0.001 s | 0.08 s | 0.03 s |
| `CreateFileW` in every project, 33,000 files | 0.33 s | 0.035 s | 0.69 s | |

**Starting, and memory**

| | Seek | Everything | fff |
|---|---|---|---|
| ready | 0.3 to 1 s, for every drive, from Jolt's index | always running, as a service | 0.1 s for Jolt's repository, 0.25 s for every project |
| memory of its own | 22 MB, plus its index files, 274 MB, which Windows reads in and drops as needed | 489 MB | 117 MB for the repository, 233 MB for every project |

What the others do better:
- **Everything** finds names as fast. It needs its service or administrator rights. It doesn't forgive typos and doesn't rank, and it answers with the whole list.
- **fff** searches text inside one folder ten times faster than Seek, because it keeps the contents of every file in memory. Each server covers one folder, and it refuses the user's home folder and the drives' roots.
- **ripgrep** needs nothing running and is on every developer's PC. Seek reads the files on each search the way ripgrep does, and was twice as fast on these folders.

Measured by others:
- [fff's own benchmark](https://github.com/dmtrKovalenko/fff#readme) timed agents doing whole tasks. In a big repository a task took 24.7 s with shell search and 14.3 s with fff, while the tokens barely moved, 85,400 against 82,500. In a held-out run, one model was slower with fff: 9.0 s against 8.1 s. Most of an agent's time is the model, not the search.
- On a 500,000-file Chromium checkout, fff's README reports 3 to 9 s for each ripgrep run, against under 10 ms for each fff query.
- [Everything's FAQ](https://www.voidtools.com/faq/): a million files take about a minute to index and use about 100 MB of memory.
- [ripgrep's README](https://github.com/BurntSushi/ripgrep#quick-examples-comparing-tools), on the Linux kernel's source: ripgrep 0.08 s, git grep 0.27 s, The Silver Searcher 0.44 s, ugrep 0.64 s, ack 2.9 s.

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

`python -I seek\bench.py --ext blend` times Seek against whatever it finds of Everything's `es.exe`, ripgrep, fff's MCP server and Windows Search, and with `--walkers`, fd and ripgrep over every drive. The tools are taken from PATH or from the variables at the top of the script.

## Limits

- Windows only, fixed drives. Other drives and network shares can still be searched by giving their folder as `path`; they are walked instead of looked up.
- `recent` only knows what changed while Seek was running, plus the dates on disk when given a `path`.
- `regex` uses the standard library's ECMAScript engine: correct but slower than literal search, and it reads only the first 4,096 characters of very long lines.
