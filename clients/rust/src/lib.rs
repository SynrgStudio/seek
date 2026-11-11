//! Seek for any Windows program: files and folders by name on every drive in milliseconds, the text in a folder's
//! files, and what the user changed lately.
//!
//! This crate is a client. It talks to Seek's background process through its named pipe, so every program and AI
//! agent on the PC shares one index of every drive, kept current by the drive watchers, and still running after the
//! program closes. A request costs about 0.1 ms on the pipe; the search itself takes 20 to 50 ms on millions of
//! names. When Seek isn't running, [`Seek::connect`] starts it, and when it isn't installed, a program can bring its
//! own copy ([`Builder::seek_exe`]).
//!
//! ```no_run
//! let seek = seek_client::Seek::connect()?;
//! let found = seek.find("report .pdf").max(100).run()?;
//! for entry in &found.entries {
//!     println!("{}", entry.path.display());
//! }
//! # Ok::<(), seek_client::Error>(())
//! ```
//!
//! The protocol underneath is Seek's [PROTOCOL.md](https://github.com/SynrgStudio/seek/blob/main/PROTOCOL.md),
//! version 1. Windows only: elsewhere the crate builds, and [`Seek::connect`] answers [`Error::Unsupported`].

mod json;
mod pipe;
#[cfg(any(feature = "bundled", test))]
mod sha256;

use std::fmt;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use json::Value;
use pipe::Version;

/// The protocol version this crate speaks.
pub const PROTOCOL: u32 = 1;

/// The Seek release this build carries, with the `bundled` feature: "0.5.1". `None` without it.
#[cfg(feature = "bundled")]
pub const BUNDLED_SEEK: Option<&str> = Some(env!("SEEK_CLIENT_BUNDLED_VERSION"));
/// The Seek release this build carries, with the `bundled` feature: "0.5.1". `None` without it.
#[cfg(not(feature = "bundled"))]
pub const BUNDLED_SEEK: Option<&str> = None;

/// The seek.exe the `bundled` feature carries, checked by build.rs against its release's SHA-256.
#[cfg(feature = "bundled")]
static BUNDLED_EXE: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/seek.exe"));

/// A seek.exe to start: a file there is, or the one this build carries, written out only when it is the one started.
#[derive(Debug, Clone)]
enum Exe {
    File(PathBuf),
    #[cfg_attr(not(feature = "bundled"), allow(dead_code))]
    Bundled,
}

// ---- errors

/// What went wrong.
#[derive(Debug)]
#[non_exhaustive]
pub enum Error {
    /// Not Windows: Seek runs only there.
    Unsupported,
    /// Seek isn't running, and there is no seek.exe to start: neither installed nor given with [`Builder::seek_exe`].
    NotInstalled,
    /// seek.exe was started but its pipe didn't appear in time.
    DidNotStart,
    /// Seek didn't answer within the time its command can take (PROTOCOL.md, "Timeouts"). The connection is dropped;
    /// the next request opens a new one.
    TimedOut,
    /// The Seek running is older than protocol 1 (before 0.5.0), and there is no newer seek.exe to start instead.
    TooOld {
        /// Its version.
        version: String,
    },
    /// Seek answered with an error.
    Seek {
        /// Its kind.
        code: Code,
        /// What it said, for a person.
        message: String,
    },
    /// Talking to the pipe failed.
    Io(std::io::Error),
    /// A reply that doesn't follow the protocol.
    Protocol(String),
}

/// The kind of an error Seek answered with (PROTOCOL.md, "Errors").
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum Code {
    /// An argument is missing or wrong, or a regex doesn't parse.
    BadArgs,
    /// The path given isn't there.
    NotFound,
    /// A file was given where a folder is needed.
    NotAFolder,
    /// The path is in a place Seek never looks.
    Private,
    /// The indexes are loading, right after Seek started: try again in a moment.
    IndexLoading,
    /// The command doesn't exist in the Seek running.
    UnknownCommand,
    /// The Seek running speaks an older protocol than this crate asked for.
    ApiTooNew,
    /// The request wasn't JSON.
    BadRequest,
    /// Anything else, and codes newer than this crate.
    Failed(String),
}

