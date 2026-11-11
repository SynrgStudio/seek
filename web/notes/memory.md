---
title: Folders kept in memory, and what Seek uses
status: stable
updated: 2026-11-11
---

After a folder's first search, the daemon reads its text files into memory on a thread of its own. A search that comes while that is happening waits for it, because it ends sooner than a second walk would.

From then on:

- **The drive watchers note every change** below the folder.
- **The next search applies the changes first:**
  - a changed file is read again;
  - a new one is added, if the rules let it in;
  - a deleted one is dropped;
  - a renamed folder is dropped and walked again under its new name.
- **The whole folder is read again** after:
  - a change to `.gitignore`, `.ignore` or `info\exclude`;
  - lost watcher events;
  - more than 20,000 [cache_changes_max] changes between two searches.
- **Searches with `hidden` or `ignored`** walk the folder as before, because the copy only holds what the default rules let in.
- **A search below a kept folder** uses it, unless the rules left that part out.

**It was checked:**

- Ten different searches over every project, compared from memory and from the drive, gave the same text, byte for byte.
- The tests check the changes one by one: a change, a new file, a deletion, a renamed folder, and a `.gitignore` edit.

**The budget:**

- Up to 512 MB [cache_mb] of folders by default.
- A folder nobody searched for 20 minutes [cache_idle_min] is let go.
- `cache = 0` in the settings turns it off.
- The folders kept are written down, and read again in the background when the daemon starts.

**What Seek uses:**

- With every project kept in memory, 328 MB [memory_mb] of its own; with no folder kept, 32 MB [memory_no_folders_mb].
- The index files are mapped from disk, so Windows reads them in and drops them as it needs.
- One process for every agent and program.
