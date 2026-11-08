"""Seek against the other ways an agent can search a Windows PC, on this PC.

    python -I seek\\bench.py [--ext blend] [--walkers]

Compared, when they are found (on PATH, or in the variables below):
    Everything   es.exe, with Everything running         SEEK_BENCH_ES
    ripgrep      rg.exe: what Claude Code's Grep runs    SEEK_BENCH_RG
    fff          fff-mcp.exe, as an MCP server            SEEK_BENCH_FFF
    fd           fd.exe, only with --walkers              SEEK_BENCH_FD
    Windows Search, through PowerShell

Seek and fff run as MCP servers over stdio and are timed warm, the way an agent's session uses them; the
command-line tools are started for every query, the way an agent's shell runs them. Times are medians of five runs
(three for the slower ones); with --walkers, fd and ripgrep also walk every fixed drive, once each, which takes
minutes. Tokens are the characters the agent gets back, divided by four. Nothing is written outside %TEMP%.
"""
import argparse, json, os, queue, shutil, statistics, string, subprocess, sys, tempfile, threading, time

NO_WINDOW = 0x08000000
HERE = os.path.dirname(os.path.abspath(__file__))
SEEK = os.path.join(HERE, "build", "seek.exe")
REPO = os.path.dirname(HERE)  # Jolt's repository
PROJECTS = os.path.dirname(REPO)  # the folder it is in: usually every project of the user
STATE = os.path.join(tempfile.gettempdir(), "seek-bench")


class Mcp:
    """A minimal MCP client over stdio: one JSON-RPC message per line."""

    def __init__(self, cmd):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  creationflags=NO_WINDOW)
        self.q, self.id = queue.Queue(), 0
        threading.Thread(target=self._read, daemon=True).start()
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "seek-bench", "version": "1"}})
        self.p.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}\n')
        self.p.stdin.flush()

    def _read(self):
        for line in self.p.stdout:
            try:
                self.q.put(json.loads(line))
            except ValueError:
                pass

    def request(self, method, params):
        self.id += 1
        t0 = time.perf_counter()
        self.p.stdin.write((json.dumps({"jsonrpc": "2.0", "id": self.id, "method": method, "params": params}) + "\n").encode())
        self.p.stdin.flush()
        while True:
            msg = self.q.get(timeout=600)
            if msg.get("id") == self.id:
                return time.perf_counter() - t0, msg

    def call(self, tool, args):
        t, msg = self.request("tools/call", {"name": tool, "arguments": args})
        return t, "".join(c.get("text", "") for c in (msg.get("result") or {}).get("content", []))

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(5)
        except Exception:
            self.p.kill()


def find_tool(env, exe, *more):
    for p in [os.environ.get(env), shutil.which(exe), *more]:
        if p and os.path.isfile(p):
            return p
    return None


def run(cmd, timeout=1800):
    t0 = time.perf_counter()
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout, creationflags=NO_WINDOW)
        return time.perf_counter() - t0, r.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return float(timeout), ""


def med(fn, n=5):
    ts, out = [], ""
    for _ in range(n):
        t, out = fn()
        ts.append(t)
    return statistics.median(ts), out


def lines(out):
    return sum(1 for l in out.splitlines() if l.strip())


