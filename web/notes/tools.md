---
title: The three tools, and their query language
status: stable
updated: 2026-11-11
---

All three are read-only. Each answer says why each result is there. A long answer is cut, with a note on how to see more.

## find: files and folders by name

On every fixed drive, or below `path`.

- **Matching.** Fuzzy and typo-tolerant, with Jolt's matching.
- **Order.** Exact names and programs rank first, then what the user changed or opened lately, what is in folders they use in Jolt, and what is inside `near`. Caches and package stores rank last. See `ranking.md`.
- **Paging.** `limit` and `offset` page through the best 2,000 [ranked_results] by name. Each answer gives the offset of the next page.
- **Summary.** `summary` answers with counts by area, folder, extension and drive, instead of a list.
- **Below a folder** (`path`), `find` leaves out what `.gitignore` leaves out there, and dependency and cache folders (`node_modules`, `.venv`…), as `grep` does.
  - The answer says how many it left out, and `ignored: true` lists them.
  - When a name exists only in there, it is shown from there, saying so.
  - On every drive nothing is left out.

The query:

| syntax | means |
|---|---|
| `report 2026` | words in the name, in order, fuzzy |
| `"two words"` | as typed, as whole words |
| `.png`, `ext:png,jpg` | the extension; a file or folder named exactly that matches too (`.gitignore`, a `.git` or `.vscode` folder) |
| `*`, `?` | wildcards on the name; `{a,b}` either |
| `{alpha,beta gamma}` on its own | a name with one of those in it, as typed (spaces allowed) |
| `!word` | names without the word |
| `!target/` | nothing inside a folder called `target` |
| `src/report.pdf` | `report.pdf` below a folder called `src` (what is before the last slash must be in the path) |
| `type:file`, `type:folder` | only files, or only folders |

Example, with a typo on purpose (paths are a demo user's):

```
find {"query": "fuzy.inl"}
find "fuzy.inl" on all drives: 3 matches:
C:\Users\me\Projects\jolt\src\fuzzy.inl  ·  11 KB  ·  modified 9 h ago  ·  in a folder used in Jolt
C:\Users\me\Backups\jolt-old\src\fuzzy.inl  ·  3.4 KB  ·  modified 2 days ago
```

```
find {"query": ".blend", "summary": true}
find ".blend" on all drives: 8,200 matches.
By area:
    3754  D:\Art\Blender Stuff
    1907  E:\Art\Blender Stuff\DecalMachine Assets
  … and 111 more areas
By extension:  .blend 8,200
By drive:  D: 3,958  E: 3,973  C: 269
```

## grep: text inside files

In a folder, or in a single file.

- **What it reads.** Code and text in UTF-8, UTF-16 or the system code page. Documents through the text filters Windows has: PDF and RTF filters come with Windows; Word and Excel need Office or its filter pack.
- **What it skips.**
  - It follows `.gitignore`, `.git\info\exclude`, the user's global excludes and `.ignore`.
  - It skips dependency folders, binaries, files only in the cloud, and files over 16 MB [grep_file_max_mb]. The answer names the big files it skipped.
  - A file given as the path is read whatever its size.
  - `ignored: true` searches all of those too.
- **The pattern.** Literal by default; `regex: true` makes it an ECMAScript regex. It ignores case unless the pattern has capitals.
- **Shaping the answer.**
  - `glob`, `context`, `limit`, `offset` and `summary`.
  - `without` leaves out lines that also have some text.
  - `before` and `after` set each side of the context, up to 200 [grep_context_max] lines.
  - `block` shows the whole block a match opens, like a function's body.
- **When nothing matches.**
  - With no match in the exact case, it retries ignoring case.
  - A literal pattern with regex signs (`a|b`, `var\(`) that finds nothing is tried as a regex.
  - With no text match at all, it suggests file names that look like the pattern.
- **Long lines.** Matches close together share their context lines. A line longer than 240 characters [grep_line_chars] is cut around each of its matches, up to 4 [grep_line_windows].
- **The header** says how long the search took, and whether it came from memory, or from a hard disk the first time.

```
grep {"pattern": "RecTake", "path": "C:\\Users\\me\\Projects\\jolt"}
grep "RecTake" in C:\Users\me\Projects\jolt: 3 matching lines in 2 files (searched 52 files in 4 ms, read from Seek's memory, not the drive).
src\app.cpp
  1335- void StopRecursive(Tab& t, bool rebuild);
  1336: bool RecTake(Tab& t, const std::shared_ptr<const Listing>& l);  // search.inl
  1337- std::wstring g_renameAfterSelect;
```

## recent: what the user is working on

What the user created, changed, renamed or opened lately, newest first: files they changed, folders and files they opened in Jolt, and files a program told Seek it opened.

- **Coverage.** It covers the whole machine from what Seek saw change, kept between runs.
- **With `path`.** It also reads the dates on disk there. `walk: false` skips that and answers in milliseconds.
- **Filters.** `query` filters names, with find's syntax. `since` sets how far back to look (`30m`, `2h`, `3d`, `2w`).
- **Left out:** system places, programs' dot folders, logs and build outputs. `include_system` brings them back.

```
recent {"limit": 3}
recent on all drives, since 7 days ago, newest first:
C:\Users\me\Projects\seek\src\grep.inl  ·  changed 1 h ago  ·  22 KB
C:\Users\me\Projects\seek\src\util.inl  ·  changed 1 h ago  ·  13 KB
C:\Users\me\Documents\notes\ideas.md  ·  changed 2 h ago  ·  4 KB
```
