//! The pipe to Seek's background process, and starting it (PROTOCOL.md, "Transport" and "Starting Seek").

use std::fs::File;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// A Seek version, "0.5.0" as (0, 5, 0).
pub(crate) type Version = (u32, u32, u32);

pub(crate) fn parse_version(s: &str) -> Option<Version> {
    let mut it = s.trim().split(|c: char| !c.is_ascii_digit()).filter(|p| !p.is_empty()).map(|p| p.parse::<u32>());
    Some((it.next()?.ok()?, it.next()?.ok()?, it.next()?.ok()?))
}

/// The pipe's name: `\\.\pipe\seek-<user>`, and for an instance with a home of its own (SEEK_HOME, the tests), a hash
/// of that folder after it, as Seek names it.
pub(crate) fn pipe_name(home: Option<&Path>) -> String {
    let mut name = format!(r"\\.\pipe\seek-{}", user_name());
    if let Some(home) = home {
        let mut key = home.to_string_lossy().replace('/', "\\");
        while key.ends_with('\\') {
            key.pop();
        }
        let mut h: u32 = 2166136261;
        for unit in key.to_lowercase().encode_utf16() {
            h = (h ^ unit as u32).wrapping_mul(16777619);
        }
        name.push_str(&format!("-{h:08x}"));
    }
    name
}

#[cfg(windows)]
fn user_name() -> String {
    #[link(name = "advapi32")]
    extern "system" {
        fn GetUserNameW(buffer: *mut u16, size: *mut u32) -> i32;
    }
    let mut buf = [0u16; 257];
    let mut n = buf.len() as u32;
    // SAFETY: buf is valid for n units, and n says how many.
    if unsafe { GetUserNameW(buf.as_mut_ptr(), &mut n) } != 0 && n > 0 {
        return String::from_utf16_lossy(&buf[..n as usize - 1]);
    }
    std::env::var("USERNAME").unwrap_or_default()
}

#[cfg(not(windows))]
fn user_name() -> String {
    String::new()
}

/// Opens the pipe. While every instance is busy (another call connected a moment ago), it tries again for a while.
pub(crate) fn open(name: &str) -> io::Result<File> {
    let started = Instant::now();
    loop {
        match std::fs::OpenOptions::new().read(true).write(true).open(name) {
            Ok(f) => return Ok(f),
            Err(e) if e.raw_os_error() == Some(231) && started.elapsed() < Duration::from_secs(5) => std::thread::sleep(Duration::from_millis(10)),
            Err(e) => return Err(e),
        }
    }
}

/// Whether the pipe is there at all.
pub(crate) fn exists(name: &str) -> bool {
    match open(name) {
        Ok(_) => true,
        Err(e) => e.raw_os_error() == Some(231),
    }
}

/// One request and its reply: a 4-byte little-endian length and that many bytes, each way.
pub(crate) fn exchange(f: &mut File, request: &[u8]) -> io::Result<Vec<u8>> {
    let mut msg = Vec::with_capacity(request.len() + 4);
    msg.extend_from_slice(&(request.len() as u32).to_le_bytes());
    msg.extend_from_slice(request);
    f.write_all(&msg)?;
    let mut len = [0u8; 4];
    f.read_exact(&mut len)?;
    let mut reply = vec![0u8; u32::from_le_bytes(len) as usize];
    f.read_exact(&mut reply)?;
    Ok(reply)
}

/// The version a seek.exe says it is (`seek.exe --version` prints "seek X.Y.Z").
pub(crate) fn exe_version(exe: &Path) -> Option<Version> {
    if !exe.is_file() {
        return None;
    }
    let mut cmd = std::process::Command::new(exe);
    cmd.arg("--version");
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let out = cmd.output().ok()?;
    parse_version(&String::from_utf8_lossy(&out.stdout))
}

