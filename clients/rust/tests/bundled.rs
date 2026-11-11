//! The `bundled` feature against a real start: the seek.exe the crate carries is written out and started when there is
//! no other, kept when it is there, and the installed one wins at the same version. Each case is a Seek instance of its
//! own (a home in %TEMP%), so the user's isn't touched.
//!
//!     cargo test --features bundled --test bundled

#![cfg(all(windows, feature = "bundled"))]

use std::fs;
use std::path::{Path, PathBuf};

use seek_client::{Seek, BUNDLED_SEEK};

fn folder(name: &str) -> PathBuf {
    let p = std::env::temp_dir().join("seek-client-test").join(name);
    let _ = fs::remove_dir_all(&p);
    fs::create_dir_all(&p).unwrap();
    p
}

fn same(a: &Path, b: &Path) -> bool {
    a.to_string_lossy().to_lowercase() == b.to_string_lossy().to_lowercase()
}

#[test]
fn the_seek_it_carries_is_started_when_there_is_no_other() {
    let home = folder("bundled-home");
    let dir = folder("bundled-exe");
    fs::write(dir.join("seek-0.0.1.exe"), b"an older one, as the first bundled builds wrote it").unwrap();
    fs::create_dir_all(dir.join("0.0.2")).unwrap();
    fs::write(dir.join(r"0.0.2\seek.exe"), b"an older one, in its own folder").unwrap();
    let connect = || Seek::builder().home(Some(home.clone())).installed_exe(None).bundled_dir(&dir).client("seek-client tests").connect();

    let seek = connect().expect("start the bundled Seek");
    let hello = seek.hello().unwrap();
    let written = dir.join(BUNDLED_SEEK.unwrap()).join("seek.exe");
    assert_eq!(Some(hello.version.as_str()), BUNDLED_SEEK);
    assert!(same(&hello.exe, &written), "{hello:?}");
    assert!(!dir.join("seek-0.0.1.exe").exists() && !dir.join("0.0.2").exists(), "the older ones are deleted");
    // its process is seek, as an installed Seek's
    let ps = std::process::Command::new("powershell")
        .args(["-NoProfile", "-Command", &format!("(Get-Process -Id {}).ProcessName", hello.pid)])
        .output()
        .unwrap();
    assert_eq!(String::from_utf8_lossy(&ps.stdout).trim(), "seek");
    let before = fs::metadata(&written).unwrap().modified().unwrap();
    drop(seek);

    // connecting again uses the Seek running, and doesn't write the file again
    let seek = connect().unwrap();
    assert_eq!(fs::metadata(&written).unwrap().modified().unwrap(), before);
    seek.stop().unwrap();
}

#[test]
fn the_installed_seek_wins_at_the_same_version() {
    let installed = PathBuf::from(std::env::var("LOCALAPPDATA").unwrap()).join(r"Programs\Seek\seek.exe");
    let v = std::process::Command::new(&installed).arg("--version").output().map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string());
    if v.as_deref().ok() != BUNDLED_SEEK.map(|b| format!("seek {b}")).as_deref() {
        eprintln!("skipped: the installed Seek ({v:?}) isn't the one bundled ({BUNDLED_SEEK:?})");
        return;
    }
    let home = folder("bundled-home-installed");
    let dir = folder("bundled-exe-installed");
    let seek = Seek::builder().home(Some(home)).installed_exe(Some(installed.clone())).bundled_dir(&dir).connect().unwrap();
    let hello = seek.hello().unwrap();
    assert!(same(&hello.exe, &installed), "{hello:?}");
    assert!(fs::read_dir(&dir).unwrap().next().is_none(), "nothing written: the bundled one wasn't needed");
    seek.stop().unwrap();
}
