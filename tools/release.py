"""Releases Seek from this PC: the version in src/version.h, built, tested and published with gh.

    python -I tools/release.py            build, test, push, publish
    python -I tools/release.py --install  and then install it here (-NoRegister)

Before: bump src/version.h (SEEK_VERSION and SEEK_VERSION_NUM), write its "## X.Y.Z" in CHANGELOG.md, run
python -I tools/web_corpus.py, and commit everything. It stops at the first thing that isn't right, before publishing.
"""
import hashlib, os, re, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(*cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def stop(why):
    raise SystemExit("release stopped: " + why)


version = re.search(r'SEEK_VERSION "([\d.]+)"', open(os.path.join(ROOT, "src", "version.h"), encoding="utf-8").read()).group(1)
changelog = open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8").read().replace("\r\n", "\n")
m = re.search(r"^## " + re.escape(version) + r"\n(.*?)(?=^## |\Z)", changelog, re.M | re.S)
if not m:
    stop(f"CHANGELOG.md has no '## {version}'")
if run("git", "status", "--porcelain").stdout.strip():
    stop("commit everything first (git status isn't clean)")
if run("gh", "release", "view", "v" + version, "--repo", "SynrgStudio/seek").returncode == 0:
    stop(f"v{version} is already released")

# the build: a failed one leaves the old seek.exe, and the tests would pass on it
log = os.path.join(ROOT, "build", "build.log")
os.makedirs(os.path.dirname(log), exist_ok=True)
with open(log, "w") as f:
    subprocess.run(["cmd", "/c", os.path.join(ROOT, "build.bat")], cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
text = open(log, errors="replace").read()
if "Done: build\\seek.exe" not in text or " error " in text:
    stop("the build failed: see build/build.log")
exe = os.path.join(ROOT, "build", "seek.exe")
got = run(exe, "--version").stdout.strip()
if got != "seek " + version:
    stop(f"build/seek.exe says {got!r}, not seek {version}")
print("built", got)

if run(sys.executable, "-I", os.path.join(ROOT, "tools", "web_corpus.py"), "--check").returncode != 0:
    stop("web/ is out of date: run python -I tools/web_corpus.py and commit")
tests = run(sys.executable, "-I", os.path.join(ROOT, "tests", "test.py"), timeout=1800)
if "all passed" not in tests.stdout:
    print("\n".join(l for l in tests.stdout.splitlines() if l.startswith("FAIL") or l.startswith("      "))[:4000])
    stop("the tests failed")
print("tests:", tests.stdout.count("PASS  "), "passed")

if run("git", "push", "-q", "origin", "main").returncode != 0:
    stop("git push failed")
sha = run("git", "rev-parse", "HEAD").stdout.strip()
digest = hashlib.sha256(open(exe, "rb").read()).hexdigest()
with tempfile.TemporaryDirectory() as tmp:
    sums = os.path.join(tmp, "SHA256SUMS.txt")
    open(sums, "w", newline="\n").write(f"{digest}  seek.exe\n")
    notes = os.path.join(tmp, "notes.md")
    open(notes, "w", encoding="utf-8", newline="\n").write(
        "Install or update, in PowerShell:\n\n```powershell\nirm https://raw.githubusercontent.com/SynrgStudio/seek/main/install.ps1 | iex\n```\n\n"
        f"Built from this release's commit, with the tests passing.\n\nSHA-256 of `seek.exe`: `{digest}`\n\n## What changed\n\n{m.group(1).strip()}\n")
    r = run("gh", "release", "create", "v" + version, exe, sums, "--repo", "SynrgStudio/seek", "--target", sha, "--title", "Seek " + version, "--notes-file", notes)
    if r.returncode != 0:
        stop("gh release create failed: " + r.stderr.strip())
print("released", r.stdout.strip())

if "--install" in sys.argv:
    time.sleep(25)  # releases/latest serves the previous release for a few seconds after a new one
    ps = "& ([scriptblock]::Create((Invoke-RestMethod https://raw.githubusercontent.com/SynrgStudio/seek/main/install.ps1))) -NoRegister | Out-Null"
    run("powershell", "-NoProfile", "-Command", ps, timeout=600)
    installed = os.path.join(os.environ["LOCALAPPDATA"], "Programs", "Seek", "seek.exe")
    print("installed:", run(installed, "--version").stdout.strip())
