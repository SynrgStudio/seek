---
title: Settings and command line
status: stable
updated: 2026-11-24
---

`%APPDATA%\Seek\seek.txt`, read when the background process starts:

```
exclude = C:\Users\me\Private      a folder
exclude = *.key                     files with that extension
exclude = secrets                   any folder or file with that name
log = no                            don't write log.txt
cache = 256                         MB of folders kept in memory; 0 for none (512 by default)
jolt = no                           don't use Jolt's index or what Jolt knows
```

Seek's own files are in `%LOCALAPPDATA%\Seek`:

- its copies of the index;
- `activity.txt`, with what it saw change;
- `log.txt`, with every call.

The command line, for people (from 0.8.0; the whole guide is `cli.md`):

```
seek report pdf              names with those words, on every drive (typos are forgiven)
seek report -p ~/Docs        only below a folder
seek g TODO src              the lines with that text in the files below src
seek r 2h                    what changed or was opened in the last 2 hours
seek i report pdf            step by step: each line typed changes the search
```

- **Flags:** `-p` folder, `-n` how many, `-d` / `-f` folders or files only, `-e pdf,md` extensions, `-s` counts by type and folder, `-a` also ignored folders, `-o` open the first one. For `g`: `-i` any case, `-c` this case, `-x` regex, `-g *.md` the files, `-v` lines without some text. `seek -h` lists them.
- **In a terminal** the results come in colors: the folder dim, the letters typed lit in the name, grep's file, line and match each in its own color. In Windows Terminal and VS Code a path opens when clicked. `NO_COLOR` or `--color never` turns colors off, `--color always` keeps them in a pipe.
- **In a pipe** it prints one full path a line (grep: `path:line:text`), so it goes into other commands: `seek report pdf | select -first 1 | ii`. Exit code 0 with results, 1 with none, 2 on an error.
- **`seek i`, step by step:** it shows the results, then each line typed changes the search and shows it again: a word more (fewer results), `-word` to take it out, `-p ~/Projects`, `-n 50`, `-d`. A flag typed again undoes itself. Enter shows more; `3` opens the third result, `o 3` shows it in its folder, `c 3` copies its path; `g text` looks inside the files there; `u` goes a step back, `?` prints the search as one command, `q` quits. `SEEK_EDITOR` (`code -g {path}:{line}`) opens results in an editor.

For agents, scripts and the background process:

```
seek --cli find "{\"query\":\"report .pdf\"}"      run a tool in this process
seek --call grep "{\"pattern\":\"TODO\",\"path\":\"C:\\\\code\"}"   through the background process
seek --reindex                                      walk the drives again now
seek --status                                       how the background process is: indexes, watchers, folders in memory
seek --stats                                        what agents asked, by day: calls, times, what found nothing
seek --stop                                         stop the background process
seek --version
```