impl Code {
    fn from(code: &str) -> Code {
        match code {
            "bad_args" => Code::BadArgs,
            "not_found" => Code::NotFound,
            "not_a_folder" => Code::NotAFolder,
            "private" => Code::Private,
            "index_loading" => Code::IndexLoading,
            "unknown_command" => Code::UnknownCommand,
            "api_too_new" => Code::ApiTooNew,
            "bad_request" => Code::BadRequest,
            other => Code::Failed(other.to_string()),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Unsupported => write!(f, "Seek runs only on Windows"),
            Error::NotInstalled => write!(f, "Seek isn't running, and there is no seek.exe to start"),
            Error::DidNotStart => write!(f, "seek.exe was started, but its pipe didn't appear"),
            Error::TimedOut => write!(f, "Seek didn't answer in time"),
            Error::TooOld { version } => write!(f, "the Seek running ({version}) is older than 0.5.0, the first with protocol 1"),
            Error::Seek { message, .. } => write!(f, "{message}"),
            Error::Io(e) => write!(f, "talking to Seek: {e}"),
            Error::Protocol(s) => write!(f, "a reply that doesn't follow Seek's protocol: {s}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e)
    }
}

/// A result of this crate.
pub type Result<T> = std::result::Result<T, Error>;

// ---- connecting

/// How to connect: who asks, which seek.exe to start, and where. [`Seek::connect`] uses the defaults.
#[derive(Debug, Clone)]
pub struct Builder {
    client: String,
    installed: Option<PathBuf>,
    own: Option<PathBuf>,
    home: Option<PathBuf>,
    start: bool,
    wait: Duration,
    timeout: Option<Duration>,
    bundled_dir: Option<PathBuf>,
}

impl Default for Builder {
    fn default() -> Self {
        Builder {
            client: format!("seek-client {}", env!("CARGO_PKG_VERSION")),
            installed: pipe::installed_exe(),
            own: None,
            home: std::env::var_os("SEEK_HOME").filter(|h| !h.is_empty()).map(PathBuf::from),
            start: true,
            wait: Duration::from_secs(15),
            timeout: None,
            bundled_dir: std::env::var_os("LOCALAPPDATA").map(|d| PathBuf::from(d).join(r"seek-client\seek")),
        }
    }
}

impl Builder {
    /// Who asks, as Seek's log and status may show it: "myapp 1.2".
    pub fn client(mut self, name: impl Into<String>) -> Self {
        self.client = name.into();
        self
    }
    /// A seek.exe this program carries. It is started when it is newer than the installed one, or when Seek isn't
    /// installed; and when a Seek older than it runs, that one is stopped and this one started.
    pub fn seek_exe(mut self, path: impl Into<PathBuf>) -> Self {
        self.own = Some(path.into());
        self
    }
    /// Where the installed seek.exe is; `None` to never use it. By default `%LOCALAPPDATA%\Programs\Seek\seek.exe`.
    pub fn installed_exe(mut self, path: Option<PathBuf>) -> Self {
        self.installed = path;
        self
    }
    /// A separate instance of Seek, with its data in this folder and a pipe of its own: for tests. By default the
    /// `SEEK_HOME` variable, which is normally not set.
    pub fn home(mut self, folder: Option<PathBuf>) -> Self {
        self.home = folder;
        self
    }
    /// Never start Seek: fail with [`Error::NotInstalled`] when it isn't running.
    pub fn no_start(mut self) -> Self {
        self.start = false;
        self
    }
    /// How long to wait for a Seek just started. 15 seconds by default.
    pub fn wait(mut self, limit: Duration) -> Self {
        self.wait = limit;
        self
    }
    /// How long to wait for any reply. By default each command waits as long as it can take (PROTOCOL.md,
    /// "Timeouts"): 5 s for hello, status and opened, 45 s for find and recent, and for grep three times its time
    /// budget and 15 s.
    pub fn timeout(mut self, limit: Duration) -> Self {
        self.timeout = Some(limit);
        self
    }
    /// With the `bundled` feature: where the seek.exe it carries is written when it is the one to start, as
    /// `%LOCALAPPDATA%\<name>\seek\seek-<version>.exe`. By default `<name>` is `seek-client`, shared by every program
    /// that carries the same Seek.
    pub fn app(mut self, name: &str) -> Self {
        self.bundled_dir = std::env::var_os("LOCALAPPDATA").map(|d| PathBuf::from(d).join(name).join("seek"));
        self
    }
    /// With the `bundled` feature: the folder where the seek.exe it carries is written, instead of [`app`](Self::app)'s.
    pub fn bundled_dir(mut self, folder: impl Into<PathBuf>) -> Self {
        self.bundled_dir = Some(folder.into());
        self
    }
    /// Connects, starting Seek when needed (PROTOCOL.md, "Starting Seek").
    pub fn connect(self) -> Result<Seek> {
        if !cfg!(windows) {
            return Err(Error::Unsupported);
        }
        let seek = Seek { pipe: pipe::pipe_name(self.home.as_deref()), b: self, conn: Mutex::new(None) };
        {
            let mut conn = seek.conn.lock().unwrap_or_else(|e| e.into_inner());
            seek.ensure(&mut conn)?;
        }
        Ok(seek)
    }
}

/// A connection to Seek. Requests through one `Seek` are served one after another; a program that searches from
/// several threads at once makes one `Seek` for each.
#[derive(Debug)]
pub struct Seek {
    b: Builder,
    pipe: String,
    conn: Mutex<Option<pipe::Conn>>,
}

impl Seek {
    /// Connects with the defaults: the installed Seek, started when it isn't running.
    pub fn connect() -> Result<Seek> {
        Builder::default().connect()
    }

    /// To set how to connect.
    pub fn builder() -> Builder {
        Builder::default()
    }

    /// The newest seek.exe this program knows of, and its version: the installed one, the program's own, and the one
    /// this build carries, in that order at the same version.
    fn best_exe(&self) -> Option<(Exe, Version)> {
        let mut best: Option<(Exe, Version)> = None;
        for p in [self.b.installed.clone(), self.b.own.clone()].into_iter().flatten() {
            if let Some(v) = pipe::exe_version(&p) {
                if best.as_ref().map_or(true, |(_, bv)| v > *bv) {
                    best = Some((Exe::File(p), v));
                }
            }
        }
        #[cfg(feature = "bundled")]
        if let Some(v) = BUNDLED_SEEK.and_then(pipe::parse_version) {
            if self.b.bundled_dir.is_some() && !BUNDLED_EXE.is_empty() && best.as_ref().map_or(true, |(_, bv)| v > *bv) {
                best = Some((Exe::Bundled, v));
            }
        }
        best
    }

    /// The file to start: the one this build carries is written out first, once.
    fn exe_path(&self, exe: &Exe) -> Result<PathBuf> {
        match exe {
            Exe::File(p) => Ok(p.clone()),
            #[cfg(feature = "bundled")]
            Exe::Bundled => {
                let dir = self.b.bundled_dir.clone().ok_or(Error::NotInstalled)?;
                Ok(write_bundled(&dir)?)
            }
            #[cfg(not(feature = "bundled"))]
            Exe::Bundled => Err(Error::NotInstalled),
        }
    }

    /// Makes sure there is a connection, to the newest Seek this program knows of.
    fn ensure(&self, conn: &mut Option<pipe::Conn>) -> Result<()> {
        if conn.is_some() {
            return Ok(());
        }
        let mut best: Option<Option<(Exe, Version)>> = None;  // looked for once, when needed
        for attempt in 0..3 {
            match pipe::open(&self.pipe) {
                Ok(f) => {
                    *conn = Some(f);
                    // A Seek before protocol 1 doesn't know hello: its reply is an error without "api", with its version.
                    let hello = self.reply_on(conn, "hello", &Args::new())?;
                    if hello.num("api").is_none() {
                        return self.too_old(conn, hello.str("version"), attempt, &mut best);
                    }
                    let running = hello.str("version").and_then(pipe::parse_version);
                    // a newer seek.exe at hand replaces an older one running, at most once (an agent may start another)
                    if attempt == 0 && self.b.start {
                        let b = best.get_or_insert_with(|| self.best_exe());
                        if let (Some((exe, v)), Some(r)) = (b.clone(), running) {
                            if v > r {
                                let _ = self.ask_on(conn, "stop", &Args::new());
                                *conn = None;
                                pipe::wait_gone(&self.pipe);
                                pipe::start(&self.exe_path(&exe)?, self.b.home.as_deref())?;
                                if !pipe::wait_for(&self.pipe, self.b.wait) {
                                    return Err(Error::DidNotStart);
                                }
                                continue;
                            }
                        }
                    }
                    return Ok(());
                }
                Err(_) if attempt < 2 => {
                    if !self.b.start {
                        return Err(Error::NotInstalled);
                    }
                    let (exe, _) = best.get_or_insert_with(|| self.best_exe()).clone().ok_or(Error::NotInstalled)?;
                    pipe::start(&self.exe_path(&exe)?, self.b.home.as_deref())?;
                    if !pipe::wait_for(&self.pipe, self.b.wait) {
                        return Err(Error::DidNotStart);
                    }
                }
                Err(e) => return Err(Error::Io(e)),
            }
        }
        Err(Error::DidNotStart)
    }

    /// A Seek before protocol 1 is running: replaced by a newer seek.exe when there is one, an error otherwise.
    fn too_old(&self, conn: &mut Option<pipe::Conn>, version: Option<&str>, attempt: usize, best: &mut Option<Option<(Exe, Version)>>) -> Result<()> {
        let version = version.unwrap_or("unknown").to_string();
        let b = best.get_or_insert_with(|| self.best_exe()).clone();
        match b {
            Some((exe, v)) if attempt == 0 && self.b.start && v >= (0, 5, 0) => {
                let _ = self.ask_on(conn, "__stop", &Args::new());  // the name older Seeks know
                *conn = None;
                pipe::wait_gone(&self.pipe);
                pipe::start(&self.exe_path(&exe)?, self.b.home.as_deref())?;
                if !pipe::wait_for(&self.pipe, self.b.wait) {
                    return Err(Error::DidNotStart);
                }
                self.ensure(conn)
            }
            _ => {
                *conn = None;
                Err(Error::TooOld { version })
            }
        }
    }

    /// One request on the connection there is: the reply, or Seek's error.
    fn ask_on(&self, conn: &mut Option<pipe::Conn>, tool: &str, args: &Args) -> Result<Value> {
        let v = self.reply_on(conn, tool, args)?;
        if v.bool("error") == Some(true) {
            return Err(Error::Seek {
                code: Code::from(v.str("code").unwrap_or("failed")),
                message: v.str("text").unwrap_or_default().to_string(),
            });
        }
        Ok(v)
    }

    /// One request on the connection there is: the reply as it came, an error in it included.
    fn reply_on(&self, conn: &mut Option<pipe::Conn>, tool: &str, args: &Args) -> Result<Value> {
        let f = conn.as_mut().ok_or_else(|| Error::Protocol("not connected".into()))?;
        let mut req = format!("{{\"api\":{PROTOCOL},\"build\":");
        json::quote(&format!("0.0.0 {}", self.b.client), &mut req);
        req.push_str(",\"tool\":");
        json::quote(tool, &mut req);
        req.push_str(",\"args\":");
        args.write(&mut req);
        req.push('}');
        let reply = pipe::exchange(f, req.as_bytes(), self.b.timeout.unwrap_or_else(|| args.limit(tool))).map_err(|e| {
            *conn = None; // half a reply may still come: this connection can't be trusted any more
            if e.kind() == std::io::ErrorKind::TimedOut {
                Error::TimedOut
            } else {
                Error::Io(e)
            }
        })?;
        let text = String::from_utf8(reply).map_err(|_| Error::Protocol("not UTF-8".into()))?;
        json::parse(&text).ok_or_else(|| Error::Protocol(text.chars().take(200).collect()))
    }

    /// One request, connecting first when needed; once again on a new connection when the old one broke (Seek was
    /// updated, or restarted).
    fn ask(&self, tool: &str, args: &Args) -> Result<Value> {
        let mut conn = self.conn.lock().unwrap_or_else(|e| e.into_inner());
        for attempt in 0..2 {
            self.ensure(&mut conn)?;
            match self.ask_on(&mut conn, tool, args) {
                Err(Error::Io(_)) if attempt == 0 => continue,
                other => return other,
            }
        }
        unreachable!()
    }

    fn rows(&self, tool: &str, args: &Args) -> Result<(Vec<String>, Vec<Vec<String>>)> {
        let v = self.ask(tool, args)?;
        let text = v.str("text").ok_or_else(|| Error::Protocol(format!("{tool}: no text")))?;
        let mut lines = text.split('\n');
        let head = lines.next().unwrap_or_default();
        if !head.starts_with("#rows") {
            return Err(Error::Protocol(format!("{tool}: {head}")));
        }
        let head = head.split('\t').skip(1).map(str::to_string).collect();
        let rows = lines.filter(|l| !l.is_empty()).map(|l| l.split('\t').map(str::to_string).collect()).collect();
        Ok((head, rows))
    }

    // ---- the commands

    /// What Seek is running: its version, the protocol, its seek.exe and its data folder.
    pub fn hello(&self) -> Result<Hello> {
        let v = self.ask("hello", &Args::new())?;
        Ok(Hello {
            version: v.str("version").unwrap_or_default().to_string(),
            build: v.str("build").unwrap_or_default().to_string(),
            api: v.num("api").unwrap_or(0.0) as u32,
            apis: match v.get("apis") {
                Some(Value::Arr(a)) => a.iter().filter_map(|x| if let Value::Num(n) = x { Some(*n as u32) } else { None }).collect(),
                _ => Vec::new(),
            },
            exe: PathBuf::from(v.str("exe").unwrap_or_default()),
            data: PathBuf::from(v.str("data").unwrap_or_default()),
            pid: v.num("pid").unwrap_or(0.0) as u32,
            ready: v.bool("ready").unwrap_or(false),
        })
    }

    /// Files and folders by name, on every fixed drive or below a folder, best first.
    pub fn find(&self, query: impl Into<String>) -> FindRequest<'_> {
        FindRequest { seek: self, args: Args::new().str("query", query.into()) }
    }

    /// The lines with some text in the files below a folder, or in one file.
    pub fn grep(&self, pattern: impl Into<String>, path: impl AsRef<Path>) -> GrepRequest<'_> {
        GrepRequest { seek: self, args: Args::new().str("pattern", pattern.into()).path("path", path.as_ref()) }
    }

