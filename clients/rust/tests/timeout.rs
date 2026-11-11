//! A Seek that stops answering: the client gives up when its time is up, instead of waiting forever.

#![cfg(windows)]

use std::path::PathBuf;
use std::time::{Duration, Instant};

use seek_client::{Error, Seek};

type Handle = isize;
#[link(name = "kernel32")]
extern "system" {
    fn CreateNamedPipeW(name: *const u16, open: u32, mode: u32, max: u32, out: u32, inp: u32, timeout: u32, sa: *const core::ffi::c_void) -> Handle;
    fn ConnectNamedPipe(h: Handle, ov: *mut core::ffi::c_void) -> i32;
    fn ReadFile(h: Handle, buf: *mut u8, len: u32, read: *mut u32, ov: *mut core::ffi::c_void) -> i32;
    fn WriteFile(h: Handle, buf: *const u8, len: u32, written: *mut u32, ov: *mut core::ffi::c_void) -> i32;
}

/// The pipe of a Seek with this home, named as Seek names it.
fn pipe_for(home: &str) -> String {
    let mut h: u32 = 2166136261;
    for unit in home.to_lowercase().encode_utf16() {
        h = (h ^ unit as u32).wrapping_mul(16777619);
    }
    format!(r"\\.\pipe\seek-{}-{h:08x}", std::env::var("USERNAME").unwrap())
}

fn read_frame(h: Handle) -> Option<Vec<u8>> {
    let read = |buf: &mut [u8]| {
        let mut at = 0;
        while at < buf.len() {
            let mut n = 0u32;
            // SAFETY: the rest of buf, on a pipe this test made.
            if unsafe { ReadFile(h, buf[at..].as_mut_ptr(), (buf.len() - at) as u32, &mut n, std::ptr::null_mut()) } == 0 || n == 0 {
                return false;
            }
            at += n as usize;
        }
        true
    };
    let mut len = [0u8; 4];
    if !read(&mut len) {
        return None;
    }
    let mut msg = vec![0u8; u32::from_le_bytes(len) as usize];
    read(&mut msg).then_some(msg)
}

/// A pretend Seek: it answers hello, and then nothing, ever.
fn silent_seek(name: String) {
    std::thread::spawn(move || {
        let wide: Vec<u16> = name.encode_utf16().chain([0]).collect();
        loop {
            // SAFETY: a byte-mode duplex pipe with this name.
            let h = unsafe { CreateNamedPipeW(wide.as_ptr(), 3, 0, 255, 65536, 65536, 0, std::ptr::null()) };
            assert_ne!(h, -1, "CreateNamedPipeW");
            // SAFETY: the pipe just made.
            unsafe { ConnectNamedPipe(h, std::ptr::null_mut()) };
            std::thread::spawn(move || {
                while let Some(req) = read_frame(h) {
                    if String::from_utf8_lossy(&req).contains("\"tool\":\"hello\"") {
                        let reply = br#"{"api":1,"apis":[1],"version":"9.9.9","error":false}"#;
                        let mut msg = (reply.len() as u32).to_le_bytes().to_vec();
                        msg.extend_from_slice(reply);
                        let mut n = 0u32;
                        // SAFETY: msg is alive for the call.
                        unsafe { WriteFile(h, msg.as_ptr(), msg.len() as u32, &mut n, std::ptr::null_mut()) };
                    } else {
                        std::thread::sleep(Duration::from_secs(3600)); // stuck
                    }
                }
            });
        }
    });
    std::thread::sleep(Duration::from_millis(200));
}

/// A pretend Seek whose indexes are loading: find answers index_loading the first `loading` times, then rows.
fn loading_seek(name: String, loading: usize) {
    std::thread::spawn(move || {
        let wide: Vec<u16> = name.encode_utf16().chain([0]).collect();
        let asked = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        loop {
            // SAFETY: a byte-mode duplex pipe with this name.
            let h = unsafe { CreateNamedPipeW(wide.as_ptr(), 3, 0, 255, 65536, 65536, 0, std::ptr::null()) };
            assert_ne!(h, -1, "CreateNamedPipeW");
            // SAFETY: the pipe just made.
            unsafe { ConnectNamedPipe(h, std::ptr::null_mut()) };
            let asked = asked.clone();
            std::thread::spawn(move || {
                while let Some(req) = read_frame(h) {
                    let req = String::from_utf8_lossy(&req);
                    let reply: &[u8] = if req.contains("\"tool\":\"hello\"") {
                        br#"{"api":1,"apis":[1],"version":"9.9.9","error":false}"#
                    } else if asked.fetch_add(1, std::sync::atomic::Ordering::SeqCst) < loading {
                        br#"{"api":1,"version":"9.9.9","text":"The drive index is still loading.","error":true,"code":"index_loading"}"#
                    } else {
                        br##"{"api":1,"version":"9.9.9","text":"#rows\t1\t1\treal\nF\tC:\\a.txt\n","error":false}"##
                    };
                    let mut msg = (reply.len() as u32).to_le_bytes().to_vec();
                    msg.extend_from_slice(reply);
                    let mut n = 0u32;
                    // SAFETY: msg is alive for the call.
                    unsafe { WriteFile(h, msg.as_ptr(), msg.len() as u32, &mut n, std::ptr::null_mut()) };
                }
            });
        }
    });
    std::thread::sleep(Duration::from_millis(200));
}

#[test]
fn index_loading_is_waited_out() {
    let home = format!(r"C:\seek-client-loading-test-{}", std::process::id());
    loading_seek(pipe_for(&home), 3);
    let seek = Seek::builder().home(Some(PathBuf::from(&home))).installed_exe(None).no_start().connect().unwrap();
    let t = Instant::now();
    let found = seek.find("a").run().expect("answered once the indexes are loaded");
    assert_eq!(found.entries.len(), 1);
    assert!(t.elapsed() >= Duration::from_millis(550), "asked again three times, 200 ms apart: {:?}", t.elapsed());
}

#[test]
fn a_seek_that_stops_answering_times_out() {
    let home = format!(r"C:\seek-client-timeout-test-{}", std::process::id());
    silent_seek(pipe_for(&home));
    let seek = Seek::builder()
        .home(Some(PathBuf::from(&home)))
        .installed_exe(None)
        .no_start()
        .timeout(Duration::from_secs(1))
        .connect()
        .expect("hello is answered");
    let t = Instant::now();
    match seek.find("anything").run() {
        Err(Error::TimedOut) => {}
        other => panic!("{other:?}"),
    }
    let took = t.elapsed();
    assert!(took >= Duration::from_millis(900) && took < Duration::from_secs(3), "{took:?}");
    // the connection that timed out is dropped: the next request opens a new one, and hello on it works
    assert!(matches!(seek.hello(), Ok(h) if h.version == "9.9.9"));
}
