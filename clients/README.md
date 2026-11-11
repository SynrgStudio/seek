# Seek's client packages

Packages that give a program Seek in a few lines. Each one speaks [PROTOCOL.md](../PROTOCOL.md) through Seek's pipe, and follows its rule for starting Seek.

| | package | state |
|---|---|---|
| [rust](rust) | `seek-client` on crates.io | written and tested, with `bundled`; not published yet. Until it is, as a git dependency: `seek-client = { git = "https://github.com/SynrgStudio/seek", package = "seek-client" }` |
| python | `seek-client` on PyPI | planned ([#24](https://github.com/SynrgStudio/seek/issues/24)) |
| npm | `seek-client` on npm | planned ([#25](https://github.com/SynrgStudio/seek/issues/25)) |

They live here, next to Seek, so that a change to the protocol and to the clients goes in one commit, and their tests run against the `seek.exe` built from the same tree.

## Versions

- **A client's version follows the protocol, not Seek.** `1.x` speaks protocol 1, with whatever Seek release runs underneath (0.5.0 or later).
- **A new Seek release doesn't need new clients.**
- **A client that carries `seek.exe`** (the `bundled` option, [#22](https://github.com/SynrgStudio/seek/issues/22)) says which Seek it carries. A release that only changes that Seek is a patch.

## Releasing

As Seek is released: from the maintainer's PC, with the tests passing.

1. Build Seek (`build.bat`) and run its tests (`python -I tests\test.py`).
2. Run the client's tests against that build (for Rust: `cargo test` in `clients/rust`).
3. Publish (`cargo publish`, `twine upload`, `npm publish`).
4. Tag the commit as `clients/rust-v1.0.0` (and the like), so client tags don't mix with Seek's `vX.Y.Z`.

A version published on crates.io, PyPI or npm can't be taken back, only yanked or deprecated. The first publish of each package is decided by the maintainer, never done as part of other work.

## Before the first publish

- **The packages' license: decided,** `MIT OR Apache-2.0` (LICENSE-MIT and LICENSE-APACHE next to each package). A GPL client would oblige every program that links it to be GPL. Seek itself stays GPL-3.0, and runs as a separate process.
- **The name `seek-client`** was checked free on crates.io, PyPI and npm on 2026-11-11.
