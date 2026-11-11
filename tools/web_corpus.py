"""Writes Seek's web corpus (web/facts.toml and web/data/results.json) from what is true in this repository, so that no
number in it is typed by hand: the measurements come from bench/results.json, the version from src/version.h, and the
design's numbers (threads, sizes, the ranking's points) are read from the source code. The website is built from the
corpus (see web/README in synrgs-web's CORPUS.md), and asks for each number by its key.

    python -I tools/web_corpus.py            write them
    python -I tools/web_corpus.py --check    fail when they aren't what this would write (tests/test.py runs it)

The prose is in web/notes/, the rules in web/rules.md, the corpus' own history in web/CHANGELOG.md: those are written
by hand. A number in a note is written with its fact's key after it, like 52 ms [find_ms].
"""
import functools, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
R = json.load(open(os.path.join(ROOT, "bench", "results.json"), encoding="utf-8"))


def src(rel):
    return open(os.path.join(ROOT, rel), encoding="utf-8").read()


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()


def code(rel, pattern, group=1, cast=int):
    """A number read from the source: the build fails here, not on the page, when the code changed shape."""
    m = re.search(pattern, src(rel))
    if not m:
        raise SystemExit(f"{rel}: no match for {pattern!r}: the code changed; update tools/web_corpus.py")
    return cast(m.group(group))


VERSION = re.search(r'SEEK_VERSION "([\d.]+)"', src("src/version.h")).group(1)
BENCH_COMMIT = git("log", "-1", "--format=%h", "--", "bench/results.json")
BENCH_DATE = git("log", "-1", "--format=%cs", "--", "bench/results.json")
BENCH_SEEK = re.search(r'SEEK_VERSION "([\d.]+)"', git("show", f"{BENCH_COMMIT}:src/version.h")).group(1)
BENCH = f"bench/results.json (bench/bench.py), Seek {BENCH_SEEK}, run committed in {BENCH_COMMIT}"
SHELL = "bench/results.json, a separate run of the shell tools (a ten-minute walk isn't repeated)"


@functools.cache
def released():
    """The date of this version's release on GitHub; the version's commit date when gh can't say. Asked once."""
    try:
        out = subprocess.run(["gh", "release", "view", "v" + VERSION, "--repo", "SynrgStudio/seek", "--json", "publishedAt", "-q", ".publishedAt"],
                             cwd=ROOT, capture_output=True, text=True, timeout=30)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()[:10]
    except (OSError, subprocess.TimeoutExpired):
        pass
    return git("log", "-1", "--format=%cs", "-S", f'SEEK_VERSION "{VERSION}"', "--", "src/version.h")


def row(task, tool):
    task = R["tasks"].get(task, task)
    for r in R["rows"]:
        if r["task"] == task and r["tool"] == tool:
            return r
    raise SystemExit(f"bench/results.json: no row for {task!r} / {tool!r}")


def as_time(s):
    """Seconds as the page writes them: ms below a second, with the decimals that mean something."""
    if s < 0.001:
        return round(s * 1000, 2), "ms"
    if s < 0.01:
        return round(s * 1000, 1), "ms"
    if s < 1:
        return round(s * 1000), "ms"
    if s < 10:
        return round(s, 1), "s"
    if s < 600:
        return round(s), "s"
    return round(s / 60, 1), "min"


FACTS = {}  # key -> dict(value, unit, what, source, date)


def fact(key, value, unit, what, source, date=BENCH_DATE):
    assert key not in FACTS, key
    FACTS[key] = {"value": value, "unit": unit, "what": what, "source": source, "date": date}


def timed(key, task, tool, what, source=BENCH):
    v, u = as_time(row(task, tool)["seconds"])
    fact(key, v, u, what, source)


def tokens(key, task, tool, what, source=BENCH):
    fact(key, row(task, tool)["tokens"], "tokens", what, source)


