//! With the `bundled` feature: the seek.exe of one Seek release, checked against its hash, for the crate to carry.
//!
//! It is downloaded from the release (curl.exe, which Windows has had since 2018, or else PowerShell), or read from the
//! file SEEK_CLIENT_EXE names, for a build without network. Either way its SHA-256 must be the one below: a seek.exe
//! that isn't that release's is never built in.

use std::path::{Path, PathBuf};
use std::process::Command;
use std::{env, fs};

#[path = "src/sha256.rs"]
mod sha256;

/// The Seek release the `bundled` feature carries, and the SHA-256 of its seek.exe (its SHA256SUMS.txt).
const SEEK_VERSION: &str = "0.5.1";
const SEEK_SHA256: &str = "fad4fce7e6d1ab6bd87ccff5eb2cec4e1252a26bb89a0d23e7207259fce3b8d9";

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=SEEK_CLIENT_EXE");
    println!("cargo:rustc-env=SEEK_CLIENT_BUNDLED_VERSION={SEEK_VERSION}");
    if env::var_os("CARGO_FEATURE_BUNDLED").is_none() {
        return;
    }
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("seek.exe");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        // Seek runs only on Windows; elsewhere the crate builds (docs.rs), with nothing inside, and never starts it.
        fs::write(&out, b"").unwrap();
        return;
    }
    if fs::read(&out).map(|b| sha256::hex(&b) == SEEK_SHA256).unwrap_or(false) {
        return; // built before
    }
    let bytes = match env::var_os("SEEK_CLIENT_EXE") {
        Some(given) => fs::read(&given).unwrap_or_else(|e| panic!("SEEK_CLIENT_EXE ({}): {e}", Path::new(&given).display())),
        None => download(&out),
    };
    let got = sha256::hex(&bytes);
    if got != SEEK_SHA256 {
        panic!("the seek.exe for the bundled feature isn't Seek {SEEK_VERSION}'s: its SHA-256 is {got}, the release's is {SEEK_SHA256}");
    }
    fs::write(&out, &bytes).unwrap();
}

fn download(out: &Path) -> Vec<u8> {
    let url = format!("https://github.com/SynrgStudio/seek/releases/download/v{SEEK_VERSION}/seek.exe");
    let part = out.with_extension("download");
    let curl = Command::new("curl.exe").args(["-sSfL", "--retry", "2", "-o"]).arg(&part).arg(&url).status();
    if !matches!(curl, Ok(s) if s.success()) {
        let ps = format!("$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing -Uri '{url}' -OutFile '{}'", part.display());
        let ok = Command::new("powershell").args(["-NoProfile", "-NonInteractive", "-Command", &ps]).status();
        if !matches!(ok, Ok(s) if s.success()) {
            panic!("couldn't download {url}. Without network, set SEEK_CLIENT_EXE to that seek.exe (Seek {SEEK_VERSION}'s).");
        }
    }
    let bytes = fs::read(&part).unwrap();
    let _ = fs::remove_file(&part);
    bytes
}