    /// What the user created, changed, renamed or opened lately, newest first.
    pub fn recent(&self) -> RecentRequest<'_> {
        RecentRequest { seek: self, args: Args::new() }
    }

    /// Each fixed drive's index: to show while a first one is being made.
    pub fn status(&self) -> Result<Status> {
        let (head, rows) = self.rows("status.rows", &Args::new())?;
        let drives = rows
            .iter()
            .filter(|r| r.len() >= 5)
            .map(|r| Drive {
                letter: r[0].chars().next().unwrap_or('?'),
                state: match r[1].as_str() {
                    "loading" => DriveState::Loading,
                    "indexing" => DriveState::Indexing,
                    "ready" => DriveState::Ready,
                    "none" => DriveState::None,
                    other => DriveState::Other(other.to_string()),
                },
                names: r[2].parse().unwrap_or(0),
                source: if r[3] == "jolt" { IndexSource::Jolt } else { IndexSource::Own },
                taken: r[4].parse::<u64>().ok().filter(|&t| t > 0).map(|t| UNIX_EPOCH + Duration::from_secs(t)),
            })
            .collect();
        Ok(Status { version: head.first().cloned().unwrap_or_default(), drives })
    }

    /// Tells Seek this program opened a file: it then ranks as used in [`find`](Seek::find), and
    /// [`recent`](Seek::recent) lists it as opened.
    pub fn opened(&self, file: impl AsRef<Path>) -> Result<()> {
        self.ask("opened", &Args::new().path("path", file.as_ref())).map(|_| ())
    }

    /// Stops Seek's background process. Only to replace it with a newer one: other programs' calls in flight fail.
    pub fn stop(&self) -> Result<()> {
        let mut conn = self.conn.lock().unwrap_or_else(|e| e.into_inner());
        self.ensure(&mut conn)?;
        let r = self.ask_on(&mut conn, "stop", &Args::new());
        *conn = None;
        pipe::wait_gone(&self.pipe);
        r.map(|_| ())
    }
}