# ---------------------------------------------------------------- the benchmark: what was measured, as measured
facts = R["facts"]
names = re.match(r"([\d.]+) million", facts["names"])
fact("names_millions", float(names.group(1)), "million", "files and folders on the benchmark PC's three drives", BENCH)
fact("machine", facts["machine"], "", "the benchmark PC", BENCH)
fact("tool_versions", facts["versions"], "", "the versions of every tool in the benchmark", BENCH)
fact("blend_files", int(row("count", "Everything, count only")["count"]), "files", "the .blend files on the benchmark PC, as Everything counted them", BENCH)
fact("projects_files", int(re.search(r"([\d,]+) files", R["labels"]["grep_projects"]).group(1).replace(",", "")), "files",
     "files in the folder of every project, the text search's big case", BENCH)
fact("repo_files", int(re.search(r"([\d,]+) files", R["labels"]["grep_repo"]).group(1).replace(",", "")), "files",
     "files in Jolt's repository, the text search's small case", BENCH)

# names, every drive
timed("find_ms", "find", "Seek", "find one file by name, place unknown, on every drive (4.9 million names); the MCP server warm, timed from request to answer")
timed("find_everything_ms", "find", "Everything", "the same with Everything's es.exe, started once per search as an agent's shell runs it (the time includes starting es.exe)")
timed("find_windows_search_ms", "find", "Windows Search", "the same with Windows Search; it didn't find the file")
timed("find_fd_s", "find", "fd", "the same with fd walking every drive, with the folders already in Windows' memory")
timed("find_ripgrep_files_s", "find", "ripgrep --files", "the same with ripgrep --files walking every drive, folders in memory")
timed("find_dir_s", "find", "dir /s", "the same with dir /s, over the user profile only", SHELL)
timed("find_get_childitem_s", "find", "Get-ChildItem", "the same with Get-ChildItem -Recurse, over the user profile only", SHELL)
fact("find_fd_from_disk_min", 15, "min", "fd walking every drive read from disk was cut at 15 minutes without finishing (the hard disk)", BENCH)
timed("find_typo_ms", "find_typo", "Seek", "the same search with a typo in the name (fuzy.inl for fuzzy.inl); Seek found it")
fact("find_typo_everything_found", bool(row("find_typo", "Everything")["found"]), "", "whether Everything found the file with that typo", BENCH)

# counting a kind of file
timed("count_ms", "count", "Seek summary", "how many .blend files there are and where, as Seek's summary")
tokens("count_tokens", "count", "Seek summary", "the tokens of that summary (characters / 4)")
timed("count_everything_list_ms", "count", "Everything, the list", "Everything listing them all")
tokens("count_everything_list_tokens", "count", "Everything, the list", "the tokens of Everything's list (characters / 4)")
timed("count_everything_count_ms", "count", "Everything, count only", "Everything giving only the count")
timed("count_fd_s", "count", "fd", "fd listing them, walking every drive with the folders in memory")
tokens("count_fd_tokens", "count", "fd", "the tokens of fd's list")
v, u = as_time(R["rows"][[i for i, r in enumerate(R["rows"]) if r["task"] == R["tasks"]["count"] and r["tool"] == "ripgrep --files"][0]]["seconds"])
fact("count_ripgrep_files_s", v, u, "ripgrep --files listing them, measured on its own the same day", BENCH)
timed("count_dir_s", "count", "dir /s", "dir /s listing them over C:\\Users, D: and E:", SHELL)
tokens("count_dir_tokens", "count", "dir /s", "the tokens of that listing", SHELL)
fact("count_windows_search_found", int(row("count", "Windows Search")["count"]), "files", "how many of the .blend files Windows Search found", BENCH)

# recent
timed("recent_ms", "recent", "Seek recent", "what changed lately, from Seek")
timed("recent_everything_ms", "recent", "Everything, newest 10", "the newest 10 files on the PC, from Everything")

