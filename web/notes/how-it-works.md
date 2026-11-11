---
title: How it works: one process for every agent
status: stable
updated: 2026-11-11
---

```
Claude Code ─┐
Codex ───────┼─ MCP over stdio ─→ seek.exe (one per session) ─┐
Cursor, any ─┘                                                ├─ named pipe, this user only ─→ seek.exe --daemon (one for everyone)
Any program (find.rows, grep.rows…) ──────────────────────────┘                                   ├─ the index of every drive (copied from Jolt, or its own)
                                                                                                  ├─ drive watchers
                                                                                                  ├─ folders kept in memory for grep
                                                                                                  ├─ what changed, kept between runs
                                                                                                  └─ the log of every call (%LOCALAPPDATA%\Seek)
```

**The path of a call:**

- The agent starts `seek.exe` as a stdio MCP server, as with any other.
- That process doesn't search. It passes each tool call through a named pipe to `seek.exe --daemon`, starting it the first time.
- The daemon outlives the session and serves every agent and program on the PC. Everything expensive to build is built there, once.

**The details:**

- **The pipe** carries a security descriptor that admits only the user and SYSTEM, and rejects remote clients.
- **The connection stays open** for the whole session. The trip from client to answer and back takes 0.1 ms [overhead_ms].
- **Calls run at once.** Each call of a session goes on a connection of its own, so the calls an agent sends at once run at once.
- **Low priority.** The daemon runs at below-normal priority, so a burst of searches never competes with the user's work.
- **Updates.**
  - A daemon from an older build makes way for a newer one: the first call from a newer `seek.exe` makes it save and exit.
  - Sessions opened before an update keep working, and get the new version's tools from their next call.
  - Seek's installer stops the running daemon before it installs, whatever folder that one runs from.
- **If the daemon can't be reached,** the call runs inside the session's own `seek.exe`.
- **Saving.** What it saw change is saved every 5 minutes [activity_save_min]. After a restart, it reads the drives' change journals to catch what changed while no daemon watched.
