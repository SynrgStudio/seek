---
title: How the benchmark was measured
status: stable
updated: 2026-11-11
---

On one PC, with every tool doing the same job the way an agent would run it.

**The setup:**

- **The PC:** [machine]. Three drives hold 4.9 million [names_millions] files and folders.
- **The versions:** [tool_versions]. Seek was version 0.1.0 (`bench_seek_version` in `[project]`). Later versions are faster at `find` (about three times, in the CHANGELOG of 0.4.4), but the benchmark hasn't been run again yet. The page shows what was measured, which is what 0.1.0 did.

**How each tool was timed:**

- **MCP servers, Seek and fff,** ran warm, the way an agent's session keeps them. They were timed from request to answer over stdio.
- **Command-line tools** (Everything's `es.exe`, ripgrep, fd, git and PowerShell) were started once per search, the way an agent's shell runs them. Their times include starting the process, because the agent pays for that.
- **Times are medians** of five runs, three for the slow ones.
- **Warm cache for everyone.** Before timing, every tool searched once, so the files were in Windows' cache for all of them. Walks of every drive ran once each, on a warm cache.

**Tokens** are the characters of the answer the agent reads, divided by four.

**Two exceptions:**

- The rows for `dir`, `Get-ChildItem`, `Select-String` and `findstr` come from an earlier run of the shell tools, because a ten-minute walk isn't worth repeating.
- fd walking the drives read from disk was cut at 15 minutes [find_fd_from_disk_min].

**Anyone can repeat it:** `python -I bench\bench.py --walkers` times Seek against whatever of these tools it finds. Every raw number is in `data/results.json`.

`data/results.json` holds:

| field | |
|---|---|
| `rows` | one per tool and task: seconds, tokens of the answer, found or not, count |
| `tasks` | the tasks' names by short id |
| `labels` | the folders' descriptions |
| `facts` | the machine, versions, overhead, index build times |
| `history`, `history_cols` | grep's history, see `journey.md` |
