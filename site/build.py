"""Builds the site into _site/: site/index.html with its placeholders filled from the measurements in
bench/results.json and the version in src/version.h, the charts drawn as inline SVG, and the files it links to.

    python site/build.py

Placeholders in index.html:
    {{VERSION}}                      the version
    {{t:TASK|TOOL}}                  a measured time, like "0.45 ms" or "4.6 s"
    {{k:TASK|TOOL}}                  the tokens of that answer
    {{n:TASK|TOOL}}                  its count (files found, lines, ...)
    {{mb:TOOL}}                      a memory row
    {{chart:ID}}  {{table:ID}}       the charts and tables defined below
Every number on the page comes from results.json, so the page can't disagree with what was measured.
"""
import html, json, math, os, re, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "site")
OUT = os.path.join(ROOT, "_site")

R = json.load(open(os.path.join(ROOT, "bench", "results.json"), encoding="utf-8"))
ROWS = R["rows"]


def version():
    text = open(os.path.join(ROOT, "src", "version.h"), encoding="utf-8").read()
    return re.search(r'SEEK_VERSION "([\d.]+)"', text).group(1)


def row(task, tool):
    for r in ROWS:
        if r["task"] == task and r["tool"] == tool:
            return r
    raise KeyError(f"no measurement for {task!r} / {tool!r}")


def fmt_time(s):
    if s is None:
        return "–"
    if s < 0.001:
        return f"{s * 1000:.2f} ms"
    if s < 0.01:
        return f"{s * 1000:.1f} ms".replace(".0 ms", " ms")
    if s < 1:
        return f"{s * 1000:.0f} ms"
    if s < 10:
        return f"{s:.1f} s"
    if s < 600:
        return f"{s:.0f} s"
    return f"{s / 60:.0f} min"


def fmt_tick(s):
    """An axis' label: powers of ten, written short."""
    if s < 1:
        return f"{s * 1000:g} ms"
    return f"{s:g} s"


def fmt_int(n):
    return f"{n:,}"


def esc(s):
    return html.escape(str(s))


# ------------------------------------------------------------------ charts: dots on a log scale, Seek in the accent
def chart(title, subtitle, items, lo=None, hi=None):
    """items: (label, seconds, is_seek, note). A dot plot on a log scale; times span six orders of magnitude."""
    vals = [s for _, s, _, _ in items if s]
    lo = lo or 10 ** math.floor(math.log10(min(vals)) - 0.15)
    hi = hi or 10 ** math.ceil(math.log10(max(vals)) + 0.05)
    W, label_w, right = 920, 210, 24
    row_h, top = 30, 8
    H = top + row_h * len(items) + 30
    x0, x1 = label_w, W - right
    X = lambda v: x0 + (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * (x1 - x0)
    out = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="{esc(title)}">']
    ticks, t = [], lo
    while t <= hi * 1.0001:
        ticks.append(t)
        t *= 10
    for t in ticks:
        x = X(t)
        out.append(f'<line x1="{x:.1f}" y1="{top - 4}" x2="{x:.1f}" y2="{H - 26}" stroke="#2b2d30" stroke-width="1"/>')
        out.append(f'<text x="{x:.1f}" y="{H - 8}" fill="#73777b" font-size="12" text-anchor="middle">{esc(fmt_tick(t))}</text>')
    for i, (label, s, seek, note) in enumerate(items):
        y = top + row_h * i + row_h / 2
        weight = "600" if seek else "400"
        color = "#f0f0ed" if seek else "#a3a4a6"
        out.append(f'<text x="{label_w - 14}" y="{y + 4:.1f}" fill="{color}" font-size="13.5" font-weight="{weight}" text-anchor="end">{esc(label)}</text>')
        if not s:
            out.append(f'<text x="{x0 + 4}" y="{y + 4:.1f}" fill="#73777b" font-size="12.5">{esc(note or "–")}</text>')
            continue
        x = X(s)
        out.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x:.1f}" y2="{y:.1f}" stroke="{"#55b8ff" if seek else "#3a3b43"}" stroke-width="2" stroke-opacity="{0.55 if seek else 1}"/>')
        r = 6.5 if seek else 5.5
        fill = "url(#g)" if seek else "#8d8d99"
        out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{fill}" stroke="#181a1d" stroke-width="2"/>')
        text = fmt_time(s) + (f"  ·  {note}" if note else "")
        anchor_right = x > x1 - 210
        tx = x - 12 if anchor_right else x + 12
        out.append(f'<text x="{tx:.1f}" y="{y + 4:.1f}" fill="{"#ffffff" if seek else "#c4c4ce"}" font-size="12.5" '
                   f'font-weight="{weight}" text-anchor="{"end" if anchor_right else "start"}">{esc(text)}</text>')
    out.insert(1, '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#6eebff"/>'
                  '<stop offset="1" stop-color="#3a6aff"/></linearGradient></defs>')
    out.append("</svg>")
    sub = f"<span>{esc(subtitle)}</span>" if subtitle else ""
    return f'<figure class="chart"><figcaption>{esc(title)}{sub}</figcaption>{"".join(out)}</figure>'