/// Writes the seek.exe this build carries as `<dir>\seek-<version>.exe`, unless it is there already (the same bytes),
/// through a temporary name, so that no half-written seek.exe is ever started. The other versions there are deleted
/// when nothing runs them: a running seek.exe can't be.
#[cfg(feature = "bundled")]
fn write_bundled(dir: &Path) -> std::io::Result<PathBuf> {
    let version = BUNDLED_SEEK.unwrap_or("0");
    let path = dir.join(format!("seek-{version}.exe"));
    let same = |p: &Path| std::fs::read(p).map(|b| b.len() == BUNDLED_EXE.len() && sha256::hex(&b) == sha256::hex(BUNDLED_EXE)).unwrap_or(false);
    if !same(&path) {
        std::fs::create_dir_all(dir)?;
        let part = dir.join(format!("seek-{version}.exe.{}.part", std::process::id()));
        std::fs::write(&part, BUNDLED_EXE)?;
        if let Err(e) = std::fs::rename(&part, &path) {
            let _ = std::fs::remove_file(&part);
            if !same(&path) {
                return Err(e); // another program wrote it meanwhile, or it runs: either way it must be whole
            }
        }
    }
    if let Ok(entries) = std::fs::read_dir(dir) {
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().to_lowercase();
            if name.starts_with("seek-") && (name.ends_with(".exe") || name.ends_with(".part")) && e.path() != path {
                let _ = std::fs::remove_file(e.path());
            }
        }
    }
    Ok(path)
}

