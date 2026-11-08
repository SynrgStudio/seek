# Changelog

## Unreleased

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
