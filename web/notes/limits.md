---
title: Limits, and what's next
status: stable
updated: 2026-11-11
---

**Limits:**

- **Windows only, fixed drives.** Network shares and removable drives can still be searched by giving their folder as `path`: they are walked instead.
- **`regex` is slow.** It uses the C++ standard library's engine: correct, but much slower than ripgrep's. It looks at the first 4,096 [regex_line_chars] characters of a very long line.
- **The first index without Jolt** of a big drive takes a while: 19.5 minutes [index_hdd_min] for 1.4 million names on a hard disk read cold. Searches below a folder work right away meanwhile, by walking it.
- **`recent` knows only so much:**
  - what changed while Seek was running;
  - what the drives' change journals still held when it started (hours of changes on a busy drive, days on a quiet one);
  - the dates on disk when given a `path`.
- **The benchmark is of Seek 0.1.0.** Later versions are faster at `find`, and it hasn't been run again yet (`method.md`).

**Not yet:**

- reading the file table directly, for an instant first index without Jolt;
- ranking by git status;
- a search for definitions;
- a signed executable;
- published client packages (`programs.md`).