# text
timed("grep_repo_ms", "grep_repo", "Seek, again", "search a word in Jolt's repository (52 files), the folder already in Seek's memory")
timed("grep_repo_first_ms", "grep_repo", "Seek, first search", "the same, the first search of that folder (read from the drive)")
timed("grep_repo_fff_ms", "grep_repo", "fff", "the same with fff, warm")
timed("grep_repo_ripgrep_ms", "grep_repo", "ripgrep", "the same with ripgrep")
timed("grep_repo_git_grep_ms", "grep_repo", "git grep", "the same with git grep")
timed("grep_repo_select_string_s", "grep_repo", "Select-String", "the same with PowerShell's Select-String, which reads build outputs too", SHELL)
tokens("grep_repo_select_string_tokens", "grep_repo", "Select-String", "the tokens of Select-String's answer", SHELL)
timed("grep_repo_findstr_s", "grep_repo", "findstr /s", "the same with findstr /s, which reads build outputs too", SHELL)
timed("grep_projects_ms", "grep_projects", "Seek, again", "search a word in every project (10,700 files), the folder already in Seek's memory")
timed("grep_projects_first_ms", "grep_projects", "Seek, first search", "the same, the first search of that folder (read from the drive)")
timed("grep_projects_fff_ms", "grep_projects", "fff", "the same with fff, warm")
timed("grep_projects_ripgrep_ms", "grep_projects", "ripgrep", "the same with ripgrep")
s_seek = row("grep_projects", "Seek, again")["seconds"]
fact("text_vs_fff", round(row("grep_projects", "fff")["seconds"] / s_seek, 1), "x", "how many times faster than fff Seek searched the text of every project (from memory, both)", BENCH)
fact("text_vs_ripgrep", round(row("grep_projects", "ripgrep")["seconds"] / s_seek), "x", "how many times faster than ripgrep Seek searched the text of every project (Seek from memory; ripgrep reads every time)", BENCH)
fact("text_first_vs_ripgrep", round(row("grep_projects", "ripgrep")["seconds"] / row("grep_projects", "Seek, first search")["seconds"], 1), "x",
     "how many times faster than ripgrep Seek's first search of every project was (both reading the drive)", BENCH)

# a name in one folder
timed("find_repo_ms", "find_repo", "Seek", "find a name inside Jolt's repository")
timed("find_repo_fff_ms", "find_repo", "fff", "the same with fff")
timed("find_repo_typo_ms", "find_repo_typo", "Seek", "the same with a typo")
timed("find_repo_typo_fff_ms", "find_repo_typo", "fff", "the same with fff, with a typo")

# starting, memory
timed("start_ms", "start", "Seek (daemon + index)", "from starting the background process to its first answer, with Jolt's index")
timed("start_fff_repo_ms", "start_fff_repo", "fff (scans the folder)", "fff's first answer in Jolt's repository (it scans the folder)")
timed("start_fff_projects_ms", "start_fff_projects", "fff (scans the folder)", "fff's first answer in every project")
mem = {r["tool"]: r["mb"] for r in R["rows"] if r["task"] == "memory"}
fact("memory_mb", mem["Seek"], "MB", "Seek's memory of its own, with every project kept in memory for grep; one process for every agent", BENCH)
fact("memory_no_folders_mb", mem["Seek, no folders"], "MB", "Seek's memory of its own with no folder kept in memory (the index files are mapped from disk, not counted)", BENCH)
fact("memory_everything_mb", mem["Everything"], "MB", "Everything's memory", BENCH)
fact("memory_fff_repo_mb", mem["fff, " + R["labels"]["repo_short"]], "MB", "fff's memory for Jolt's repository, per agent session and folder", BENCH)
fact("memory_fff_projects_mb", mem["fff, every project"], "MB", "fff's memory for every project, per agent session and folder", BENCH)
over = re.match(r"([\d.]+) ms", facts["overhead"])
fact("overhead_ms", float(over.group(1)), "ms", "the trip from an agent's session to the background process and back, the search left out", BENCH)