def winsearch(where):
    return ["powershell", "-NoProfile", "-NonInteractive", "-Command",
            "$c = New-Object -ComObject ADODB.Connection; $c.Open(\"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';\"); "
            f"$rs = $c.Execute(\"SELECT System.ItemPathDisplay FROM SYSTEMINDEX WHERE {where}\"); "
            "while (-not $rs.EOF) { $rs.Fields.Item('System.ItemPathDisplay').Value; $rs.MoveNext() }"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ext", default="pdf", help="an extension to count on every drive (default pdf)")
    ap.add_argument("--walkers", action="store_true", help="also time fd and ripgrep walking every drive (minutes)")
    ap.add_argument("--json", help="also write the results here")
    a = ap.parse_args()

    es = find_tool("SEEK_BENCH_ES", "es.exe", r"C:\Program Files\Everything\es.exe")
    rg = find_tool("SEEK_BENCH_RG", "rg.exe")
    fff_exe = find_tool("SEEK_BENCH_FFF", "fff-mcp.exe", os.path.join(os.environ.get("LOCALAPPDATA", ""), "fff-mcp", "bin", "fff-mcp.exe"))
    fd = find_tool("SEEK_BENCH_FD", "fd.exe")
    drives = [f"{c}:\\" for c in string.ascii_uppercase
              if os.path.exists(f"{c}:\\") and __import__("ctypes").windll.kernel32.GetDriveTypeW(f"{c}:\\") == 3]
    target = os.path.join("src", "fuzzy.inl")
    rows = []

    def rec(task, tool, t, out, found=None, count=None):
        rows.append(dict(task=task, tool=tool, seconds=round(t, 4), tokens=len(out) // 4, found=found, count=count))
        f = "" if found is None else ("found" if found else "missed")
        print(f"{task:<34} {tool:<30} {t:>9.3f} s {len(out) // 4:>9,} tok  {f} {'' if count is None else count}", flush=True)

    print(f"{'task':<34} {'tool':<30} {'time':>11} {'tokens':>13}")
    run([SEEK, "--stop"])
    t0 = time.perf_counter()
    seek = Mcp([SEEK])
    _, out = seek.call("find", {"query": "fuzzy.inl", "limit": 5})
    rec("start: first answer", "Seek (daemon + index)", time.perf_counter() - t0, out)

    T = "find fuzzy.inl, every drive"
    t, out = med(lambda: seek.call("find", {"query": "fuzzy.inl", "limit": 5}))
    rec(T, "Seek", t, out, found=target in out)
    if es:
        t, out = med(lambda: run([es, "fuzzy.inl"]))
        rec(T, "Everything", t, out, found=target in out, count=lines(out))
    t, out = med(lambda: run(winsearch("System.FileName = 'fuzzy.inl'")), 3)
    rec(T, "Windows Search", t, out, found=target in out, count=lines(out))

    T = "find fuzy.inl (typo), every drive"
    t, out = med(lambda: seek.call("find", {"query": "fuzy.inl", "limit": 5}))
    rec(T, "Seek", t, out, found=target in out)
    if es:
        t, out = med(lambda: run([es, "fuzy.inl"]))
        rec(T, "Everything", t, out, found=target in out, count=lines(out))

    T = f"every .{a.ext}: how many, where"
    t, out = med(lambda: seek.call("find", {"query": "." + a.ext, "summary": True}))
    rec(T, "Seek summary", t, out)
    if es:
        t, out = med(lambda: run([es, "-get-result-count", "ext:" + a.ext]))
        rec(T, "Everything, count only", t, out, count=out.strip())
        t, out = med(lambda: run([es, "ext:" + a.ext]), 3)
        rec(T, "Everything, the list", t, out, count=lines(out))
    t, out = med(lambda: run(winsearch(f"System.FileExtension = '.{a.ext}'")), 3)
    rec(T, "Windows Search", t, out, count=lines(out))

    T = "what changed lately"
    t, out = med(lambda: seek.call("recent", {"limit": 10}))
    rec(T, "Seek recent", t, out)
    if es:
        t, out = med(lambda: run([es, "-sort", "date-modified-descending", "-n", "10"]))
        rec(T, "Everything, newest 10", t, out)

    fffs = []

    def fff(root, tag):
        st = os.path.join(STATE, "fff-" + tag)
        os.makedirs(st, exist_ok=True)
        t0 = time.perf_counter()
        m = Mcp([fff_exe, "--no-update-check", "--frecency-db", os.path.join(st, "frecency"), "--history-db",
                 os.path.join(st, "history"), "--log-file", os.path.join(st, "fff.log"), root])
        while "fuzzy.inl" not in m.call("find_files", {"query": "fuzzy.inl"})[1] and time.perf_counter() - t0 < 600:
            time.sleep(0.02)
        ready = time.perf_counter() - t0
        while "RecTake" not in m.call("grep", {"query": "RecTake"})[1] and time.perf_counter() - t0 < 600:
            time.sleep(0.05)
        time.sleep(3)  # its content index, built after the scan
        fffs.append(m)
        return m, ready

    for root, word, label in ((REPO, "RecTake", "Jolt's repository"), (PROJECTS, "CreateFileW", "every project")):
        T = f"grep {word}, {label}"
        seek.call("grep", {"pattern": word, "path": root})  # both read the files once before timing
        if rg:
            run([rg, "-n", word, root])
        t, out = med(lambda: seek.call("grep", {"pattern": word, "path": root}))
        rec(T, "Seek", t, out)
        if rg:
            t, out = med(lambda: run([rg, "-n", word, root]))
            rec(T, "ripgrep", t, out, count=lines(out))
        if root == REPO:
            t, out = med(lambda: run(["git", "-C", REPO, "grep", "-n", word]))
            rec(T, "git grep", t, out, count=lines(out))
        if fff_exe:
            m, ready = fff(root, "repo" if root == REPO else "projects")
            rec(f"start: first answer, {label}", "fff (scans the folder)", ready, "")
            t, out = med(lambda: m.call("grep", {"query": word}))
            rec(T, "fff", t, out)
            if root == REPO:
                for q in ("fuzzy.inl", "fuzy.inl"):
                    t, out = med(lambda: m.call("find_files", {"query": q}))
                    rec(f"find {q}, Jolt's repository", "fff", t, out, found="fuzzy.inl" in out)
                    t, out = med(lambda: seek.call("find", {"query": q, "path": REPO, "limit": 5}))
                    rec(f"find {q}, Jolt's repository", "Seek", t, out, found="fuzzy.inl" in out)

    for m in fffs:
        m.close()

    if a.walkers:
        T = "find fuzzy.inl, every drive"
        if fd:
            run([fd, "-HI", "-g", "fuzzy.inl", *drives])  # warms the drives' metadata
            t, out = run([fd, "-HI", "-g", "fuzzy.inl", *drives])
            rec(T, "fd", t, out, found=target in out, count=lines(out))
            t, out = run([fd, "-HI", "-e", a.ext, *drives])
            rec(f"every .{a.ext}: how many, where", "fd", t, out, count=lines(out))
        if rg:
            t, out = run([rg, "--files", "--no-ignore", "--hidden", "-g", "fuzzy.inl", *drives])
            rec(T, "ripgrep --files", t, out, found=target in out, count=lines(out))

    seek.close()
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
