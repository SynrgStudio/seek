//! The pipe to Seek's background process, and starting it (PROTOCOL.md, "Transport" and "Starting Seek").

use std::io;
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

/// A connection to the pipe, opened for overlapped I/O: every read and write waits at most until a deadline, and is
/// cancelled then (PROTOCOL.md, "Timeouts"). std's File can't stop waiting on a pipe.
#[derive(Debug)]
pub(crate) struct Conn {
    pipe: isize,
    event: isize,
}

#[cfg(windows)]
mod win {
    pub(super) type Handle = isize;
    #[repr(C)]
    pub(super) struct Overlapped {
        pub internal: usize,
        pub internal_high: usize,
        pub offset: u32,
        pub offset_high: u32,
        pub event: Handle,
    }
    #[link(name = "kernel32")]
    extern "system" {
        pub(super) fn CreateFileW(name: *const u16, access: u32, share: u32, sa: *const core::ffi::c_void, disposition: u32, flags: u32, template: Handle) -> Handle;
        pub(super) fn CreateEventW(sa: *const core::ffi::c_void, manual: i32, initial: i32, name: *const u16) -> Handle;
        pub(super) fn ReadFile(h: Handle, buf: *mut u8, len: u32, read: *mut u32, ov: *mut Overlapped) -> i32;
        pub(super) fn WriteFile(h: Handle, buf: *const u8, len: u32, written: *mut u32, ov: *mut Overlapped) -> i32;
        pub(super) fn WaitForSingleObject(h: Handle, ms: u32) -> u32;
        pub(super) fn GetOverlappedResult(h: Handle, ov: *mut Overlapped, n: *mut u32, wait: i32) -> i32;
        pub(super) fn CancelIoEx(h: Handle, ov: *mut Overlapped) -> i32;
        pub(super) fn CloseHandle(h: Handle) -> i32;
    }
}

#[cfg(windows)]
impl Drop for Conn {
    fn drop(&mut self) {
        // SAFETY: handles this Conn opened, closed once.
        unsafe {
            win::CloseHandle(self.pipe);
            win::CloseHandle(self.event);
        }
    }
}

#[cfg(windows)]
impl Conn {
    /// One read or write, waiting at most until the deadline; cancelled and an error then.
    fn io(&mut self, write: bool, buf: *mut u8, len: u32, deadline: Instant) -> io::Result<u32> {
        use win::*;
        let mut ov = Overlapped { internal: 0, internal_high: 0, offset: 0, offset_high: 0, event: self.event };
        let mut n = 0u32;
        // SAFETY: buf is valid for len bytes until the operation is over: it is waited for below, cancelled or not.
        let started = unsafe { if write { WriteFile(self.pipe, buf, len, std::ptr::null_mut(), &mut ov) } else { ReadFile(self.pipe, buf, len, std::ptr::null_mut(), &mut ov) } };
        if started == 0 {
            let e = io::Error::last_os_error();
            if e.raw_os_error() != Some(997) && e.raw_os_error() != Some(234) {
                return Err(e); // not ERROR_IO_PENDING (nor ERROR_MORE_DATA: a message read in parts)
            }
        }
        let left = deadline.saturating_duration_since(Instant::now()).as_millis().min(u32::MAX as u128 - 1) as u32;
        // SAFETY: the event belongs to this Conn; ov lives until the operation is over.
        unsafe {
            if WaitForSingleObject(self.event, left) == 258 {
                // WAIT_TIMEOUT: cancelled, and waited for, so that nothing writes into buf afterwards
                CancelIoEx(self.pipe, &mut ov);
                GetOverlappedResult(self.pipe, &mut ov, &mut n, 1);
                return Err(io::Error::new(io::ErrorKind::TimedOut, "Seek didn't answer in time"));
            }
            if GetOverlappedResult(self.pipe, &mut ov, &mut n, 0) == 0 {
                let e = io::Error::last_os_error();
                if e.raw_os_error() != Some(234) {
                    return Err(e);
                }
            }
        }
        if n == 0 && len > 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "Seek closed the connection"));
        }
        Ok(n)
    }

    fn write_all(&mut self, mut buf: &[u8], deadline: Instant) -> io::Result<()> {
        while !buf.is_empty() {
            let n = self.io(true, buf.as_ptr() as *mut u8, buf.len().min(1 << 20) as u32, deadline)? as usize;
            buf = &buf[n..];
        }
        Ok(())
    }

    fn read_exact(&mut self, buf: &mut [u8], deadline: Instant) -> io::Result<()> {
        let mut at = 0;
        while at < buf.len() {
            at += self.io(false, buf[at..].as_mut_ptr(), (buf.len() - at).min(1 << 20) as u32, deadline)? as usize;
        }
        Ok(())
    }
}