// ---- requests and their answers

/// The arguments of a request, in the order given.
#[derive(Debug, Clone, Default)]
struct Args(Vec<(&'static str, Arg)>);

#[derive(Debug, Clone)]
enum Arg {
    Str(String),
    Bool(bool),
    Num(u64),
    List(Vec<String>),
}

impl Args {
    fn new() -> Self {
        Args(Vec::new())
    }
    fn set(mut self, k: &'static str, v: Arg) -> Self {
        self.0.retain(|(key, _)| *key != k);
        self.0.push((k, v));
        self
    }
    fn str(self, k: &'static str, v: String) -> Self {
        self.set(k, Arg::Str(v))
    }
    fn path(self, k: &'static str, p: &Path) -> Self {
        self.set(k, Arg::Str(p.to_string_lossy().into_owned()))
    }
    fn bool(self, k: &'static str, v: bool) -> Self {
        self.set(k, Arg::Bool(v))
    }
    fn num(self, k: &'static str, v: u64) -> Self {
        self.set(k, Arg::Num(v))
    }
    /// How long a command can take, at most (PROTOCOL.md, "Timeouts").
    fn limit(&self, tool: &str) -> Duration {
        match tool {
            "find.rows" | "recent.rows" => Duration::from_secs(45),
            "grep.rows" => {
                let budget = self.0.iter().find_map(|(k, v)| match (k, v) {
                    (&"time_budget_ms", Arg::Num(n)) => Some((*n).clamp(500, 60_000)),
                    _ => None,
                });
                Duration::from_millis(3 * budget.unwrap_or(6_000) + 15_000)
            }
            _ => Duration::from_secs(5),
        }
    }
    fn write(&self, out: &mut String) {
        out.push('{');
        for (i, (k, v)) in self.0.iter().enumerate() {
            if i > 0 {
                out.push(',');
            }
            json::quote(k, out);
            out.push(':');
            match v {
                Arg::Str(s) => json::quote(s, out),
                Arg::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
                Arg::Num(n) => out.push_str(&n.to_string()),
                Arg::List(l) => {
                    out.push('[');
                    for (j, s) in l.iter().enumerate() {
                        if j > 0 {
                            out.push(',');
                        }
                        json::quote(s, out);
                    }
                    out.push(']');
                }
            }
        }
        out.push('}');
    }
}

/// What [`Seek::hello`] says.
#[derive(Debug, Clone)]
pub struct Hello {
    /// Seek's version, "0.5.0".
    pub version: String,
    /// Its build, "0.5.0 Nov 11 2026 03:12:00".
    pub build: String,
    /// The newest protocol version it speaks.
    pub api: u32,
    /// Every protocol version it speaks.
    pub apis: Vec<u32>,
    /// The seek.exe running.
    pub exe: PathBuf,
    /// Its data folder: the indexes and the history.
    pub data: PathBuf,
    /// Its process id.
    pub pid: u32,
    /// Whether the drive indexes are loaded (right after a start, for about a second, they aren't).
    pub ready: bool,
}

/// Files or folders only.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// Files and folders.
    Any,
    /// Files only.
    File,
    /// Folders only.
    Folder,
}

/// A search by name, to set up and [`run`](FindRequest::run).
#[must_use]
pub struct FindRequest<'a> {
    seek: &'a Seek,
    args: Args,
}