def items(task, tools):
    """tools: (tool, label, note) or tool names; a note of None shows the answer's tokens when it has many."""
    out = []
    for t in tools:
        tool, label, note = t if isinstance(t, tuple) else (t, t, None)
        r = row(task, tool)
        out.append((label, r.get("seconds"), label.startswith("Seek"), note))
    return out


# ------------------------------------------------------------------ tables
def table(head, rows, seek_rows=(), nums=()):
    th = "".join(f'<th class="{"num" if i in nums else ""}">{h}</th>' for i, h in enumerate(head))
    body = []
    for k, r in enumerate(rows):
        cls = ' class="seek"' if k in seek_rows else ""
        body.append(f"<tr{cls}>" + "".join(f'<td class="{"num" if i in nums else ""}">{c}</td>' for i, c in enumerate(r)) + "</tr>")
    return f'<div class="table-wrap"><table><thead><tr>{th}</tr></thead><tbody>{"".join(body)}</tbody></table></div>'


def ok(b):
    return '<span class="ok">yes</span>' if b else '<span class="no">no</span>'


def T(task, tool):
    return fmt_time(row(task, tool).get("seconds"))


def K(task, tool):
    return fmt_int(row(task, tool)["tokens"])


TASKS = R["tasks"]  # the task names, by short id: the benchmark writes the long ones


def tk(id):
    return TASKS[id]


def build_charts():
    c = {}
    c["find"] = chart("Find a file by name, place unknown", "every fixed drive, 4.9 million names",
                      items(tk("find"), [("Seek", "Seek", None), ("Everything", "Everything (es.exe)", None),
                                         ("Windows Search", "Windows Search", "didn't find it"), ("fd", "fd, folders in memory", None),
                                         ("ripgrep --files", "ripgrep --files", None), ("fd, from disk", "fd, from disk", "over 15 min, cut"),
                                         ("dir /s", "dir /s, profile only", None), ("Get-ChildItem", "Get-ChildItem, profile only", None)]))
    c["grep_projects"] = chart("Search text in every project", R["labels"]["grep_projects"],
                               items(tk("grep_projects"), [("Seek, again", "Seek", None), ("fff", "fff", None),
                                                           ("Seek, first search", "Seek, first search", None), ("ripgrep", "ripgrep", None)]))
    c["grep_repo"] = chart("Search text in one repository", R["labels"]["grep_repo"],
                           items(tk("grep_repo"), [("Seek, again", "Seek", None), ("fff", "fff", None), ("Seek, first search", "Seek, first search", None),
                                                   ("git grep", "git grep", None), ("ripgrep", "ripgrep", None)]))
    c["find_repo"] = chart("Find a file by name inside one repository", "",
                           items(tk("find_repo"), [("Seek", "Seek", None), ("fff", "fff", None)]) +
                           [(l + ", typo", s, seek, n) for l, s, seek, n in items(tk("find_repo_typo"), [("Seek", "Seek", None), ("fff", "fff", None)])])
    c["tokens"] = token_chart()
    return c