/// Starts `seek.exe --daemon`, detached and out of this program's job, so that it outlives it.
///
/// Through CreateProcessW itself, inheriting no handle: std's Command lets the child inherit every inheritable handle
/// of this program, and the background process, which runs for hours, would keep this program's pipes and files open
/// (a console's output, a log), as Seek's own seek.exe never does.
#[cfg(windows)]
pub(crate) fn start(exe: &Path, home: Option<&Path>) -> io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    #[repr(C)]
    struct StartupInfo {
        cb: u32,
        reserved: *mut u16,
        desktop: *mut u16,
        title: *mut u16,
        x: u32,
        y: u32,
        x_size: u32,
        y_size: u32,
        x_chars: u32,
        y_chars: u32,
        fill: u32,
        flags: u32,
        show: u16,
        reserved2_len: u16,
        reserved2: *mut u8,
        input: *mut core::ffi::c_void,
        output: *mut core::ffi::c_void,
        error: *mut core::ffi::c_void,
    }
    #[repr(C)]
    struct ProcessInfo {
        process: *mut core::ffi::c_void,
        thread: *mut core::ffi::c_void,
        pid: u32,
        tid: u32,
    }
    #[link(name = "kernel32")]
    extern "system" {
        fn CreateProcessW(app: *const u16, cmd: *mut u16, pa: *const core::ffi::c_void, ta: *const core::ffi::c_void, inherit: i32, flags: u32,
                          env: *const core::ffi::c_void, dir: *const u16, si: *mut StartupInfo, pi: *mut ProcessInfo) -> i32;
        fn CloseHandle(h: *mut core::ffi::c_void) -> i32;
    }
    // the environment, as this program has it: with SEEK_HOME set for a home of its own, and without it otherwise
    let mut vars: Vec<(std::ffi::OsString, std::ffi::OsString)> = std::env::vars_os().filter(|(k, _)| !k.eq_ignore_ascii_case("SEEK_HOME")).collect();
    if let Some(h) = home {
        vars.push(("SEEK_HOME".into(), h.as_os_str().to_owned()));
    }
    vars.sort_by_key(|(k, _)| k.to_ascii_uppercase());
    let mut env: Vec<u16> = Vec::new();
    for (k, v) in &vars {
        env.extend(k.encode_wide());
        env.push(u16::from(b'='));
        env.extend(v.encode_wide());
        env.push(0);
    }
    env.push(0);
    // "\\?\C:\..." (from canonicalize) as "C:\...": Seek reports the path it runs from, and that is the one people know
    let plain = exe.to_string_lossy();
    let plain = match plain.strip_prefix(r"\\?\") {
        Some(rest) if rest.as_bytes().get(1) == Some(&b':') => rest.to_string(),
        _ => plain.into_owned(),
    };
    let exe = Path::new(&plain);
    let app: Vec<u16> = exe.as_os_str().encode_wide().chain([0]).collect();
    const DETACHED: u32 = 0x0000_0008;
    const NEW_GROUP: u32 = 0x0000_0200;
    const UNICODE_ENV: u32 = 0x0000_0400;
    const BREAKAWAY: u32 = 0x0100_0000;
    for flags in [DETACHED | NEW_GROUP | UNICODE_ENV | BREAKAWAY, DETACHED | NEW_GROUP | UNICODE_ENV] {
        let mut cmd: Vec<u16> = format!("\"{}\" --daemon", exe.display()).encode_utf16().chain([0]).collect();
        // SAFETY: all-zero is a valid StartupInfo and ProcessInfo (null pointers, zero sizes).
        let mut si: StartupInfo = unsafe { std::mem::zeroed() };
        si.cb = std::mem::size_of::<StartupInfo>() as u32;
        let mut pi: ProcessInfo = unsafe { std::mem::zeroed() };
        // SAFETY: every pointer is to a live, NUL-terminated buffer of this function, or null; no handle is inherited.
        let ok = unsafe {
            CreateProcessW(app.as_ptr(), cmd.as_mut_ptr(), std::ptr::null(), std::ptr::null(), 0, flags, env.as_ptr().cast(), std::ptr::null(), &mut si, &mut pi)
        };
        if ok != 0 {
            // SAFETY: handles CreateProcessW just gave.
            unsafe {
                CloseHandle(pi.thread);
                CloseHandle(pi.process);
            }
            return Ok(());
        }
    }
    Err(io::Error::last_os_error())
}

#[cfg(not(windows))]
pub(crate) fn start(_exe: &Path, _home: Option<&Path>) -> io::Result<()> {
    Err(io::Error::new(io::ErrorKind::Unsupported, "Seek runs only on Windows"))
}

/// Waits until the pipe is there (true) or the time is up.
pub(crate) fn wait_for(name: &str, limit: Duration) -> bool {
    let started = Instant::now();
    while started.elapsed() < limit {
        if exists(name) {
            return true;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    false
}

/// Waits until the pipe is gone (the background process exited), for up to five seconds.
pub(crate) fn wait_gone(name: &str) {
    let started = Instant::now();
    while exists(name) && started.elapsed() < Duration::from_secs(5) {
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// Where the Seek installer puts seek.exe.
pub(crate) fn installed_exe() -> Option<PathBuf> {
    std::env::var_os("LOCALAPPDATA").map(|d| PathBuf::from(d).join(r"Programs\Seek\seek.exe"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions() {
        assert_eq!(parse_version("seek 0.5.0\n"), Some((0, 5, 0)));
        assert_eq!(parse_version("0.4.10 Nov 11 2026 03:12:00"), Some((0, 4, 10)));
        assert!(parse_version("0.4.10").unwrap() > parse_version("0.4.9").unwrap());
        assert_eq!(parse_version("seek"), None);
    }

    #[test]
    fn a_home_of_its_own_names_its_pipe_as_seek_does() {
        // the same hash as tests/test.py's pipe_call and Seek's InstanceTag
        let name = pipe_name(Some(Path::new(r"C:\Temp\Seek-Home\")));
        let mut h: u32 = 2166136261;
        for c in r"c:\temp\seek-home".encode_utf16() {
            h = (h ^ c as u32).wrapping_mul(16777619);
        }
        assert!(name.ends_with(&format!("-{h:08x}")), "{name}");
        assert!(name.starts_with(r"\\.\pipe\seek-"));
    }
}
