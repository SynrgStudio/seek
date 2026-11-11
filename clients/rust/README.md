# seek-client

[Seek](https://github.com/SynrgStudio/seek) for any Windows program, in Rust:

- files and folders by name on every drive, in milliseconds;
- the text in a folder's files;
- what the user changed or opened lately.

```toml
[dependencies]
seek-client = "1"
```

```rust
use seek_client::Seek;

let seek = Seek::connect()?;                                   // starts Seek when it isn't running
let found = seek.find("report .pdf").max(100).run()?;
for entry in &found.entries {
    println!("{}", entry.path.display());
}

let hits = seek.grep("TODO", r"C:\code\myapp").glob(["*.rs"]).run()?;
let lately = seek.recent().path(r"C:\notes").since("3d").walk(false).run()?;
seek.opened(r"C:\notes\plan.md")?;                             // it ranks as used from now on
```

## How it works

This crate is a client, as a database driver is: it doesn't hold an index of its own. It talks to Seek's background process through a named pipe.

- **One index for everyone.** Every program and AI agent on the PC shares one index of every drive. The drive watchers keep it current, even while your program is closed.
- **The cost.** Your program pays about 0.1 ms per request on the pipe. The search itself takes 20 to 50 ms on millions of names.
- **No dependencies.** Nothing but the standard library.
- **It never waits forever.** Each reply has a time limit, as long as its command can take (PROTOCOL.md, "Timeouts"); past it the request fails with `Error::TimedOut`, and the next one opens a new connection. `Builder::timeout` sets one limit for all.

`Seek::connect` follows Seek's rule for starting it:

- If Seek is running, it uses it.
- If not, it starts the installed `seek.exe`, or the copy your program carries ([`Builder::seek_exe`](https://docs.rs/seek-client)), whichever is newer.
- If your copy is newer than the Seek running, it replaces it.

The background process it starts inherits none of your program's handles and outlives it.

If Seek isn't installed and your program doesn't carry it, `connect` answers `Error::NotInstalled`. Installing Seek is one line of PowerShell: see [Seek's README](https://github.com/SynrgStudio/seek#install).

## Commands

| | |
|---|---|
| `find(query)` | files and folders by name, best first. The query is Seek's: `.png`, `"as typed"`, `src/name`, `!word`, `!folder/`, `*` and `?`. `.path()` for only below a folder, `.near()` to rank a folder first |
| `grep(pattern, path)` | matching lines with their line and column. Follows `.gitignore`, and reads PDF and Office documents |
| `recent()` | created, changed, renamed or opened lately, with the time |
| `status()` | each drive's index: loading, being made (with the names so far), ready |
| `opened(file)` | tell Seek your program opened a file |
| `hello()`, `stop()` | what is running; make room for a newer Seek |

Errors from Seek carry a [`Code`](https://docs.rs/seek-client): `NotFound`, `BadArgs`, `IndexLoading`… The protocol underneath is [PROTOCOL.md](https://github.com/SynrgStudio/seek/blob/main/PROTOCOL.md), version 1. This crate's version follows the protocol: `1.x` speaks protocol 1, with any Seek from 0.5.0 on.

## Tests

`cargo test` runs against `build\seek.exe` of this repository (or `SEEK_EXE`), as a Seek instance of its own in `%TEMP%`, so the one your agents use is left alone. With `SEEK_OLD_EXE` set to a `seek.exe` before 0.5.0, replacing an older Seek is checked too.

Windows only. On other systems the crate builds, and `connect` answers `Error::Unsupported`.