impl FindRequest<'_> {
    /// Only below this folder.
    pub fn path(mut self, folder: impl AsRef<Path>) -> Self {
        self.args = self.args.path("path", folder.as_ref());
        self
    }
    /// A folder whose surroundings rank first. A boost, not a filter: [`path`](Self::path) filters.
    pub fn near(mut self, folder: impl AsRef<Path>) -> Self {
        self.args = self.args.path("near", folder.as_ref());
        self
    }
    /// Files, folders, or both.
    pub fn kind(mut self, kind: Kind) -> Self {
        self.args = self.args.str("type", match kind {
            Kind::Any => "any",
            Kind::File => "file",
            Kind::Folder => "folder",
        }.into());
        self
    }
    /// With a path: also what `.gitignore` leaves out there, and dependency and cache folders.
    pub fn ignored(mut self, yes: bool) -> Self {
        self.args = self.args.bool("ignored", yes);
        self
    }
    /// How many at most: 1 to 100,000; 5,000 by default.
    pub fn max(mut self, n: u32) -> Self {
        self.args = self.args.num("max", u64::from(n));
        self
    }
    /// Searches.
    pub fn run(self) -> Result<Found> {
        let (head, rows) = self.seek.rows("find.rows", &self.args)?;
        Ok(Found {
            total: head.first().and_then(|t| t.parse().ok()).unwrap_or(0),
            exact: head.get(1).map_or(true, |e| e == "1"),
            kind: match head.get(2).map(String::as_str) {
                Some("near") => MatchKind::Near,
                Some("scattered") => MatchKind::Scattered,
                _ => MatchKind::Real,
            },
            entries: rows.into_iter().filter(|r| r.len() >= 2).map(|r| Entry { is_dir: r[0] == "D", path: PathBuf::from(&r[1]) }).collect(),
        })
    }
}