def token_chart():
    """How many tokens the answer to 'how many .blend files, and where' costs, on a log scale too."""
    t = tk("count")
    rows = [("Seek summary", row(t, "Seek summary")["tokens"], True), ("Everything, the list", row(t, "Everything, the list")["tokens"], False),
            ("fd", row(t, "fd")["tokens"], False)]
    W, label_w, right, row_h, top = 920, 210, 24, 30, 8
    H = top + row_h * len(rows) + 30
    lo, hi = 100, 1_000_000
    X = lambda v: label_w + (math.log10(v) - 2) / 4 * (W - right - label_w)
    out = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="Tokens">',
           '<defs><linearGradient id="g2" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#6eebff"/><stop offset="1" stop-color="#3a6aff"/></linearGradient></defs>']
    for v in (100, 1000, 10000, 100000, 1000000):
        x = X(v)
        out.append(f'<line x1="{x:.1f}" y1="4" x2="{x:.1f}" y2="{H - 26}" stroke="#2b2d30"/>')
        out.append(f'<text x="{x:.1f}" y="{H - 8}" fill="#73777b" font-size="12" text-anchor="{"end" if v == hi else "middle"}">{fmt_int(v)}</text>')
    for i, (label, v, seek) in enumerate(rows):
        y = top + row_h * i + row_h / 2
        x = X(max(v, 100))
        out.append(f'<text x="{label_w - 14}" y="{y + 4:.1f}" fill="{"#f0f0ed" if seek else "#a3a4a6"}" font-size="13.5" font-weight="{600 if seek else 400}" text-anchor="end">{esc(label)}</text>')
        out.append(f'<rect x="{label_w}" y="{y - 7:.1f}" width="{x - label_w:.1f}" height="14" rx="4" fill="{"url(#g2)" if seek else "#3a3b43"}"/>')
        out.append(f'<text x="{x + 10:.1f}" y="{y + 4:.1f}" fill="{"#fff" if seek else "#c4c4ce"}" font-size="12.5">{fmt_int(v)} tokens</text>')
    out.append("</svg>")
    return f'<figure class="chart"><figcaption>What the answer costs the agent<span>how many .blend files there are, and where</span></figcaption>{"".join(out)}</figure>'


def build_tables():
    t = {}
    f, ty, cnt, rec = tk("find"), tk("find_typo"), tk("count"), tk("recent")
    t["names"] = table(
        ["tool", "find <code>fuzzy.inl</code>", "with a typo", "count <code>.blend</code>, and where", "tokens of that answer"],
        [["Seek", T(f, "Seek"), ok(row(ty, "Seek")["found"]) + " · " + T(ty, "Seek"), T(cnt, "Seek summary"), K(cnt, "Seek summary")],
         ["Everything", T(f, "Everything"), ok(row(ty, "Everything")["found"]), T(cnt, "Everything, count only") + " for the count; " + T(cnt, "Everything, the list") + " for the list",
          K(cnt, "Everything, the list") + " for the list"],
         ["Windows Search", T(f, "Windows Search") + ", didn't find it", "–", T(cnt, "Windows Search") + f", {row(cnt, 'Windows Search')['count']} of {fmt_int(int(row(cnt, 'Everything, count only')['count']))}", K(cnt, "Windows Search")],
         ["fd, folders in memory", T(f, "fd"), ok(False), T(cnt, "fd"), K(cnt, "fd")],
         ["ripgrep --files, folders in memory", T(f, "ripgrep --files"), ok(False), T(cnt, "ripgrep --files"), K(cnt, "ripgrep --files")],
         ["fd, from disk", "over 15 min", "–", "–", "–"],
         ["dir /s, the profile only", T(f, "dir /s"), ok(False), T(cnt, "dir /s") + ", three drives", K(cnt, "dir /s")],
         ["Get-ChildItem -Recurse, the profile only", T(f, "Get-ChildItem"), ok(False), "–", "–"]],
        seek_rows=(0,))
    gr, gp, fr, frt = tk("grep_repo"), tk("grep_projects"), tk("find_repo"), tk("find_repo_typo")
    t["text"] = table(
        ["tool", "text, " + R["labels"]["grep_repo"], "text, " + R["labels"]["grep_projects"], "a name in the repository", "with a typo"],
        [["Seek, again", T(gr, "Seek, again"), T(gp, "Seek, again"), T(fr, "Seek"), T(frt, "Seek")],
         ["fff", T(gr, "fff"), T(gp, "fff"), T(fr, "fff"), T(frt, "fff")],
         ["Seek, first search", T(gr, "Seek, first search"), T(gp, "Seek, first search"), "–", "–"],
         ["git grep", T(gr, "git grep"), "–", "–", "–"],
         ["ripgrep", T(gr, "ripgrep"), T(gp, "ripgrep"), "–", "–"]],
        seek_rows=(0, 2), nums=(1, 2, 3, 4))
    m = {r["tool"]: r["mb"] for r in ROWS if r["task"] == "memory"}
    st = tk("start")
    t["start"] = table(
        ["", "ready to answer", "memory of its own"],
        [["Seek", T(st, "Seek (daemon + index)") + ", for every drive, from Jolt's index", f"{m['Seek']} MB with every project kept in memory; one process for every agent"],
         ["Seek without Jolt", R["index_build"], "the same"],
         ["Everything", "always running, as a service", f"{m['Everything']} MB"],
         ["fff", T(tk("start_fff_repo"), "fff (scans the folder)") + " for the repository, " + T(tk("start_fff_projects"), "fff (scans the folder)") + " for every project",
          f"{m['fff, ' + R['labels']['repo_short']]} MB for the repository, {m['fff, every project']} MB for every project; one process per agent session and folder"]],
        seek_rows=(0, 1))
    t["recent"] = table(
        ["tool", "time", "what it answers"],
        [["Seek recent", T(rec, "Seek recent"), "the files the user changed and the folders they opened, with system churn left out"],
         ["Everything, newest 10", T(rec, "Everything, newest 10"), "the newest files on the PC: logs, caches and browser data"]],
        seek_rows=(0,))
    t["history"] = table(R["history_cols"], R["history"], nums=(1, 2))
    # the same values as ToolFind in src/tools.inl
    t["ranking"] = table(
        ["what Seek knows", "effect on the score", "shown as"],
        [["changed in the last hour, day, or week", "+3,000, +1,500, +600", "changed 2 h ago"],
         ["in a folder the user opens in Jolt, or up to two levels below it", "up to +1,200, by how much it is used", "in a folder used in Jolt"],
         ["inside the folder given as <code>near</code>", "+2,500; otherwise +150 for each folder level shared with it", "inside seek"],
         ["in a cache, a package store, a log or AppData", "−2,000", "–"],
         ["the name's own score", "fuzzy score × 8, minus its depth", "exact name, program"]])
    return t


