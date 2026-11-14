# Seek

- Build: `build.bat` (→ `build\seek.exe`). A failed build leaves the old exe, and the tests pass on it: check the log says `Done: build\seek.exe` and `build\seek.exe --version` shows the new version. `tools/release.py` checks both.
- Test: `python -I tests\test.py` (ends with `all passed`). The Rust client: `cargo test --features bundled` in `clients/rust`.
- Release, from this PC, never from GitHub Actions: bump `src/version.h` (`SEEK_VERSION` and `SEEK_VERSION_NUM`), add `## X.Y.Z` to `CHANGELOG.md`, run `python -I tools/web_corpus.py`, commit, then `python -I tools/release.py --install`.
- The website (seek.synrgs.tech) is built by the PORTFOLIO thread from `web/`. Change `web/`, never `web/facts.toml` by hand (`tools/web_corpus.py` writes it; the tests fail when it is stale), then tell PORTFOLIO: `corpus seek actualizado · commit <sha> · cambió: ...`. `web/rules.md` says what is never published.
- The protocol for programs (`PROTOCOL.md`): every reply's shape is fixed in `tests/protocol/v1.json`. Changing one is protocol version 2, not an edit.
- A new MCP tool: its schema and the server's instructions in `src/server.inl`, its dispatch in `RunTool` (`src/tools.inl`), and the tool lists the tests expect (`tools/list`, `__tools`).
- This repository is public: no code, file names or paths from private projects. Reproduce a report with files of your own of the same shape.
