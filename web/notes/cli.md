---
title: Using Seek from a terminal: a guide for people
status: stable
updated: 2026-11-25
---

From 0.8.0 on, Seek has a command line made for people, besides the one for agents and scripts. It is the same search agents get, asked from the same background process, with short flags, colors, and a step by step mode. The installer puts `seek` on the PATH: open a new terminal after installing.

Paths in the examples are a demo user's (`C:\Users\me`).

## The four commands

```
seek report pdf            names with those words, on every drive
seek g TODO src            the lines with that text, in the files below src
seek r 2h                  what changed or was opened in the last 2 hours
seek i report pdf          step by step: each line typed changes the search
```

- **`seek <words>`** finds files and folders by name, on every fixed drive, in milliseconds. `seek f <words>` and `seek find <words>` are the same.
- **`seek g <text> <folder>`** finds text inside the files below a folder; without the folder, below the current one. `seek grep` is the same.
- **`seek r <span>`** lists what was created, changed, renamed or opened lately, newest first. The span is `30m`, `2h`, `3d` or `2w`; 7 days [recent_default_days] by default. `seek recent` is the same.
- **`seek i …`** starts step by step mode (below). `seek i g TODO` and `seek i r 2h` start it on grep or recent.
- **`seek -h`** lists everything. `seek` alone, with no arguments, is the MCP server an agent starts, not a search.

## Finding names

The words are those of a name, in order, and forgiving: a typo still finds it.

| you type | finds |
|---|---|
| `seek report 2026` | names with both words, in that order |
| `seek fuzy.inl` | `fuzzy.inl`, with the typo |
| `seek report .pdf` | only `.pdf` files (or `-e pdf`) |
| `seek "final draft"` | the words as typed, whole |
| `seek *.log` | wildcards on the name: `*` and `?` |
| `seek report !old` | leaves out names with `old` |
| `seek src/main.rs` | `main.rs` below a folder called `src` |
| `seek report ~\Docs` | only below that folder |

**Where to look.** `-p <folder>`, or the folder itself among the words when it looks like one: `.`, `..`, `~`, `.\x`, `..\x`, `~\x`, `C:\x`. `~` is the home folder (`C:\Users\me`). Below a folder, what `.gitignore` leaves out and dependency folders (`node_modules`, `.venv`…) are left out, as they are for agents; `-a` searches them too.

**What comes first.** Exact names and programs, then what the user changed or opened lately, then what is in folders they use, as for agents (`ranking.md`). `--near <folder>` ranks what is near that folder first.

## Finding text

```
seek g TODO                      below the current folder
seek g "connection refused" logs
seek g TODO src -e cpp,h         only those extensions
seek g todo -g *.md -g !*.min.js
seek g "fn \w+_test" -x          a regex
```

- **Case.** It ignores case unless the text has capitals. `-i` ignores it always, `-c` matches it exactly.
- **What it reads, and skips,** is what grep reads for agents (`tools.md`): code and text in any encoding, PDF and Office documents, `.gitignore` followed, binaries and files over 16 MB [grep_file_max_mb] skipped. `-a` also searches what `.gitignore` leaves out, dependency folders and hidden files.
- **Globs that are names** (`-g *.gradle`, `-g Cargo.toml`) take the files from the index, so even a whole drive answers in a moment (`grep.md`).
- **When nothing matches** in the exact case, it tries ignoring case, and says so; not with `-c`. A literal text with regex signs that finds nothing is tried as a regex, and says so.

## Recent

```
seek r                 the last 7 days (the default)
seek r 30m             the last half hour
seek r 3d -p ~\Projects
seek r 2h report       only names like "report"
```

Each line says when, what happened (`created`, `changed`, `renamed`, `opened`) and the path. System places (AppData, Windows, temp folders, caches) are left out; `-a` shows them.

## Flags

| flag | | for |
|---|---|---|
| `-p <folder>`, `--path` | only below that folder | all |
| `-n <how many>`, `--limit` | how many results | all |
| `-d`, `--dirs` | folders only | names |
| `-f`, `--files` | files only | names |
| `-e pdf,md`, `--ext` | those extensions | names, recent |
| `-s`, `--summary` | counts by type and folder instead of a list | names |
| `-a`, `--all` | also ignored and dependency folders, hidden files, system places | all |
| `-o`, `--open` | open the first result | all |
| `--near <folder>` | rank what is near it first | names |
| `-i`, `--ignore-case` | ignore case | text |
| `-c`, `--case` | match case exactly | text |
| `-x`, `--regex` | the text is a regex | text |
| `-g <glob>`, `--glob` | which files (`*.md`, `!*.min.js`, several with commas) | text |
| `-v <text>`, `--without` | leave out lines that have it | text |
| `-t <span>`, `--since` | how far back | recent |
| `--color always\|never\|auto` | colors | all |
| `--rows` | the rows of the protocol for programs (`PROTOCOL.md`) | all |

