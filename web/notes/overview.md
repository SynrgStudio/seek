---
title: What Seek is
status: stable
updated: 2026-11-11
---

Seek is a file search for Windows, made first for AI coding agents (Claude Code, Codex, Cursor, any MCP client) and usable by any program through a named pipe.

It answers four questions, with four read-only tools:

- **`find`**: files and folders by name, on every fixed drive or below a folder. From an index of every name, in milliseconds. Typo-tolerant, and ranked by what the user changed or opened lately and the folders they use.
- **`grep`**: the text inside the files of a folder or of one file, PDF and Office documents included. It follows `.gitignore`. After a folder's first search Seek keeps its text in memory, kept current by the drive watchers.
- **`recent`**: what the user created, changed, renamed or opened lately, without the churn programs write all the time.
- **`sizes`** (since 0.6.0): what takes up the space below a folder or on a drive: the biggest folders and files.

**One background process** (`seek.exe --daemon`) serves every agent session and every program of the user at once. It keeps the index, watches the drives, and remembers what changed between runs.

**Answers are short on purpose.** An agent reads every character of an answer as tokens:

- Seek counts and summarizes instead of listing.
- Each result carries the reason it ranks where it does.
- Long answers are cut, with a note on how to see more.

**Facts that frame it:**

- One `seek.exe`, about 1 MB, C++20.
- No dependencies, no service, no administrator rights.
- Windows 10 and 11.
- GPL-3.0.
- It shares its drive index format and its fuzzy matching with [Jolt](https://jolt.synrgs.tech), a file explorer by the same author. When Jolt is installed, Seek uses Jolt's index.

The version, release date, license and links are in `facts.toml` under `[project]`.
