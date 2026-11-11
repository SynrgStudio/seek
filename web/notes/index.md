---
title: The drive index
status: stable
updated: 2026-11-11
---

Every name on every fixed drive, in the format Jolt keeps. Each entry is 12 bytes [index_entry_bytes]: its parent, where its name is, whether it is a folder, and its depth. The names follow in one block.

**Where the index comes from:**

- **With Jolt installed,** Seek copies Jolt's index, which Jolt builds from the drive's file table.
  - It looks for a newer copy every 10 minutes [jolt_check_min].
  - It copies the file instead of reading it in place, because Jolt replaces the file when it reindexes, and a mapped file would block that.
- **Without Jolt,** Seek walks each drive itself, on up to 8 [index_walk_threads] threads, with no special rights.
  - On the benchmark PC: 18 s [index_ssd_s] for 2.1 million [index_ssd_names_millions] names on an NVMe SSD, 3.8 s [index_ssd2_s] for 1.3 million [index_ssd2_names_millions] on another, and 19.5 minutes [index_hdd_min] for 1.4 million [index_hdd_names_millions] on a hard disk read cold.
  - It saves the result and walks the drives again once a day in the background, while the old index keeps answering.
  - While a drive is indexed for the first time, searches on every drive lack its names; a search below a folder there walks the folder.
- **Between walks,** the drive watchers add what is created and hide what is deleted, so an answer doesn't depend on how old the index is. What was created since is kept between runs too.

**Searching** runs every name through the fuzzy matching on up to 16 [name_scan_threads] threads.

- Each thread takes runs of names that are next to each other in memory, and keeps the best in a heap.
- Equal scores keep the index's order, so the same search always gives the same answer.

**Searching inside one folder.** The index lists names in the order of the drive's file table, so a folder's contents are scattered among millions of names.

- The first search scoped to a folder builds each folder's list of children. From then on, a search below a folder visits only what is below it.
- With a folder, the ignore rules (`.gitignore` and dependency folders) are worked out once per search from the index's folders. An ignore file is read from disk only where the index says there is one.
