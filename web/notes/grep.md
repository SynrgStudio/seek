---
title: How grep reads a folder
status: stable
updated: 2026-11-11
---

1. **The walk.** One pool of up to 12 [grep_threads] threads both lists folders and takes files, files first, so the queue stays short. Results are sorted back into the order a single walk gives: a folder's files, then its subfolders, by name.
2. **The rules.**
   - At the root of each repository Seek adds the user's global excludes and `.git\info\exclude`, then each folder's `.gitignore` and `.ignore`.
   - Without any `!` rule, plain names and `*.ext` rules are looked up in a set instead of matched one by one.
   - Dependency and cache folders are skipped by name. So are private places (`privacy.md`), checked once per folder on the way down.
3. **The bytes.**
   - A file's first 8 KB [binary_probe_kb] tell whether it is binary; then the rest is never read.
   - For a literal pattern in ASCII, Seek looks for its bytes before decoding anything, since those bytes are the same in UTF-8 and in the system code page. A regex that is only plain texts joined by `|` gets the same first look.
   - The search starts at the pattern's rarest character, with `memchr`, and compares around it, folding case on the fly when needed. Most files end here.
4. **The text.** Only a file that may match is decoded: from UTF-16 by its mark, from UTF-8, or from the system code page. Then each matching line gets its line number, its column, and its context.
5. **Documents** go through the text filter Windows has for their type. The PDF filter that comes with Windows only accepts a stream, which the classic `LoadIFilter` can't give it, so Seek creates the filter itself and hands it one.

**Limits of the reading:**

- A folder's files over 16 MB [grep_file_max_mb] aren't read, and the answer names them. A file given as the path is read whatever its size, in parts.
- `regex` uses the C++ standard library's engine. It looks at the first 4,096 [regex_line_chars] characters of a very long line.

**After the first search** the folder's text stays in memory: `memory.md`.
