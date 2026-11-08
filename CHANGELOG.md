# Changelog

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
