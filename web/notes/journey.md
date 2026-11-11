---
title: grep, from 6 seconds to 11 milliseconds
status: stable
updated: 2026-11-11
---

The first comparison found Seek's grep slow on a big folder: it stopped at its budget of 6 s [grep_history_first_s]. Each step after it was measured with the same search, `CreateFileW` over every project. The steps, with their times and how many files each read, are in `data/results.json` under `history`, with the column names in `history_cols`. The last step is 11 ms [grep_history_last_ms].

What each step did:

1. **Look for the pattern's bytes before decoding any text.**
2. **Check privacy by the entry's own name, and look `.gitignore` names up.** Lookups instead of matching every rule.
3. **List folders on 12 threads.**
4. **Read only the first 8 KB of a binary.**
5. **Follow `.git\info\exclude` and the global excludes, as git does.** This one was a bug a comparison found: Seek read scratch files that git itself ignores. The `files read` column of `history` shows the drop.
6. **Keep the folder in memory, current through the drive watchers.** From then on a search reads no file at all.

A test now guards each of the two bugs this journey found: the ignore rules in `.git\info\exclude`, and the log being opened and closed on every call.