# the first index, without Jolt
ib = facts["index_build"]
m = re.match(r"([\d.]+) s for the ([\d.]+) million names on an NVMe SSD, ([\d.]+) s for the ([\d.]+) million on another, and ([\d.]+) minutes for the ([\d.]+) million on a hard disk read cold", ib)
if not m:
    raise SystemExit("bench/results.json: facts.index_build changed shape; update tools/web_corpus.py")
fact("index_ssd_s", float(m.group(1)), "s", f"Seek walking a drive itself (no Jolt): {m.group(2)} million names on an NVMe SSD", BENCH)
fact("index_ssd_names_millions", float(m.group(2)), "million", "the names on that NVMe SSD", BENCH)
fact("index_ssd2_s", float(m.group(3)), "s", f"the same: {m.group(4)} million names on another SSD", BENCH)
fact("index_ssd2_names_millions", float(m.group(4)), "million", "the names on that other SSD", BENCH)
fact("index_hdd_min", float(m.group(5)), "min", f"the same: {m.group(6)} million names on a hard disk, read cold", BENCH)
fact("index_hdd_names_millions", float(m.group(6)), "million", "the names on that hard disk", BENCH)

# grep's history, every project: each step in seconds too (data/grep_history.json), the first and the last as facts
hist = R["history"]


def hist_seconds(text):
    m = re.fullmatch(r"([\d.]+) (s|ms)", text.strip())
    if not m:
        raise SystemExit(f"bench/results.json: a history time reads {text!r}; update tools/web_corpus.py")
    return float(m.group(1)) * (1 if m.group(2) == "s" else 0.001)


HISTORY = [{"step": h[0], "seconds": round(hist_seconds(h[1]), 6), "files_read": None if h[2] in ("none", "") else int(h[2].replace(",", ""))} for h in hist]
first, last = as_time(float(hist[0][1].split()[0]) * (1 if hist[0][1].endswith(" s") else 0.001)), as_time(float(hist[-1][1].split()[0]) * (1 if hist[-1][1].endswith(" s") else 0.001))
fact("grep_history_first_s", first[0], first[1], "grep over every project in the first comparison (stopped at its 6 s budget)", BENCH)
fact("grep_history_last_ms", last[0], last[1], "the same search after the last step of grep's history (data/results.json, history)", BENCH)

# ---------------------------------------------------------------- the design: numbers read from the source code
DESIGN = "src/ of this version"
fact("index_entry_bytes", code("src/index.inl", r"static_assert\(sizeof\(IdxNode\) == (\d+)"), "bytes", "the size of one name's entry in the drive index (Jolt's format): parent, name, folder or not, depth", "src/index.inl", None)
fact("name_scan_threads", code("src/index.inl", r"work < 100000 \? 1u : std::max\(1u, std::min\((\d+)u"), "threads", "at most, to run every name through the fuzzy matching; one for under 100,000 names", "src/index.inl", None)
fact("index_walk_threads", code("src/index.inl", r"n = std::max\(2u, std::min\((\d+)u, std::thread::hardware_concurrency\(\)\)\); i < n; \+\+i\) threads.emplace_back\(work\)"), "threads",
     "at most, to walk a drive when Seek makes its own index", "src/index.inl", None)