def fill(text, charts, tables):
    def sub(m):
        kind, arg = m.group(1), m.group(2)
        if kind == "chart":
            return charts[arg]
        if kind == "table":
            return tables[arg]
        if kind == "mb":
            return str(next(r["mb"] for r in ROWS if r["task"] == "memory" and r["tool"] == arg))
        task, tool = arg.split("|")
        task = TASKS.get(task, task)
        r = row(task, tool)
        if kind == "t":
            return fmt_time(r.get("seconds"))
        if kind == "k":
            return fmt_int(r["tokens"])
        if kind == "n":
            return fmt_int(int(r["count"]))
        raise ValueError(kind)
    text = text.replace("{{VERSION}}", version())
    for k, v in R.get("facts", {}).items():
        text = text.replace("{{" + k + "}}", v)
    text = re.sub(r"\{\{(chart|table|t|k|n|mb):([^}]+)\}\}", sub, text)
    left = re.findall(r"\{\{[^}]+\}\}", text)
    if left:
        raise SystemExit(f"placeholders left: {left[:5]}")
    return text


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(os.path.join(OUT, "media"))
    page = fill(open(os.path.join(SITE, "index.html"), encoding="utf-8").read(), build_charts(), build_tables())
    open(os.path.join(OUT, "index.html"), "w", encoding="utf-8").write(page)
    for f in ("style.css", "CNAME", "InterVariable.woff2", "Inter-LICENSE.txt"):
        shutil.copy(os.path.join(SITE, f), OUT)
    for f in ("seek-256.png", "seek-512.png", "seek.ico"):
        shutil.copy(os.path.join(ROOT, "assets", f), os.path.join(OUT, "media"))
    shutil.copy(os.path.join(ROOT, "assets", "seek.ico"), os.path.join(OUT, "favicon.ico"))
    shutil.copy(os.path.join(ROOT, "install.ps1"), OUT)
    shutil.copy(os.path.join(ROOT, "bench", "results.json"), OUT)
    print("built", OUT)


if __name__ == "__main__":
    main()
