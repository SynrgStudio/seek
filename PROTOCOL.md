# Seek's protocol for programs

Version 1, from Seek 0.5.0.

Any program of the same Windows user can ask Seek's background process directly, without MCP: a search box, an editor, a script. This is what such a program sends and gets back, and what it can count on from later versions of Seek. A client can be written from this page alone.

Seek's MCP tools (`find`, `grep`, `recent`) can also be called through the pipe, but they answer in text written for AI agents, and that text changes from version to version. A program should use the commands below instead.

## Transport

- **The pipe:** `\\.\pipe\seek-<USERNAME>`, where `<USERNAME>` is what `GetUserNameW` gives. Only that user (and SYSTEM) can open it.
- **Messages:** each is a 4-byte little-endian length, then that many bytes of UTF-8 JSON. Both ways, one reply per request.
- **Connections:** one connection can carry many requests, one after another, and should be kept open. Several connections at once are served at once: a program that searches from several threads opens one each.
- **Size:** a request is at most 64 MB. A reply can be large: `find.rows` with `max` 100,000 is several megabytes.

## A request

```json
{"api": 1, "build": "0.0.0 myapp 1.2", "tool": "find.rows", "args": {"query": "report .pdf"}}
```

| field | |
|---|---|
| `tool` | the command |
| `args` | its arguments, an object |
| `api` | the protocol version the program was written for: `1`. Optional; a daemon that speaks only older versions answers `api_too_new` |
| `build` | who asks. A program sends something that starts with `0.0.0`: then it is never taken for a newer Seek (a newer Seek's own `seek.exe` makes the background process step aside for it). Optional for the commands below, but send it |

Unknown fields are ignored.

## A reply

```json
{"api": 1, "version": "0.5.0", "text": "#rows\t2\t1\treal\nF\tC:\\docs\\report.pdf\nF\tC:\\old\\report.pdf\n", "error": false}
```

| field | |
|---|---|
| `api` | the newest protocol version this daemon speaks |
| `version` | Seek's version |
| `text` | the answer: rows, as described for each command |
| `error` | `true` when the command failed; `text` then says why, for a person |
| `code` | when `error` is true: what kind of failure, for the program (see Errors) |

A program ignores fields it doesn't know: later versions may add some.

## Rows

`text` is lines ending in `\n`. The first starts with `#rows` and holds counts; each of the others is one result. Fields are separated by tabs. Paths are full Windows paths, as on disk (not lowercase). No field holds a tab or a line break.

**Rows can grow:** a later version may add fields at the end of the header or of a row, never remove or reorder them. A program reads the fields it knows by position and ignores the rest.

## Commands

### `hello`

What the program is talking to, answered without touching the index: call it once after connecting.

No args. The reply has no `text`, but these fields:

| field | |
|---|---|
| `api`, `apis` | the newest protocol version, and every one this daemon speaks: `[1]` |
| `version`, `build` | Seek's version, and its build (`"0.5.0 Nov 11 2026 03:12:00"`) |
| `exe` | the path of the `seek.exe` running |
| `data` | the folder with its index and its history (`%LOCALAPPDATA%\Seek`) |
| `pid` | its process id |
| `ready` | whether the drive indexes are loaded. Right after it starts, for about a second, they aren't |

### `find.rows`

Files and folders by name, on every fixed drive or below a folder, best first. The same search as the `find` tool: fuzzy and typo-tolerant, ranked by exact names, what the user changed or opened lately, and the folders they use.

| arg | |
|---|---|
| `query` | required. The words of a name (fuzzy, in order); `"as typed"`; `.png` or `ext:png,jpg`; `*` and `?`; `{a,b c}` (one of them); `!word` (not in the name); `!folder/` (not in that folder); `src/name` (a name below a folder called src); `type:file` or `type:folder` |
| `path` | only below this folder |
| `near` | a folder whose surroundings rank first. A boost, not a filter: use `path` to filter |
| `type` | `file`, `folder` or `any` |
| `ignored` | with `path`: also what `.gitignore` leaves out there, and dependency and cache folders (`node_modules`, `.venv`…). Left out by default |
| `max` | how many rows at most: 1 to 100,000, 5,000 by default |

```
#rows	<total>	<exact>	<kind>
<D|F>	<path>
```

- `total`: how many match; `exact` is `1` when that count is exact, `0` when it is an estimate (past `max`, Seek doesn't look at every match).
- `kind`: `real`; `near` when no name has the letters typed and these are close (a typo); `scattered` when no name has them together and these have them spread along the name.
- `D` for a folder, `F` for a file. Sizes and dates aren't given: a program reads them for the rows it shows.

### `grep.rows`

The lines of the files below a folder (or of one file) that have some text. The same search as the `grep` tool: it follows `.gitignore` and skips dependency and cache folders, binaries, and files over 16 MB, and reads PDF and Office documents.

| arg | |
|---|---|
| `pattern` | required. The text, or a regex with `regex: true` |
| `path` | required. The folder or file |
| `regex` | the pattern is a regex (ECMAScript). Default `false` |
| `case` | `sensitive`, `insensitive`, or by default insensitive unless the pattern has capitals |
| `glob` | a pattern or a list of them on the file names (`*.md`); `!` leaves out (`!*.min.js`) |
| `without` | lines with this text are left out |
| `ignored` | also what `.gitignore` leaves out. Default `false` |
| `hidden` | also hidden files and folders. Default `false` |
| `documents` | also PDF and Office documents. Default `true` |
| `limit` | rows at most, 1 to 100,000; 2,000 by default |
| `per_file` | rows from one file at most, 1 to 10,000; 100 by default |
| `time_budget_ms` | how long it may search, 500 to 60,000; 6,000 by default |

```
#rows	<files>	<lines>	<cut>	<how>
<path>	<line>	<column>	<text>
```

- `files` and `lines`: how many files have the text, and how many lines.
- `cut`: `1` when not every matching line is listed (`limit`, `per_file`, or the time budget).
- `how`: `asked`; `nocase` when nothing matched the exact case and these ignore it; `regex` when a literal pattern with regex signs found nothing and these are its matches as a regex.
- `line` and `column` count from 1. `text` is the line without its indentation; a very long one is cut around the match, with `…`.

The first search of a folder reads it from the disk. Seek then keeps it in memory, kept current by the drive watchers, and the next searches there don't read the disk.

### `recent.rows`

What the user created, changed, renamed or opened lately, newest first.

| arg | |
|---|---|
| `path` | only below this folder |
| `query` | only names like this, as in `find.rows` |
| `since` | how far back: `30m`, `2h`, `3d`, `2w`. Default `7d` |
| `walk` | with `path`: also read the dates on the disk there, to cover what changed while Seek wasn't running. Default `true`; `false` answers from what Seek saw, in milliseconds |
| `include_system` | also system places (AppData, Windows, Program Files, temp, caches) and what `.gitignore` leaves out. Default `false` |
| `limit` | rows at most, 1 to 100,000; 1,000 by default |

```
#rows	<total>	<exact>
<D|F>	<path>	<time>	<what>
```

- `time`: Unix seconds.
- `what`: `created`, `changed`, `renamed` or `opened`.

### `status.rows`

The index of each fixed drive. A program shows this while a first index is being made (on a PC without Jolt).

No args.

```
#rows	<version>	<drives>
<letter>	<state>	<names>	<source>	<time>
```

- `state`: `loading` (Seek has just started and is reading its indexes: about a second), `indexing` (the first index of that drive is being made), `ready`, or `none`.
- `names`: how many names; while indexing, how many so far. How many a drive has in all is known only at the end, so there is no percentage.
- `source`: `jolt` (Jolt's index, copied) or `own` (Seek walked the drive).
- `time`: when the index was taken, in Unix seconds; `0` when there is none yet.

While a drive is indexed for the first time, searches on every drive lack its names, and searches with a `path` there walk the folder.

### `opened`

A program opened a file: it then ranks as used in `find.rows`, and `recent.rows` lists it as `opened`, until it changes again.

| arg | |
|---|---|
| `path` | required. The file |

`text` is `ok`.

### `stop`

The background process saves what it knows and exits. A program uses it only to replace an older Seek with a newer one (see Starting Seek). Other programs' calls in flight fail, and their clients start Seek again.

No args. `text` is `stopping`.

## Errors

| `code` | |
|---|---|
| `bad_request` | the message isn't JSON |
| `bad_args` | an argument is missing or wrong, or a regex doesn't parse |
| `not_found` | the path given isn't there |
| `not_a_folder` | a file was given where a folder is needed |
| `private` | the path is in a place Seek never looks (keys, browser profiles, password managers, and what the user excluded) |
| `index_loading` | a search on every drive, while the indexes load after a start. Try again in a moment |
| `unknown_command` | the command doesn't exist in this version |
| `api_too_new` | the request asked for a newer protocol than this daemon speaks |
| `failed` | anything else |

A later version may add codes. A program treats a code it doesn't know as `failed`.

## Timeouts

Seek answers every request, but a client shouldn't wait forever on a reply. The background process could hang, or be killed halfway through writing. These limits cover the slowest legitimate answer:

| command | wait at most | why it can take that long |
|---|---|---|
| `hello`, `opened`, `stop`, `status.rows` | 5 s | they touch neither the index nor the disk's folders |
| `find.rows`, `recent.rows` | 45 s | right after Seek starts, they wait for the indexes to load (up to 30 s on a slow disk) and for the drives' change journals to be read (up to 10 s). Otherwise they answer in milliseconds |
| `grep.rows` | 3 × `time_budget_ms` + 15 s (33 s by default) | a literal search that finds nothing is tried again ignoring case, and as a regex, each within the budget |

**How to stop waiting.** Two ways:

- **Overlapped I/O.** Open the pipe with `FILE_FLAG_OVERLAPPED`, wait on the read with a timeout (`WaitForSingleObject` on its event, or `GetOverlappedResultEx`), and on timeout call `CancelIoEx`.
- **A reading thread.** If the read already runs on a thread of its own, call `CancelSynchronousIo` on that thread from another one.

**After a timeout, close that connection and open a new one.** The old reply may still arrive, half read, and the stream can't be trusted any more. The request itself keeps running in Seek until it ends, which does no harm.

If `hello` on the new connection doesn't answer within its 5 s either, the background process is stuck. Tell the user, and leave it running: killing it loses what it saw change since its last save, at most 5 minutes.

## Starting Seek

Seek's background process (`seek.exe --daemon`) serves every program and AI agent of the user: one index, one history, and it keeps running after the program that started it closes. It isn't started with Windows: the first client that needs it starts it.

Seek is installed in `%LOCALAPPDATA%\Programs\Seek\seek.exe`. A program may also carry a copy of its own.

**When the pipe isn't there:**

1. Of the `seek.exe` the program knows (the installed one, and its own copy if it has one), start the newest. At the same version, the installed one. `seek.exe --version` prints `seek X.Y.Z`.
2. Start it as `"<path>" --daemon`, with `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP`, and `CREATE_BREAKAWAY_FROM_JOB` when allowed (try with it, then without), so that it outlives the program. Without the `SEEK_HOME` variable set: that makes a separate instance, with data and a pipe of its own.
3. Wait for the pipe, trying every 100 ms for up to 15 seconds.

**When the pipe is there:**

1. Use it. Every reply carries `version`.
2. If the program has a newer `seek.exe` than the one running, send `stop`, wait until the pipe is gone, and start the newer one as above. Then call `hello` again: an agent's `seek.exe` may have started another one in between. At most two tries.
3. Do this only when the one running is older: `stop` interrupts the other clients' calls.

**A copy of its own** is written under the program's folder in `%LOCALAPPDATA%`, as `seek-<version>.exe`, through a temporary name and a rename: a running `seek.exe` can't be replaced, so versions sit side by side, and old ones are deleted when nothing runs them.

Whatever `seek.exe` runs, the data is the same: `%LOCALAPPDATA%\Seek` (the indexes, the history), and `%APPDATA%\Seek\seek.txt` (the settings). A program that uninstalls itself leaves them: an installed Seek and the user's agents use them too.

Seek's installer stops the background process before it installs, whatever `seek.exe` it runs from.

## What version 1 promises

From Seek 0.5.0 on, as long as the daemon's `apis` include `1`:

- these commands keep their names, their arguments and their rows; rows may gain fields at the end, and replies may gain fields;
- errors keep their codes; new codes may appear;
- the names they had before 0.5.0 keep working: `__rows`, `__grep_rows`, `__recent_rows`, `__status_rows` and `__opened`.

A change that breaks any of that is version 2, and a daemon that speaks it keeps speaking 1.

## Limits

- Windows only, fixed drives. A folder on a network share or a removable drive can be given as `path`: it is walked instead of searched in the index.
- One background process per user.
- The first index of a drive without Jolt takes seconds on an SSD and minutes on a hard disk (19 minutes for 1.4 million names on one here, read cold).