Short flags that take no value go together: `-ds` is `-d -s`.

**How many.** In a terminal, 30 [cli_names_terminal] names, 50 [cli_text_terminal] lines of text, 30 [cli_recent_terminal] recent; in a pipe, 1,000 [cli_pipe] (text: 2,000 [cli_pipe_text]). `-n` changes it; the footer says how many there are and how many are shown.

## In a terminal

The results come in colors, the terminal's own 16, so they suit its theme, light or dark:

- **Names:** the folder dim, the name bright, and the letters that match what was typed lit; folders found in blue.
- **Text:** the file, the line number and the match, each in its own color, grouped by file.
- **Recent:** when, and what happened, each in its own color.
- **Notes**, such as "these are close to it, as if it had a typo", in yellow.

Paths show from the current folder when they are below it, and from `~` when they are below the home folder. A path too long for the line loses its middle. In Windows Terminal, VS Code and WezTerm a path opens when clicked.

`NO_COLOR` (the variable, set to anything) or `--color never` turns colors off.

## In a pipe

When the output goes to another command or a file, Seek prints only the results: one full path a line, and for text `path:line:text`, as editors and ripgrep's users expect. No colors, no counts.

```
seek report pdf | select -first 1 | ii       open the best one
seek *.log -p C:\Logs | Copy-Item -Destination D:\Old   copy them
code (seek main.rs -n 1)                     open it in VS Code
seek g TODO src | Measure-Object -Line       how many lines
```

**Exit codes:** 0 with results, 1 with none, 2 on an error (a flag Seek doesn't know, a folder that isn't there). A script can test them.

`--color always` keeps the terminal view in a pipe, for a pager that shows colors (`less -R`).

## Step by step: `seek i`

For when what to look for, or how precisely, isn't clear yet. Seek shows the results, then each line typed changes the search and shows it again. The prompt always shows the whole search:

```
> seek i report pdf
  1  ~\Documents\report_2024.pdf
  2  D:\Backup\report final.pdf
  …
about 312 found · showing 30

report pdf › -p ~\Projects
  1  ~\Projects\site\docs\report.pdf
  …
14 found

report pdf · ~\Projects › 2024
  1  ~\Projects\site\docs\report_2024.pdf
3 found

report pdf 2024 · ~\Projects › 1
Opened ~\Projects\site\docs\report_2024.pdf
```

| type | does |
|---|---|
| a word | adds it: fewer results |
| `-word` | takes that word out again |
| `!word`, `.pdf` | as in a name: leaves out names with it, an extension |
| any flag | `-p`, `-n 50`, `-d`, `-e pdf`, `-s`, `-a`… changes that and searches again |
| the same flag again | undoes it: `-d`, then `-d`, is folders, then everything again |
| `-p` alone | not below any folder any more |
| Enter | the next results |
| `3` | opens the 3rd result |
| `o 3` | shows it in its folder, in Explorer |
| `c 3` | copies its path |
| `g text` | searches that text in the files, below the same folder |
| `f words` | finds names again |
| `r 2h` | what changed lately, below the same folder |
| `u` | one step back |
| `?` | the search as one command, to type again or keep in a script |
| `h` | the help |
| `q` | quits |

Switching with `g`, `f` or `r` keeps the folder, the number of results and `-a`. Numbers count on from page to page, so `35` opens the 35th even after Enter. `seek i` reads its lines from a pipe too, for a script.

## Opening results

A number in `seek i`, or `-o`, opens the file with the program Windows opens it with, and a folder in Explorer. Seek then counts it as used: it ranks higher in later searches, and `seek r` lists it as `opened`.

To open text results in an editor at their line, set `SEEK_EDITOR`, with `{path}` and `{line}` where they go:

```
$env:SEEK_EDITOR = 'code -g {path}:{line}'     VS Code
$env:SEEK_EDITOR = 'notepad++ -n{line} {path}' Notepad++
```

Without `{path}`, the path goes at the end. To keep it, set it in the PowerShell profile or as a user environment variable.

## For agents, scripts and the background process

These are unchanged (`settings.md`):

```
seek --cli find "{\"query\":\"report .pdf\"}"   a tool in this process, answered as for an agent
seek --call grep "{...}"                        the same through the background process
seek --status, --stats, --reindex, --stop, --version
```

## Not yet

- Lines of context around a text match in a terminal (`-C`): agents have them; the command line doesn't yet.
- Filtering as each key is typed, a command to jump to a folder (`cd`), and tab completion of flags: ideas, not there.