/// Opens the pipe. While every instance is busy (another call connected a moment ago), it tries again for a while.
#[cfg(windows)]
pub(crate) fn open(name: &str) -> io::Result<Conn> {
    use win::*;
    let wide: Vec<u16> = name.encode_utf16().chain([0]).collect();
    let started = Instant::now();
    loop {
        const READ_WRITE: u32 = 0x8000_0000 | 0x4000_0000;
        const OPEN_EXISTING: u32 = 3;
        const OVERLAPPED: u32 = 0x4000_0000;
        // SAFETY: wide is NUL-terminated; no security attributes, no template.
        let pipe = unsafe { CreateFileW(wide.as_ptr(), READ_WRITE, 0, std::ptr::null(), OPEN_EXISTING, OVERLAPPED, 0) };
        if pipe != -1 {
            // SAFETY: an unnamed, manual-reset event.
            let event = unsafe { CreateEventW(std::ptr::null(), 1, 0, std::ptr::null()) };
            if event == 0 {
                let e = io::Error::last_os_error();
                // SAFETY: the pipe just opened.
                unsafe { CloseHandle(pipe) };
                return Err(e);
            }
            return Ok(Conn { pipe, event });
        }
        let e = io::Error::last_os_error();
        if e.raw_os_error() == Some(231) && started.elapsed() < Duration::from_secs(5) {
            std::thread::sleep(Duration::from_millis(10));
            continue;
        }
        return Err(e);
    }
}

#[cfg(not(windows))]
pub(crate) fn open(_name: &str) -> io::Result<Conn> {
    Err(io::Error::new(io::ErrorKind::Unsupported, "Seek runs only on Windows"))
}

/// Whether the pipe is there at all.
pub(crate) fn exists(name: &str) -> bool {
    match open(name) {
        Ok(_) => true,
        Err(e) => e.raw_os_error() == Some(231),
    }
}

/// One request and its reply: a 4-byte little-endian length and that many bytes, each way; all of it within limit.
#[cfg(windows)]
pub(crate) fn exchange(c: &mut Conn, request: &[u8], limit: Duration) -> io::Result<Vec<u8>> {
    let deadline = Instant::now() + limit;
    let mut msg = Vec::with_capacity(request.len() + 4);
    msg.extend_from_slice(&(request.len() as u32).to_le_bytes());
    msg.extend_from_slice(request);
    c.write_all(&msg, deadline)?;
    let mut len = [0u8; 4];
    c.read_exact(&mut len, deadline)?;
    let mut reply = vec![0u8; u32::from_le_bytes(len) as usize];
    c.read_exact(&mut reply, deadline)?;
    Ok(reply)
}

#[cfg(not(windows))]
pub(crate) fn exchange(_c: &mut Conn, _request: &[u8], _limit: Duration) -> io::Result<Vec<u8>> {
    Err(io::Error::new(io::ErrorKind::Unsupported, "Seek runs only on Windows"))
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
                win::CloseHandle(pi.thread as isize);
                win::CloseHandle(pi.process as isize);
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
