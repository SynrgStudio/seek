# What changed in Seek's corpus

## 2026-11-11, Seek 0.5.1

- `project.version` is 0.5.1.
- `notes/tools.md`: a query like `.git` also finds folders with exactly that name.

## 2026-11-11

The corpus is new, for seek.synrgs.tech built by PORTFOLIO. It holds everything the old page (`site/`) said, updated to Seek 0.5.0.

- **`facts.toml` is written by `tools/web_corpus.py`.**
  - It reads the measurements from `bench/results.json`, the version from `src/version.h`, and the design's numbers (threads, sizes, the ranking's points) from the source code.
  - Seek's tests fail when it is out of date.
  - Don't edit it by hand.
- **`data/results.json`** is a copy of `bench/results.json`, kept equal by the same script.
- **Notes**, one topic per file: `overview`, `problem`, `tools`, `method`, `results`, `published-elsewhere`, `how-it-works`, `index`, `ranking`, `grep`, `memory`, `journey`, `privacy`, `install`, `settings`, `programs`, `limits`. A number in a note has its fact's key after it, in brackets.
- **Changed from the old page:**
  - `find` scans names on up to 16 threads, not 8 (since 0.4.4): `name_scan_threads`.
  - The old page said "40× faster than ripgrep". The measurement says 45×: `text_vs_ripgrep`.
  - New since that page: `find` below a folder follows `.gitignore` (0.4.7); `!folder/` and `{a,b c}` in queries; `recent` with `walk: false` and opened files; the protocol for programs (0.5.0, `notes/programs.md`); the installer stops a Seek running from anywhere.
  - The benchmark is said to be of Seek 0.1.0, which it is (`project.bench_seek_version`).
- **Left out on purpose:**
  - The old page's "about 30 ms for a drive" for building the children lists, "from 5.2 ms" before them, "25,000 scratch files" and "0.3 ms per call" in grep's history. They were one-off observations while writing the code, not in the benchmark.
  - Seek's own search window (`rules.md`).
- **Media:** `seek-512.png`, `seek-256.png` and `seek.ico`.