/// What [`Seek::find`] found.
#[derive(Debug, Clone)]
pub struct Found {
    /// How many match in all.
    pub total: u64,
    /// Whether [`total`](Self::total) is exact; past `max` it is an estimate.
    pub exact: bool,
    /// What kind of matches these are.
    pub kind: MatchKind,
    /// The best ones, best first.
    pub entries: Vec<Entry>,
}

/// What kind of matches a search found.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MatchKind {
    /// Names with the letters typed, in order and together.
    Real,
    /// No name has them: names close to what was typed (a typo).
    Near,
    /// No name has them together: these have them spread along the name.
    Scattered,
}

/// A file or folder found.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// Its full path.
    pub path: PathBuf,
    /// Whether it is a folder.
    pub is_dir: bool,
}

/// When letter case matters to [`Seek::grep`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Case {
    /// Insensitive unless the pattern has capitals (the default).
    Smart,
    /// Always.
    Sensitive,
    /// Never.
    Insensitive,
}

/// A search for text, to set up and [`run`](GrepRequest::run).
#[must_use]
pub struct GrepRequest<'a> {
    seek: &'a Seek,
    args: Args,
}

impl GrepRequest<'_> {
    /// The pattern is a regex (ECMAScript).
    pub fn regex(mut self, yes: bool) -> Self {
        self.args = self.args.bool("regex", yes);
        self
    }
    /// When letter case matters.
    pub fn case(mut self, case: Case) -> Self {
        if case != Case::Smart {
            self.args = self.args.str("case", if case == Case::Sensitive { "sensitive" } else { "insensitive" }.into());
        }
        self
    }
    /// Only files whose names match these patterns (`*.md`); one starting with `!` leaves out (`!*.min.js`).
    pub fn glob<I: IntoIterator<Item = S>, S: Into<String>>(mut self, patterns: I) -> Self {
        self.args = self.args.set("glob", Arg::List(patterns.into_iter().map(Into::into).collect()));
        self
    }
    /// Lines with this text are left out.
    pub fn without(mut self, text: impl Into<String>) -> Self {
        self.args = self.args.str("without", text.into());
        self
    }
    /// Also what `.gitignore` leaves out, and dependency and cache folders.
    pub fn ignored(mut self, yes: bool) -> Self {
        self.args = self.args.bool("ignored", yes);
        self
    }
    /// Also hidden files and folders.
    pub fn hidden(mut self, yes: bool) -> Self {
        self.args = self.args.bool("hidden", yes);
        self
    }
    /// Also PDF and Office documents (true by default).
    pub fn documents(mut self, yes: bool) -> Self {
        self.args = self.args.bool("documents", yes);
        self
    }
    /// Lines at most: 1 to 100,000; 2,000 by default.
    pub fn limit(mut self, n: u32) -> Self {
        self.args = self.args.num("limit", u64::from(n));
        self
    }
    /// Lines from one file at most: 1 to 10,000; 100 by default.
    pub fn per_file(mut self, n: u32) -> Self {
        self.args = self.args.num("per_file", u64::from(n));
        self
    }
    /// How long it may search: 500 ms to 60 s; 6 s by default.
    pub fn time_budget(mut self, limit: Duration) -> Self {
        self.args = self.args.num("time_budget_ms", limit.as_millis() as u64);
        self
    }
    /// Searches.
    pub fn run(self) -> Result<Grepped> {
        let (head, rows) = self.seek.rows("grep.rows", &self.args)?;
        Ok(Grepped {
            files: head.first().and_then(|t| t.parse().ok()).unwrap_or(0),
            lines: head.get(1).and_then(|t| t.parse().ok()).unwrap_or(0),
            cut: head.get(2).map_or(false, |c| c == "1"),
            how: match head.get(3).map(String::as_str) {
                Some("nocase") => How::IgnoringCase,
                Some("regex") => How::AsRegex,
                _ => How::Asked,
            },
            hits: rows
                .into_iter()
                .filter(|r| r.len() >= 4)
                .map(|r| Hit { path: PathBuf::from(&r[0]), line: r[1].parse().unwrap_or(0), column: r[2].parse().unwrap_or(0), text: r[3].clone() })
                .collect(),
        })
    }
}

/// What [`Seek::grep`] found.
#[derive(Debug, Clone)]
pub struct Grepped {
    /// How many files have the text.
    pub files: u64,
    /// How many lines.
    pub lines: u64,
    /// Whether not every matching line is in [`hits`](Self::hits) (limit, per_file, or the time budget).
    pub cut: bool,
    /// How it searched.
    pub how: How,
    /// The matching lines, file by file.
    pub hits: Vec<Hit>,
}

