# Changelog

## 0.1.0

The first release, as a repository of its own. Seek began in [Jolt](https://github.com/SynrgStudio/jolt)'s repository and shares its fuzzy matching and its drive index.

- **Tools:**
  - **`find`:** names on every fixed drive in milliseconds, typo-tolerant, ranked by what the user is doing.
  - **`grep`:** text in a folder's files and in PDF and Office documents, following `.gitignore`.
  - **`recent`:** what the user changed and opened lately.
- **One background process for every agent,** reached through a named pipe only its user can open.
- **Folders kept in memory after their first search,** and kept current by the drive watchers.
- **Without Jolt,** Seek indexes the drives itself, in parallel, and walks them again once a day.
- **Private places are never searched,** and every call is written to a log the user can read.
- **MCP over stdio,** protocol versions from 2024-11-05 on, batches included.
- **The installer** downloads the release, checks its SHA-256, and adds Seek to Claude Code and Codex.
