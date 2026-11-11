---
title: Seek for programs: the protocol, and the client packages
status: stable
updated: 2026-11-11
---

Besides agents, any program of the same user can ask Seek's background process directly: a search box, an editor, a script. It sends JSON through Seek's named pipe and gets rows back, from the same index and history the agents use. Per request the pipe costs about 0.1 ms [overhead_ms]; the search itself is what the benchmark measured.

The protocol is version 1 [protocol_version], specified in [PROTOCOL.md](https://github.com/SynrgStudio/seek/blob/main/PROTOCOL.md) (`protocol` in `[project]`), since Seek 0.5.0:

| command | what it gives |
|---|---|
| `find.rows` | files and folders by name, best first |
| `grep.rows` | the matching lines below a folder: path, line, column, text |
| `recent.rows` | what was created, changed, renamed or opened lately, with the time |
| `status.rows` | each drive's index: loading, being made (with the names so far), ready |
| `opened` | tells Seek a program opened a file, so it ranks as used |
| `hello`, `stop` | what is running; make room for a newer Seek |

**What version 1 promises:**

- Replies and errors are versioned, and errors carry a code.
- Rows only grow at their end, and the shape of every reply is fixed by the tests.
- A program can carry a copy of `seek.exe`. The spec says how to start Seek so that the newest one runs and every program shares one background process and one index.

**Who uses it:**

- [Slate](https://github.com/SynrgStudio/slate), a text editor by the same author, uses it for its file finder.
- **Client packages** are on the roadmap ([issue #27](https://github.com/SynrgStudio/seek/issues/27)).
  - The Rust client, `seek-client` (MIT OR Apache-2.0), is written and tested in the repository (`clients/rust`), but **not published** on crates.io yet. A program can use it from the repository meanwhile.
  - Its `bundled` option carries one Seek release's `seek.exe` inside the program, checked against that release's SHA-256, for PCs where Seek isn't installed. It starts it only when no newer Seek is installed, and a newer one installed later replaces it.
  - Python and npm come after it.