/// How [`Seek::grep`] searched.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum How {
    /// As asked.
    Asked,
    /// Nothing matched the exact case: these ignore it.
    IgnoringCase,
    /// A literal pattern with regex signs found nothing: these are its matches as a regex.
    AsRegex,
}

/// A matching line.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Hit {
    /// The file.
    pub path: PathBuf,
    /// The line, from 1.
    pub line: u32,
    /// Where the match starts in it, from 1.
    pub column: u32,
    /// The line without its indentation; a very long one around the match, with "…".
    pub text: String,
}

/// What the user did lately, to set up and [`run`](RecentRequest::run).
#[must_use]
pub struct RecentRequest<'a> {
    seek: &'a Seek,
    args: Args,
}

impl RecentRequest<'_> {
    /// Only below this folder.
    pub fn path(mut self, folder: impl AsRef<Path>) -> Self {
        self.args = self.args.path("path", folder.as_ref());
        self
    }
    /// Only names like this, as in [`Seek::find`].
    pub fn query(mut self, query: impl Into<String>) -> Self {
        self.args = self.args.str("query", query.into());
        self
    }
    /// How far back: "30m", "2h", "3d", "2w". Seven days by default.
    pub fn since(mut self, span: impl Into<String>) -> Self {
        self.args = self.args.str("since", span.into());
        self
    }
    /// With a path: also read the dates on the disk there (true by default). False answers from what Seek saw, in
    /// milliseconds.
    pub fn walk(mut self, yes: bool) -> Self {
        self.args = self.args.bool("walk", yes);
        self
    }
    /// Also system places and what `.gitignore` leaves out.
    pub fn include_system(mut self, yes: bool) -> Self {
        self.args = self.args.bool("include_system", yes);
        self
    }
    /// How many at most: 1 to 100,000; 1,000 by default.
    pub fn limit(mut self, n: u32) -> Self {
        self.args = self.args.num("limit", u64::from(n));
        self
    }
    /// Asks.
    pub fn run(self) -> Result<Recent> {
        let (head, rows) = self.seek.rows("recent.rows", &self.args)?;
        Ok(Recent {
            total: head.first().and_then(|t| t.parse().ok()).unwrap_or(0),
            exact: head.get(1).map_or(true, |e| e == "1"),
            items: rows
                .into_iter()
                .filter(|r| r.len() >= 4)
                .map(|r| Change {
                    is_dir: r[0] == "D",
                    path: PathBuf::from(&r[1]),
                    time: UNIX_EPOCH + Duration::from_secs(r[2].parse().unwrap_or(0)),
                    what: match r[3].as_str() {
                        "created" => What::Created,
                        "renamed" => What::Renamed,
                        "opened" => What::Opened,
                        _ => What::Changed,
                    },
                })
                .collect(),
        })
    }
}

/// What [`Seek::recent`] says.
#[derive(Debug, Clone)]
pub struct Recent {
    /// How many there are.
    pub total: u64,
    /// Whether that is all of them.
    pub exact: bool,
    /// Newest first.
    pub items: Vec<Change>,
}

/// Something the user did to a file or folder.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Change {
    /// Its full path.
    pub path: PathBuf,
    /// Whether it is a folder.
    pub is_dir: bool,
    /// When.
    pub time: SystemTime,
    /// What.
    pub what: What,
}

/// What happened to a file.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum What {
    /// Created.
    Created,
    /// Changed (also: its date on disk is recent).
    Changed,
    /// Renamed or moved to here.
    Renamed,
    /// Opened, by Jolt or a program that told Seek ([`Seek::opened`]).
    Opened,
}

/// What [`Seek::status`] says.
#[derive(Debug, Clone)]
pub struct Status {
    /// Seek's version.
    pub version: String,
    /// Each fixed drive.
    pub drives: Vec<Drive>,
}

/// A drive's index.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Drive {
    /// Its letter, 'C'.
    pub letter: char,
    /// Its index's state.
    pub state: DriveState,
    /// How many names; while indexing, how many so far (how many in all is known only at the end).
    pub names: u64,
    /// Whose index it is.
    pub source: IndexSource,
    /// When it was taken.
    pub taken: Option<SystemTime>,
}

/// The state of a drive's index.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum DriveState {
    /// Seek has just started and is reading its indexes: about a second.
    Loading,
    /// Its first index is being made. Searches on every drive lack its names until it ends.
    Indexing,
    /// Ready.
    Ready,
    /// No index.
    None,
    /// A state newer than this crate.
    Other(String),
}

/// Whose index a drive has.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IndexSource {
    /// Jolt's, copied.
    Jolt,
    /// Seek's own: it walked the drive.
    Own,
}
