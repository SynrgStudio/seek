---
title: The comparisons, tool by tool
status: stable
updated: 2026-11-11
---

All numbers are from the benchmark (`method.md`), on one PC, Seek 0.1.0. The charts on today's page are dot plots on a log scale, since the times span six orders of magnitude. Every row of `data/results.json` can be charted the same way: `rows` grouped by `task`.

## Names, on every drive

| task | Seek | the others |
|---|---|---|
| find a file, place unknown | 52 ms [find_ms] | Everything 93 ms [find_everything_ms]; Windows Search 233 ms [find_windows_search_ms], didn't find it; fd 4.7 s [find_fd_s] with the folders in memory, over 15 min [find_fd_from_disk_min] from disk; ripgrep --files 5.8 s [find_ripgrep_files_s]; `dir /s` 85 s [find_dir_s] and `Get-ChildItem` 98 s [find_get_childitem_s] over one profile |
| the same with a typo | 57 ms [find_typo_ms], found | Everything didn't find it [find_typo_everything_found]; neither do fd or ripgrep (they match exactly) |
| how many `.blend` files, and where | 55 ms [count_ms], 378 tokens [count_tokens] | Everything's list 100 ms [count_everything_list_ms] and 196,438 tokens [count_everything_list_tokens]; Everything's count alone 74 ms [count_everything_count_ms] (no "where"); fd 5.0 s [count_fd_s], 194,388 tokens [count_fd_tokens]; `dir /s` 567 s [count_dir_s], 104,157 tokens [count_dir_tokens]; Windows Search found 9 [count_windows_search_found] of 8,200 [blend_files] |
| what changed lately | 9.1 ms [recent_ms] | Everything's newest 10: 105 ms [recent_everything_ms], and it answers with logs, caches and browser data |

**Why Seek is fast here.** It doesn't walk anything: every name on every drive is already in memory, from Jolt's index or one Seek builds itself (`index.md`). A search runs every name through the fuzzy matching on up to 16 [name_scan_threads] threads and keeps the best in a heap.

Everything is as fast at the lookup itself. Most of its time here is starting `es.exe`, which an agent can't avoid without an MCP server for it.

**Why the answer is short.** Counting 8,200 files and saying where they are is a summary Seek computes, not a list the model reads.

## Text, in folders

| task | Seek from memory | Seek, first search | fff | ripgrep | other |
|---|---|---|---|---|---|
| a word in one repository (52 files [repo_files]) | 0.40 ms [grep_repo_ms] | 3.2 ms [grep_repo_first_ms] | 0.61 ms [grep_repo_fff_ms] | 29 ms [grep_repo_ripgrep_ms] | git grep 35 ms [grep_repo_git_grep_ms]; Select-String 6.7 s [grep_repo_select_string_s] and 2,348,604 tokens [grep_repo_select_string_tokens], findstr 2.8 s [grep_repo_findstr_s] (both read build outputs too) |
| a word in every project (10,700 files [projects_files]) | 11 ms [grep_projects_ms] | 132 ms [grep_projects_first_ms] | 36 ms [grep_projects_fff_ms] | 487 ms [grep_projects_ripgrep_ms] | |
| a name in one repository | 0.33 ms [find_repo_ms] | | 0.43 ms [find_repo_fff_ms] | | |
| the same with a typo | 0.40 ms [find_repo_typo_ms] | | 0.44 ms [find_repo_typo_fff_ms] | | |

**The ratios.** On every project, Seek from memory was 3.3× [text_vs_fff] faster than fff and 45× [text_vs_ripgrep] faster than ripgrep. Its first search, reading the drive like ripgrep, was already 3.7× [text_first_vs_ripgrep] faster.

"Seek, first search" reads the folder from the drive. After it, the background process keeps the folder in memory, and "from memory" is every search after that (`memory.md`). fff, in the same way, answers from the index it built when its session started. ripgrep is what Claude Code's Grep tool runs.

**Against fff,** which is built for this, Seek was faster in every row, and the gap grows with the folder. In the small repository both answer in under a millisecond, and most of that is the trip from client to server and back.

## Starting, and memory

| | ready to answer | memory of its own |
|---|---|---|
| Seek | 296 ms [start_ms], every drive, from Jolt's index | 328 MB [memory_mb] with every project kept in memory for grep; 32 MB [memory_no_folders_mb] without; one process for every agent |
| Seek without Jolt | 18 s [index_ssd_s] for 2.1 million [index_ssd_names_millions] names on an NVMe SSD; 3.8 s [index_ssd2_s] for 1.3 million [index_ssd2_names_millions] on another; 19.5 min [index_hdd_min] for 1.4 million [index_hdd_names_millions] on a hard disk read cold | the same |
| Everything | always running, as a service | 493 MB [memory_everything_mb] |
| fff | 110 ms [start_fff_repo_ms] for the repository, 246 ms [start_fff_projects_ms] for every project (it scans the folder) | 114 MB [memory_fff_repo_mb] for the repository, 201 MB [memory_fff_projects_mb] for every project; one process per agent session and folder |

**What matters for memory** is how many copies there are and who pays for them:

- Each agent session starts its own fff, with its own index of its own folder: three sessions in the same folder are three copies.
- Seek is one process for every agent. Its index files are mapped from disk, so Windows reads them in and drops them as it needs.

## What each other tool does better

The page has always said this, and it should keep saying it:

- **Everything:**
  - It indexes NTFS from the file table in seconds and updates it from the change journal.
  - It works for people, with no agent around.
  - It has a rich query language, a GUI, and network and FAT drives.
  - On time it is even with Seek: both answer from an index in memory.
- **fff:**
  - It ranks by how often files are opened and by git status.
  - It can return only the definitions of a name, and searches several identifiers in one call (SIMD Aho-Corasick).
  - It runs on Linux and macOS, and plugs into Neovim.
  - Its own benchmark is honest about its limits: an agent's whole task got faster with it, but the tokens barely moved (`published-elsewhere.md`).
- **ripgrep:**
  - It needs nothing running and no memory, and it is on every developer's machine.
  - Its regex engine is far faster than the C++ standard library's one that Seek uses for `regex: true`.
- **fd, `dir /s`, `Get-ChildItem`:**
  - fd is a fast walker: with the folders in Windows' memory it walks three drives in seconds. Read from disk, it takes as long as the disks do.
  - `dir /s` and `Get-ChildItem` are what agents use by default.
- **Windows Search** answers from its own index, but only for the places it indexes.