fact("grep_threads", code("src/grep.inl", r"nth = std::max\(2u, std::min\((\d+)u"), "threads", "at most, to list a folder and read its files", "src/grep.inl", None)
fact("binary_probe_kb", code("src/grep.inl", r"std::min<uint64_t>\(size, (\d+)\)") // 1024, "KB", "what grep reads of a file to tell it is binary; then the rest isn't read", "src/grep.inl", None)
fact("grep_file_max_mb", code("src/grep.inl", r"kTextMax = (\d+)ull << 20"), "MB", "files over this aren't read by grep in a folder (named in the answer); a file given as the path is read whatever its size", "src/grep.inl", None)
fact("grep_line_chars", code("src/grep.inl", r"kWide = (\d+)"), "characters", "a line longer than this is shown around its matches only", "src/grep.inl", None)
fact("grep_line_windows", code("src/grep.inl", r"kWindows = (\d+)"), "windows", "how many matches of one long line are shown, each with what is around it", "src/grep.inl", None)
fact("grep_context_max", code("src/tools.inl", r'ArgInt\(a, "before", around, 0, (\d+)\)'), "lines", "the most context grep shows on each side of a match (before, after)", "src/tools.inl", None)
fact("regex_line_chars", code("src/grep.inl", r"text.data\(\) \+ std::min\(e, s \+ (\d+)\);  // very long lines"), "characters", "regex looks at this many characters of a very long line", "src/grep.inl", None)
fact("cache_mb", code("src/util.inl", r"uint64_t cacheMb = (\d+);"), "MB", "folders kept in memory for grep, at most, by default (cache in seek.txt)", "src/util.inl", None)
fact("cache_idle_min", code("src/cache.inl", r"kCacheIdle = (\d+) \* 60 \* 1000"), "min", "a folder nobody searched for this long is let go", "src/cache.inl", None)
fact("cache_changes_max", code("src/cache.inl", r"kDirtyMax = (\d+);"), "changes", "more changes than this between two searches, and a kept folder is read again whole", "src/cache.inl", None)
fact("ranked_results", code("src/tools.inl", r"constexpr size_t kRanked = (\d+);"), "results", "the best names by name that find ranks again by what the user is doing, and pages through", "src/tools.inl", None)
fact("jolt_check_min", code("src/server.inl", r"if \(tick % (\d+) == 0\)\s*\n\s*for \(wchar_t c : FixedDrives\(\)\) LoadVolume\(c, false\);"),
     "min", "how often Seek looks for a newer copy of Jolt's index (and of its own)", "src/server.inl", None)
fact("activity_save_min", code("src/server.inl", r"if \(tick % (\d+) == 0\) \{\s*\n\s*ActSave\(\);"), "min", "how often what Seek saw change is saved to disk", "src/server.inl", None)
fact("rank_match_scale", code("src/tools.inl", r"c.key = sc \* (\d+) - std::min<int>\(depth, \d+\);"), "x",
     "a name's score is its matching score (with the exact-name and program points) times this, before what Seek knows about the user is added", "src/tools.inl", None)
fact("rank_depth_max", code("src/tools.inl", r"c.key = sc \* \d+ - std::min<int>\(depth, (\d+)\);"), "points",
     "a name's depth (folders above it) is taken from its score, up to this", "src/tools.inl", None)
for key, pat, what in [
    ("rank_exact_program", r"kExactProgram = (\d+)", "matching points for a program whose name is exactly what was typed (.exe, .lnk, .bat)"),
    ("rank_exact_name", r"kExactName = (\d+)", "matching points for a name that is exactly what was typed (without its extension, or with it)"),
    ("rank_program", r"kProgramName = (\d+)", "matching points for a program with the word whole in its name"),
]:
    fact(key, code("src/fuzzy.inl", pat), "points", what, "src/fuzzy.inl", None)
for key, pat, what in [
    ("rank_changed_hour", r"age < kHour \? (\d+) :", "points for a file changed in the last hour"),
    ("rank_changed_day", r"age < kHour \? \d+ : age < kDay \? (\d+) :", "points for one changed in the last day"),
    ("rank_changed_week", r"age < 7 \* kDay \? (\d+) : 0", "points for one changed in the last week"),
    ("rank_jolt_folder_max", r"std::min\((\d+)\.0, 300\.0 \* fh\)", "points, at most, for a file in a folder the user opens in Jolt, by how much it is used"),
    ("rank_jolt_file_max", r"std::min\((\d+)\.0, 600\.0 \+ 300\.0 \* h\)", "points, at most, for a file the user opened from Jolt"),
    ("rank_jolt_picked_max", r"std::min\((\d+)\.0, 3000\.0 \+ 500\.0 \* picked\)", "points, at most, for a file picked in Jolt's Ctrl+P for this very text"),
    ("rank_near", r"f.boost \+= (\d+);\s*\n\s*f.why.push_back\(L\"inside \"", "points for a file inside the folder given as near"),
    ("rank_near_level", r"f.boost \+= static_cast<int>\((\d+) \* \(shared - 1\)\)", "points for each folder level shared with near, otherwise"),
    ("rank_noise", r"f.boost -= (\d+);  // caches", "points taken from a file in a cache, a package store, a log or AppData"),
]:
    fact(key, code("src/tools.inl", pat), "points", what, "src/tools.inl", None)
fact("protocol_version", code("src/server.inl", r"constexpr int kApi = (\d+);"), "", "the version of Seek's protocol for programs (PROTOCOL.md)", "src/server.inl", None)

# ---------------------------------------------------------------- measured by others: their numbers, their sources
OTHERS = [
    ("fff_agent_shell_s", 24.7, "s", "fff's README: a whole agent task in a big repository with shell search", "https://github.com/dmtrKovalenko/fff"),
    ("fff_agent_fff_s", 14.3, "s", "fff's README: the same task with fff", "https://github.com/dmtrKovalenko/fff"),
    ("fff_agent_shell_tokens", 85400, "tokens", "fff's README: that task's tokens with shell search", "https://github.com/dmtrKovalenko/fff"),
    ("fff_agent_fff_tokens", 82500, "tokens", "fff's README: with fff (almost the same)", "https://github.com/dmtrKovalenko/fff"),
    ("fff_heldout_fff_s", 9.0, "s", "fff's README: a held-out run where one model was slower with fff", "https://github.com/dmtrKovalenko/fff"),
    ("fff_heldout_shell_s", 8.1, "s", "fff's README: the same held-out run with shell search", "https://github.com/dmtrKovalenko/fff"),
    ("fff_chromium_files", 500000, "files", "fff's README: the Chromium checkout of its test", "https://github.com/dmtrKovalenko/fff"),
    ("fff_chromium_ripgrep_s_min", 3, "s", "fff's README: each ripgrep run on that checkout, from", "https://github.com/dmtrKovalenko/fff"),
    ("fff_chromium_ripgrep_s_max", 9, "s", "fff's README: each ripgrep run on that checkout, to", "https://github.com/dmtrKovalenko/fff"),
    ("fff_chromium_query_ms_max", 10, "ms", "fff's README: each fff query on that checkout, under", "https://github.com/dmtrKovalenko/fff"),
    ("everything_index_min_per_million", 1, "min", "Everything's FAQ: indexing about a million files", "https://www.voidtools.com/faq/"),
    ("everything_mb_per_million", 100, "MB", "Everything's FAQ: memory for about a million files", "https://www.voidtools.com/faq/"),
    ("ripgrep_kernel_s", 0.082, "s", "ripgrep's README, the Linux kernel's source: ripgrep", "https://github.com/BurntSushi/ripgrep#quick-examples-comparing-tools"),
    ("ripgrep_kernel_git_grep_s", 0.273, "s", "the same: git grep -P", "https://github.com/BurntSushi/ripgrep#quick-examples-comparing-tools"),
    ("ripgrep_kernel_ag_s", 0.443, "s", "the same: The Silver Searcher", "https://github.com/BurntSushi/ripgrep#quick-examples-comparing-tools"),
    ("ripgrep_kernel_ugrep_s", 0.639, "s", "the same: ugrep", "https://github.com/BurntSushi/ripgrep#quick-examples-comparing-tools"),
    ("ripgrep_kernel_ack_s", 2.9, "s", "the same: ack", "https://github.com/BurntSushi/ripgrep#quick-examples-comparing-tools"),
    ("claude_glob_timeout_s", 20, "s", "users' reports: Claude Code's Glob tool timing out in large repositories, where ripgrep alone takes under a second", "https://github.com/anthropics/claude-code/issues"),
]
for key, value, unit, what, url in OTHERS:
    fact(key, value, unit, what + " (measured by others, on their machines)", url, None)


# ---------------------------------------------------------------- writing
def toml_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    return json.dumps(v, ensure_ascii=False)


def facts_toml():
    out = [
        "# Seek's facts, for synrgs.tech: written by tools/web_corpus.py from bench/results.json, src/version.h and the",
        "# source code. Don't edit by hand: change what it reads, and run it (tests/test.py fails when this is stale).",
        "",
        "[project]",
        'name = "Seek"',
        f'version = "{VERSION}"',
        f"released = {released()}",
        'license = "GPL-3.0"',
        'platforms = ["Windows 10", "Windows 11"]',
        'repo = "https://github.com/SynrgStudio/seek"',
        'download = "https://github.com/SynrgStudio/seek/releases/latest"',
        'install = "irm https://raw.githubusercontent.com/SynrgStudio/seek/main/install.ps1 | iex"',
        'protocol = "https://github.com/SynrgStudio/seek/blob/main/PROTOCOL.md"',
        'changelog = "https://github.com/SynrgStudio/seek/blob/main/CHANGELOG.md"',
        'related = ["https://jolt.synrgs.tech"]',
        f'bench_seek_version = "{BENCH_SEEK}"   # the Seek the benchmark measured; newer ones are faster at find (CHANGELOG 0.4.4)',
        "",
    ]
    for key, f in FACTS.items():
        out.append(f"[facts.{key}]")
        out.append(f"value = {toml_value(f['value'])}")
        if f["unit"]:
            out.append(f"unit = {toml_value(f['unit'])}")
        out.append(f"what = {toml_value(f['what'])}")
        out.append(f"source = {toml_value(f['source'])}")
        out.append(f"date = {f['date'] or released()}")
        out.append("")
    out += [
        "[media.icon]",
        'file = "seek-512.png"',
        'what = "Seek\'s icon, 512 px; seek-256.png is the same at 256 px, seek.ico has every Windows size"',
        "",
    ]
    return "\n".join(out)


def note_keys():
    """Every [key] a note cites must be a fact: a note can't cite a number the corpus doesn't have."""
    missing = []
    notes = os.path.join(WEB, "notes")
    for name in sorted(os.listdir(notes)):
        text = open(os.path.join(notes, name), encoding="utf-8").read()
        for key in re.findall(r"\[([a-z][a-z0-9_]+)\]", text):
            if key not in FACTS and key != "project":  # `[project]` names the table, not a fact
                missing.append(f"notes/{name}: [{key}]")
    for key in ("machine", "tool_versions"):
        assert key in FACTS
    return missing


def main():
    check = "--check" in sys.argv
    missing = note_keys()
    if missing:
        raise SystemExit("notes cite facts that don't exist: " + "; ".join(missing))
    want = {
        os.path.join(WEB, "facts.toml"): facts_toml(),
        os.path.join(WEB, "data", "results.json"): src("bench/results.json"),
        os.path.join(WEB, "data", "grep_history.json"): json.dumps(
            {"about": "grep over every project (CreateFileW), step by step; the same as history in results.json, with the times in seconds. Written by tools/web_corpus.py.",
             "steps": HISTORY}, indent=1, ensure_ascii=False) + "\n",
    }
    stale = []
    for path, text in want.items():
        # read without newline translation, then compared without line endings: git may check files out with CRLF
        have = open(path, encoding="utf-8", newline="").read() if os.path.exists(path) else None
        if have is None or have.replace("\r\n", "\n") != text.replace("\r\n", "\n"):
            stale.append(os.path.relpath(path, ROOT))
            if not check:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                open(path, "w", encoding="utf-8", newline="\n").write(text)
    if check:
        if stale:
            raise SystemExit("web corpus out of date: " + ", ".join(stale) + "; run python -I tools/web_corpus.py")
        print("web corpus up to date")
    else:
        print("wrote " + (", ".join(stale) if stale else "nothing: up to date") + f"; {len(FACTS)} facts")


if __name__ == "__main__":
    main()
