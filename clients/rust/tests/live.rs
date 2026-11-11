//! Against a real Seek: build\seek.exe of this repository (or SEEK_EXE), as an instance of its own (a home in %TEMP%,
//! with a pipe of its own), so the Seek the user's agents talk to is left alone. With SEEK_OLD_EXE set to a seek.exe
//! before 0.5.0, the replacing of an older Seek is checked too.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use seek_client::*;

fn exe() -> PathBuf {
    std::env::var_os("SEEK_EXE")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join(r"..\..\build\seek.exe"))
        .canonicalize()
        .map(|p| PathBuf::from(p.to_string_lossy().trim_start_matches(r"\\?\")))
        .expect("build Seek first (build.bat), or set SEEK_EXE")
}

fn folder(name: &str) -> PathBuf {
    let p = std::env::temp_dir().join("seek-client-test").join(name);
    let _ = fs::remove_dir_all(&p);
    fs::create_dir_all(&p).unwrap();
    p
}

fn put(root: &Path, rel: &str, text: &str) -> PathBuf {
    let p = root.join(rel);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(&p, text).unwrap();
    p
}

fn same(a: &Path, b: &Path) -> bool {
    a.to_string_lossy().to_lowercase().trim_end_matches('\\') == b.to_string_lossy().to_lowercase().trim_end_matches('\\')
}

fn connect(home: &Path, own: Option<PathBuf>) -> Result<Seek> {
    let b = Seek::builder().home(Some(home.to_path_buf())).installed_exe(None).client("seek-client tests");
    match own {
        Some(e) => b.seek_exe(e),
        None => b,
    }
    .connect()
}

#[test]
fn every_command_against_a_real_seek() {
    let home = folder("home");
    let files = folder("files");
    let plan = put(&files, r"notes\plan.md", "- [ ] ship the qzk feature\n");
    put(&files, r"src\engine.rs", "fn engine() {}\n");

    let seek = connect(&home, Some(exe())).expect("connect, starting Seek");

    let hello = seek.hello().unwrap();
    assert!(hello.apis.contains(&1), "{hello:?}");
    assert!(same(&hello.exe, &exe()), "{hello:?}");
    assert!(same(&hello.data, &home), "{hello:?}");
    assert!(hello.pid > 0);

    let found = seek.find("plan").path(&files).run().unwrap();
    assert_eq!(found.kind, MatchKind::Real);
    assert!(found.entries.iter().any(|e| same(&e.path, &plan) && !e.is_dir), "{found:?}");
    let none = seek.find("qzzqxxnothingatall").path(&files).run().unwrap();
    assert!(none.entries.is_empty() && none.total == 0, "{none:?}");
    let one = seek.find("e").path(&files).max(1).run().unwrap();
    assert_eq!(one.entries.len(), 1, "{one:?}");

    let grepped = seek.grep("qzk", &files).run().unwrap();
    assert_eq!((grepped.files, grepped.lines, grepped.cut, grepped.how), (1, 1, false, How::Asked), "{grepped:?}");
    let hit = &grepped.hits[0];
    assert!(same(&hit.path, &plan));
    assert_eq!((hit.line, hit.column, hit.text.as_str()), (1, 16, "- [ ] ship the qzk feature"));
    let globbed = seek.grep("fn", &files).glob(["*.md"]).run().unwrap();
    assert_eq!(globbed.files, 0, "{globbed:?}");

    let recent = seek.recent().path(&files).since("1d").run().unwrap();
    assert!(recent.items.iter().any(|c| same(&c.path, &plan)), "{recent:?}");

    seek.opened(&plan).unwrap();
    let after = seek.recent().path(&files).walk(false).run().unwrap();
    assert!(after.items.iter().any(|c| same(&c.path, &plan) && c.what == What::Opened), "{after:?}");

    match seek.opened(files.join("nothere.md")) {
        Err(Error::Seek { code: Code::NotFound, .. }) => {}
        other => panic!("{other:?}"),
    }
    match seek.find("plan").path(files.join("nothere")).run() {
        Err(Error::Seek { code: Code::NotFound, .. }) => {}
        other => panic!("{other:?}"),
    }
    match seek.grep("", &files).run() {
        Err(Error::Seek { code: Code::BadArgs, .. }) => {}
        other => panic!("{other:?}"),
    }

    let status = seek.status().unwrap();
    assert_eq!(status.version, hello.version);
    assert!(!status.drives.is_empty(), "{status:?}");

    seek.stop().unwrap();
    match Seek::builder().home(Some(home.clone())).installed_exe(None).no_start().connect() {
        Err(Error::NotInstalled) => {}
        other => panic!("stopped, then {other:?}"),
    }
}

#[test]
fn a_seek_before_protocol_1_is_replaced_by_a_newer_one() {
    let Some(old) = std::env::var_os("SEEK_OLD_EXE").map(PathBuf::from) else {
        eprintln!("skipped: set SEEK_OLD_EXE to a seek.exe before 0.5.0");
        return;
    };
    let home = folder("home-old");
    let old = {  // as an .exe: Start-Process doesn't run a file named otherwise (the installer leaves seek.exe.old-*)
        let copy = folder("old-exe").join("seek.exe");
        fs::copy(&old, &copy).unwrap();
        copy
    };
    // through Start-Process, which hands the background process none of this test's handles (its output's pipe)
    let start_old = || {
        let ok = Command::new("powershell")
            .args(["-NoProfile", "-Command", &format!("Start-Process -FilePath '{}' -ArgumentList '--daemon' -WindowStyle Hidden", old.display())])
            .env("SEEK_HOME", &home)
            .status()
            .unwrap();
        assert!(ok.success());
        std::thread::sleep(std::time::Duration::from_secs(2));
    };

    // without a newer seek.exe at hand: it says so
    start_old();
    match connect(&home, None) {
        Err(Error::TooOld { version }) => assert!(version.starts_with("0.4"), "{version}"),
        other => panic!("{other:?}"),
    }

    // with one: the old one makes room
    let seek = connect(&home, Some(exe())).expect("replace the old Seek");
    let hello = seek.hello().unwrap();
    assert!(same(&hello.exe, &exe()), "{hello:?}");
    seek.stop().unwrap();
}
