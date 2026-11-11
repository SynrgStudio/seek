# Changelog

## 0.5.1

- **`find` finds a folder by its own dot name: `.git`, `.vscode`, `.github`.** A query like `.git` is an extension filter, and an extension never matched a folder, so a search for `.git` folders listed only the small `.git` files of worktrees, and below a repository answered "Nothing named like". Now a name that is exactly what was typed matches as a folder too, as a dotfile like `.gitignore` already did as a file. Files with that extension still match (`app.vscode`), and `type: folder` keeps only the folders. Inside a repository, `.git` is among what `find` leaves out below a folder, so it is shown from there, saying so.

## 0.5.0

Seek's protocol for programs, version 1: what any program can count on when it asks Seek through its pipe. The first step for client packages in Rust, Python and npm ([roadmap](https://github.com/SynrgStudio/seek/issues/27)).

- **[PROTOCOL.md](PROTOCOL.md)**: the whole protocol as a spec. It covers the messages, every command's arguments and rows, the error codes, how a program starts Seek or a copy of its own, and what version 1 promises to keep.
- **Stable names** for the commands for programs: `find.rows`, `grep.rows`, `recent.rows`, `status.rows` and `opened`. The names they had before (`__rows` and the like) keep working.
- **`hello`**: what a program is talking to (Seek's version, the protocol versions it speaks, the `seek.exe` running, its data folder, and whether the index is loaded), answered without touching the index.
- **`stop`**, for a program that brings a newer Seek than the one running.
- **Every reply says the protocol's version (`api`)**, and an error says its kind in `code`: `bad_args`, `not_found`, `not_a_folder`, `private`, `index_loading`, `unknown_command`, `api_too_new`, `bad_request` or `failed`. A request for a newer protocol than the daemon speaks is answered with `api_too_new`.
- **`find.rows` takes `max` inside `args` too.**
- **The installer stops a running Seek wherever it runs from**, a copy a program carries included. Before, it stopped one only when Seek was already installed.
- **Tests keep the replies' shapes** (`tests/protocol/v1.json`): a shape that changes fails the tests, old names included.

## 0.4.10

- **`__status_rows`**, for programs, through the pipe: Seek's version, and for each drive whether its index is loading, being made for the first time (with the names found so far), ready or missing, whose it is (Jolt's or Seek's own) and when it was taken. A program can say "Seek is indexing D: (812,345 names)" while the first index is made, instead of making up a percentage. README has the format.

## 0.4.9

- **`recent` right after Seek starts waits for what the last run saw.** The first call after the background process started (after an update or a reboot) could come before the saved activity was read: it answered with almost nothing, and "changes seen live since 0000-00-00". It now waits for that, as `find` does: about a second, once.

## 0.4.8

- **`recent` answers sooner, above all with `walk: false` or a `path`.** It asked whether each path it had seen change was left out by a `.gitignore`, which reads the disk, before asking whether its name was wanted at all; now the cheap questions come first, and the answers about `.gitignore` are kept for a minute between calls. Here, with a `path` and `walk: false`, from about 55 ms to under 10; with a query on every drive, about three times sooner.
- **`!folder/` on every drive costs little**: the names of the folders on the way up are compared, without making the path of every name that matches.

## 0.4.7

- **`find` below a folder leaves out what `.gitignore` leaves out there, and dependency and cache folders**, as `grep` already did: a search for notes in a Rust repository listed the files in `target` too, and in one repository here 449 of 509 `.md` files were in ignored folders. The answer says how many were left out, and `ignored: true` lists them. When a name is only in there, it is shown from there, saying so. Searches on every drive are as before.
- **`!folder/` leaves out what is in that folder** (`ext:md !target/`, `!docs/old/`). `!word` still leaves out names with the word.
- **For programs that list the answers themselves**, through the pipe: `__grep_rows` gives grep's matches one per line, with the path, line, column and text; `__recent_rows` gives recent's files with the time in Unix seconds and what happened, and `walk: false` skips reading the dates on disk; `__opened` tells Seek a program opened a file, so it ranks as used and `recent` lists it as opened. README has the details.
- **`recent` takes `walk: false`** too, and a path fragment in its query (`src/ .md`, `!target/`).
- **`grep` says when a slow first search read a hard disk**: the next ones come from memory.

## 0.4.6

- **`find` takes `{alpha,beta gamma}` as "a name with one of these in it".** A group in braces on its own was read as a wildcard that had to be the whole name, so `{report,summary} .pdf` found nothing even with both files there, and a space inside the braces split the query into words. Now a space inside braces belongs to the alternative, and braces alone match names that have one of the alternatives as typed, as whole words, like "in quotes". With the rest of a name around them (`{alpha,beta}.stl`, `*{2024,2025}*`) they are still a wildcard.

## 0.4.5

- **`find` with a folder and an extension, like `docs/ .png`, finds what is in that folder.** A part of the path was looked at only among the best 2,000 names kept, and with nothing but an extension or a common word those are the shallowest on the PC: a folder a few levels down had none of them, and the answer was "Nothing named like that" with the files right there. `src/main` listed a fraction of what it should for the same reason. Now the path is checked for every name that matches, so the list, its count and `summary` are of that folder.

## 0.4.4

- **`find` answers about three times sooner.** On a PC with 4.9 million names, a search by name went from 100–150 ms to 40–50 ms, and one with a wildcard (`*.blend`) from about 400 ms to 100 ms. Seek's window searches at every letter typed, and the wait showed. What changed: each thread reads names that are next to each other in memory instead of one in eight; scoring a match no longer asks for memory, nor twice whether the name is a program; turning the best matches into paths and ranking them by what the user did lately runs on several threads; the list of what was created since the index is made once, not at every search; and wildcards compare letters without a call for each.
- **The same search always lists equal scores in the same order**: the index's. Before, which thread had read a name could decide it.

## 0.4.3

- **`grep` takes a pattern like `var\("NAME` as a regex when the text itself isn't there.** A literal pattern with regex signs that finds nothing is tried as a regex, but a backslash right after a letter was read as a path's (`src\bin`), so `std::env::var\("ABC` answered 0 matches without saying why. A backslash before a bracket or a quantifier (`\(`, `\[`, `\{`, `\|`, `\?`, `\*`, `\+` and the closing ones) is now a sign wherever it is, and the answer says it searched as a regex.

## 0.4.2

- **A file made and then changed is still found after Seek restarts.** What was created since the index was taken is kept between runs in `activity.txt`, but that file held only each path's last change, and the newest 40,000: a file created and saved again was written down as changed, and after a restart (an update, a reboot) `find` no longer knew it until the next index, even by its exact name. On a busy day a new folder could drop off the end the same way. Now what the index lacks, and what it still has that is gone, are written down whole.

## 0.4.1

- **Rows for a program's own list.** The background process answers a new request on its pipe, `__rows`: a `find` whose answer is a count and one line per result (`D` or `F`, a tab, the path), up to as many as asked, for a program that lists the results itself. Seek's window, a search box for people in a repository of its own, is the first to use it. Nothing is read from the drive for those rows, the calls aren't written to the log or counted in `--stats`, and a program of any version is served.

## 0.4.0

- **The folders in memory come back after a restart.** The background process remembers which folders it kept for `grep` (their paths, in `kept.txt`) and reads them again when it starts, so the first search after an update or a restart is answered from memory. Before, it walked the drive again: on a folder of 10,800 files that had left the system's cache, 6 s and a partial answer, cut by the time budget.
- **`grep` runs a regex of plain texts joined by `|` as text,** without the regex engine: `foo|bar baz|x\.y`, which is how agents write most of theirs (93 of the 130 regexes with `|` in two days of calls). Files with none of the texts are passed over without being decoded, like for a literal pattern. The 93 over 10,800 files read from the drive: 183 s before, 19 s now; `ERROR|FATAL` in a log of 177 MB, 3.1 s before, 0.5 s now. And these find matches past the first 4,096 characters of a very long line, which the regex engine doesn't look at.
- **`seek --stats`** shows what agents asked, by day and tool: how many calls, the time half of them and 95 of 100 took, how many found nothing, how many were cut by the time budget, and how many failed. It is kept in `stats.txt`, past the log's rotation (the log holds about two days), so Seek can be compared over time; the first time, it starts from what the log still has. The command line and Seek's own tests and benchmark aren't counted.
- **The build on GitHub runs the tests on every push again,** and no longer publishes: releases are built, tested and published from the maintainer's PC, and the README says so. Before, each release cancelled that run, which left the README's badge failing.

## 0.3.7

- **Ready for what Jolt learns next** (Jolt's PR #146): `usage.txt` lines with a fourth column, `file`, are files opened from Jolt or chosen in its Ctrl+P. Such a file ranks higher in `find` ("opened in Jolt"), above the folder heat that applies to the others, and `recent` lists it. And `picks.txt`, what was chosen in Ctrl+P for what was typed: a `find` for the same text puts what was picked first ("picked in Jolt for this"). Lines of three columns are read as before. From a notice by Jolt's thread.

## 0.3.6

- **`find` takes "a phrase in quotes" as typed:** the name must have it, as whole words, ignoring case, so `"male 2"` finds `Male 2` and not `Female 2`, and a quoted full name finds that name only. Before, quotes only kept the words together as one fuzzy word, whose letters were found inside other names. Also in `recent`'s query. And with `path`, the header says the paths are relative to it, subfolders included. From a report.

## 0.3.5

- **`grep` takes a leading `(?i)` in a regex,** as other engines do and agents write by habit: it ignores case. `(?m)` and `(?s)` are taken too, and change nothing, since each line is searched apart. And a regex that doesn't compile says what it probably used that the engine (ECMAScript) lacks, and what to use instead: an inline flag past the start, lookbehind, named groups, `\A` and `\z`. Before, `(?i)foo|bar` answered `regex_error(error_syntax)` and nothing more. From a report.
- **A start reads only the part of the change journals it needs.** What the watchers saw is saved with where each drive's journal was, every 5 minutes and when the process stops, and the next start reads from there: the time no background process watched. Before, each start read all of the journal and looked up every name made since the index, which took about 3 s on a busy drive with an index of a few hours, and grew with its age; now the first call after a start takes under a second. Without a mark (the first start of this version, or a journal that went past it), the whole journal is read as before.

## 0.3.4

- **What was made while no background process ran is found.** At its start, the background process reads each drive's change journal (NTFS keeps one, readable without special rights) for what was made or renamed since the index was taken and is still there, the way the drive watchers would have seen it. Before, a file saved during an update, before the first agent call after logging in, or while the process was stopped, couldn't be found by name until the index was renewed. It adds about a second to the first call after a start, and `seek --status` says how many names it found. From a report: a file saved in Downloads during an update.
- **Building Seek doesn't stop the installed one:** `build.bat` stops only a background process running from `build\seek.exe` (`seek --stop here`). Before, every build stopped the user's background process too, and with it what the watchers see.

## 0.3.3

- **After an update, open sessions get the new tools without a restart.** The background process sends its version with each answer (a few bytes the agent never sees); when the session's `seek.exe` is older, it tells the client once that the tools changed, and gives it the new version's list, so new parameters and new tools can be used. Claude Code asks for the list again right away. It works from this version on: a session must already be 0.3.3 or later for a later update to reach it this way. Before, a session kept the tools it started with until it was restarted.
- **The installer puts Seek's folder on the user's PATH,** so `seek --status` and `seek --version` work in a console; `-NoPath` leaves it out. The PATH is read and written as the registry keeps it, its `%VARIABLES%` unexpanded, and new consoles have it at once.

## 0.3.2

- **`grep` shows more around a match:** `before` and `after` set each side apart, up to 200 lines, and `block` shows the whole block a match opens, up to the line that closes it, like a function's body after its signature. The block is told by indentation and brackets, so the same rule serves braces on the line (Rust, Go, JavaScript), on the next one (C#, C++), none (Python), and `end` (Ruby, Lua), up to 300 lines. Before, `context` went up to 5 lines on both sides, and reading a function found by its signature took the shell.

## 0.3.1

- **`grep` takes `without`:** the lines that also have that text are left out, like `grep -v` after the search. Literal, or a regex when `regex` is true, with the pattern's case rule. For leaving out declarations, comments or tests when the matches are many.

## 0.3.0

The second half of the review: what it suggested.

- **A session's tool calls run at the same time,** each on a thread and a connection of its own. Before, a session's calls were served one after the other, so a `find` sent during a slow `grep` waited for it ([#13](https://github.com/SynrgStudio/seek/issues/13)).
- **`find` follows a renamed folder:** what is inside is walked (with a budget) and counts as created since the index, and what the index lists under the old name is hidden. Before, after `mv old new` nothing inside `new` could be found until the index was renewed ([#16](https://github.com/SynrgStudio/seek/issues/16)).
- **A drive whose watcher couldn't start** (a BitLocker drive still locked, a drive that appears later) is tried again every five minutes. Before, its changes went unseen until the background process restarted ([#15](https://github.com/SynrgStudio/seek/issues/15)).
- **`grep` searches a file over 16 MB when it is the path given,** reading it in parts along line boundaries, with the line numbers and the context as if it were read whole. The limit stays for the files met on a walk of a folder, which are named as not read ([#8](https://github.com/SynrgStudio/seek/issues/8)).
- **`seek --status`** says how the background process is: each drive's index, its age and names; what was created and deleted since; the drive watchers and the batches they lost; the folders kept in memory for `grep`; and whether Jolt's index is used ([#14](https://github.com/SynrgStudio/seek/issues/14)).

## 0.2.2

A review of the whole code, with what it found.

- **Updating with agent sessions open:** the background process serves the sessions of an older Seek instead of stepping aside for them, so a session that was open at the update gets the new version's answers from its next call. Before, such a session made the new background process exit, then went on with the old version by itself ([#3](https://github.com/SynrgStudio/seek/issues/3)).
- **`find`:**
  - What was created since the index was taken is kept by path: a deleted file leaves it, a newer index drops what it already holds, and over 100,000 entries the oldest go, not the newest. Before, the list stopped at 50,000 and never let one go, so after that many files were created nothing new could be found until the index was renewed ([#4](https://github.com/SynrgStudio/seek/issues/4)). A file created since the index and deleted again leaves no record at all, so build churn no longer crowds out what matters.
  - `.gitignore` finds `.gitignore` ([#5](https://github.com/SynrgStudio/seek/issues/5)).
  - `src/report.pdf` looks for `report.pdf` under a `src` folder, `src/*.inl` works too, and `docs/` alone asks for a name ([#6](https://github.com/SynrgStudio/seek/issues/6)).
  - `{a,b}` in a wildcard means either ([#7](https://github.com/SynrgStudio/seek/issues/7)).
  - When the only names found have the letters scattered along them, the answer says so, the way it does for close names.
- **`grep`:**
  - `{a,b}` in a glob means either, and when a glob leaves every file out, or there is nothing to read, the answer says so ([#7](https://github.com/SynrgStudio/seek/issues/7)).
  - Files over 16 MB and files that can't be read are named, with the reason, instead of passed over in silence ([#9](https://github.com/SynrgStudio/seek/issues/9)).
  - A backslash right after a letter or a digit, as in `src\bin`, isn't taken for a regex sign ([#12](https://github.com/SynrgStudio/seek/issues/12)).
- `recent` no longer holds up the drive watchers while it filters what they saw ([#10](https://github.com/SynrgStudio/seek/issues/10)), and a watcher never waits for a folder in memory that is being brought up to date ([#11](https://github.com/SynrgStudio/seek/issues/11)).

## 0.2.1

- **`grep`:**
  - Matches close together share their context: each line is written once, a matching one with `:` even as another's context, and `--` splits the groups that don't touch, the way ripgrep does. Before, each match wrote its whole block, so the lines between two close matches came out twice or more.
  - A long line shows a window around each of its matches, up to four, with their columns, and counts the ones past those. Before, only the first match's window was shown, with no word of the others.
  - The header says how long the search took, says "read from Seek's memory, not the drive" instead of "kept in memory", and "1 file" instead of "1 files".

## 0.2.0

- **`find`:**
  - `offset` pages through the results, and each answer gives the offset of the next ones ([#1](https://github.com/SynrgStudio/seek/issues/1)). Every call ranks the same best 2,000 names, so the pages don't skip or repeat any.
  - The count is what there is to show. The same path twice, private places, deleted files and names outside a path fragment aren't counted any more, so a `limit` at the count shows them all, and "more" appears only when there is a next one ([#2](https://github.com/SynrgStudio/seek/issues/2)). When there are too many to count one by one, it says "about".
- **`grep`:** a literal pattern that finds nothing but has regex signs, like `a|b`, is tried as a regex, and the answer says so.
- `recent` leaves out what a repository's `.gitignore` leaves out, such as build outputs and generated sites, the same way it leaves out caches. The folders opened in Jolt always count, and `include_system` shows everything.
- **The installer:**
  - It finds Claude Code where its own installer puts it, `%USERPROFILE%\.local\bin`, even when that folder isn't on PATH.
  - It says what happened with each client.
  - It leaves the console's settings as they were.
  - It updates Seek while agent sessions are using it. Before, it couldn't replace `seek.exe` with any session open; now the old one is moved aside, and those sessions get the new one from their next call.

## 0.1.0

The first release, as a repository of its own. Seek began in [Jolt](https://github.com/SynrgStudio/jolt)'s repository and shares its fuzzy matching and its drive index.

- **Tools:**
  - **`find`:** names on every fixed drive in milliseconds, typo-tolerant, ranked by what the user is doing.
  - **`grep`:** text in a folder's files and in PDF and Office documents, following `.gitignore`.
  - **`recent`:** what the user changed and opened lately.
- **One background process for every agent,** reached through a named pipe only its user can open.
- **Folders kept in memory after their first search,** and kept current by the drive watchers.
- **Without Jolt,** Seek indexes the drives itself, in parallel, and walks them again once a day.
- **Private places are never searched,** and every call is written to a log the user can read.
- **MCP over stdio,** protocol versions from 2024-11-05 on, batches included.
- **The installer** downloads the release, checks its SHA-256, and adds Seek to Claude Code and Codex.
