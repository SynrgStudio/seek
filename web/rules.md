# What is never published about Seek

PORTFOLIO reads this before every change. It overrides everything else in `web/`.

## Private things

- **Paths and names from the author's PC.** No real user name, drive folder or project path: examples use a demo user (`C:\Users\me\…`), as `notes/tools.md` does. The folder names in the `.blend` summary example are made up too.
  - *Why:* they identify a person and their machine.
- **Private projects.** No name, code, file name or path from closed projects that reported bugs to Seek. Seek's CHANGELOG describes those cases by their shape ("a C++ file with lines of 1,700 characters"); the page does the same, or leaves them out.
  - *Why:* those repositories are private, and Seek's is public.
- **Seek's search window**, a desktop app for people built on Seek's protocol. Seek's CHANGELOG names it as a user of the pipe, but it isn't released: its repository is private, and there is nothing to download. Don't present it as a product, show it, or promise it, until the author announces it.

## Not yet true

- **The client packages.** `seek-client` is written but not published on crates.io, PyPI or npm. Don't show `cargo add`, `pip install` or `npm install` for it as if it were available. "Written, in the repository; publishing is on the roadmap" is right.
- **The signed executable, an instant first index without Jolt, ranking by git status, definitions:** next steps, not features (`notes/limits.md`).
- **A newer benchmark.** The numbers are Seek 0.1.0's. Don't present them as 0.5.0's. And don't scale them by the speedups in the CHANGELOG: only what was measured is published.

## How numbers are said

- **Every number comes from `facts.toml`, by its key.**
  - Don't round in Seek's favor: 45× is 45×, not "about 50×".
  - Don't turn one PC's run into a general claim. "On one PC, 4.9 million names" stays attached to it.
- **Never "faster than Everything" for names.** The lookup is even; the difference measured is mostly starting `es.exe`. What Seek adds over Everything is typo tolerance, ranking, summaries, text search, and no service or administrator rights.
- **Every comparison says what the other tool does better** (`notes/results.md`). Dropping that side makes the page a sales page, which it has never been.
- **Numbers measured by others** keep their source link, and say they come from other machines.

## Third parties

- **The other tools may be named:** Everything, fff, ripgrep, fd, git grep, Windows Search, PowerShell's commands, The Silver Searcher, ugrep, ack, and Claude Code's Glob and Grep. Say what they do as their own documentation does.
- **Don't use their logos or screenshots of them.**
