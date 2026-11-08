"""Seek's tests: the tools on test files in %TEMP%\\seek-test, and the MCP protocol over stdio through a background
process of their own (SEEK_HOME points it at the test folder, so the one the user's agents talk to is left alone).
Outside that folder they only read. Run: python -I tests\\test.py"""
import io, json, os, re, shutil, subprocess, sys, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(os.path.dirname(HERE), "build", "seek.exe")
TMP = os.environ["TEMP"]
ROOT = os.path.join(TMP, "seek-test")
os.environ["SEEK_HOME"] = os.path.join(TMP, "seek-test-home")  # every seek.exe started from here: its files and its daemon
CI = bool(os.environ.get("CI"))  # GitHub's machines: no Jolt, and maybe no PDF text filter
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok else "\n      " + detail.replace("\n", "\n      ")[:1500]))
    fails += 0 if ok else 1


def cli(tool, args, via="--cli"):
    r = subprocess.run([EXE, via, tool, json.dumps(args)], capture_output=True, timeout=120)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def put(rel, data):
    p = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data if isinstance(data, bytes) else data.encode("utf-8"))
    return p


def docx(text):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr("[Content_Types].xml", '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="xml" ContentType="application/xml"/>'
                   '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
        z.writestr("_rels/.rels", '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="r1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
        z.writestr("word/document.xml", '<?xml version="1.0"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
                   '<w:body><w:p><w:r><w:t>' + text + '</w:t></w:r></w:p></w:body></w:document>')
    return b.getvalue()


def pdf(text):
    """A one-page PDF with that text, written by hand (no library): enough for Windows' PDF filter."""
    stream = f"BT /F1 24 Tf 72 700 Td ({text}) Tj ET".encode("latin-1")
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
            b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"\nendstream",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for i, o in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    return bytes(out)


subprocess.run([EXE, "--stop"], timeout=60)  # the test daemon of a run that stopped halfway
shutil.rmtree(ROOT, ignore_errors=True)
shutil.rmtree(os.environ["SEEK_HOME"], ignore_errors=True)
os.makedirs(os.path.join(ROOT, "repo", ".git"))
os.makedirs(os.environ["SEEK_HOME"])
put("docs/lease.pdf", pdf("The brown fox signs the lease for the burrow"))
put("repo/.gitignore", "build/\n*.log\n!keep.log\n")
put("repo/src/engine.cpp", "int Engine() {\n    // the quick brown fox\n    return 42;\n}\nvoid RenderFrame() {}\n")
put("repo/src/engine.h", "int Engine();\nvoid RenderFrame();\n")
put("repo/src/notes_utf16.txt", "﻿brown fox in utf-16\n".encode("utf-16-le"))
put("repo/src/cancion.txt", "la canción del zorro marrón\n".encode("cp1252"))
put("repo/build/out.txt", "brown fox in an ignored folder\n")
put("repo/debug.log", "brown fox in an ignored file\n")
put("repo/keep.log", "brown fox kept by a ! rule\n")
put("repo/node_modules/pkg/index.js", "brown fox in dependencies\n")
put("repo/blob.bin", b"brown fox\x00\x01\x02 in a binary")
put("repo/many.txt", "".join(f"line {i} needle\n" for i in range(60)))
put("docs/contract.docx", docx("The tenant pays the brown fox fee every month"))
put("docs/Informe Final 2026.txt", "informe\n")
put("repo/src/.gitignore", "generated.txt\n*.tmp\nscratch/\n")  # no "!" rules: looked up, not matched one by one
put("repo/src/generated.txt", "brown fox generated\n")
put("repo/src/x.tmp", "brown fox in a temp file\n")
put("repo/src/scratch/a.txt", "brown fox in a scratch folder\n")
put("repo/src/sub/scratch", "brown fox in a file named like an ignored folder\n")
put("repo/big.txt", "filler line\n" * 3000 + "the needle at the end of a long file\n")
put("repo/order/a.txt", "ordermark\n")
put("repo/order/z.txt", "ordermark\n")
put("repo/order/sub/m.txt", "ordermark\n")
put("repo/.git/info/exclude", "excluded_dir/\n")
put("repo/excluded_dir/x.txt", "brown fox left out by info/exclude\n")
put("repo/.ignore", "dotignored.txt\n")
put("repo/dotignored.txt", "brown fox left out by .ignore\n")
put("repo/table.md", "| a | b |\n|---|---|\n")
for i in range(25):
    put(f"pages/pagefile {i:02}.txt", "page\n")
put("elsewhere/pagefile 99.txt", "page\n")  # the same name outside pages: left out by "pages\ pagefile"
lines_with = lambda n, word, at: "".join(f"row {i}{' ' + word if i in at else ''}\n" for i in range(1, n + 1))
put("ctx/near.txt", lines_with(30, "marker", {13, 15}))  # closer than twice the context
put("ctx/dense.txt", lines_with(20, "tick", {5, 6, 8, 9}))  # four in five lines
put("ctx/apart.txt", lines_with(30, "spot", {3, 20}))
put("ctx/long.txt", "short\n" + "a" * 100 + ("needle7" + "b" * 263) * 6 + "\nshort\n")  # one line of 1,720 characters
put("bigs/huge.log", "filler\n" * 2500000)  # 17.5 MB: over what grep reads
put("bigs/small.log", "filler\n")

S = ROOT
# ---- find, scoped to the test folder (walked: it was created after the index)
code, out = cli("find", {"query": "engine", "path": S})
check("find: by name in a folder", "repo\\src\\engine.cpp" in out and "repo\\src\\engine.h" in out, out)
code, out = cli("find", {"query": "informe final", "path": S})
check("find: two words", "Informe Final 2026.txt" in out, out)
code, out = cli("find", {"query": "engnie", "path": S})
check("find: a typo still finds it", "engine" in out and "close to what was typed" in out, out)
code, out = cli("find", {"query": ".docx", "path": S})
check("find: by extension", "contract.docx" in out and "engine" not in out, out)
code, out = cli("find", {"query": "*.cpp", "path": S, "summary": True})
check("find: summary counts", "By area" in out and "By extension" in out, out)
code, out = cli("find", {"query": "zzzqqqxxx", "path": S})
check("find: nothing says so", out.startswith("Nothing named like"), out)


def names(out):  # the results of a find answer, without its header and its "more" line
    return [l.split("  ")[0] for l in out.splitlines()[1:] if l and not l.startswith("(")]


PAGES = {"query": "pages\\ pagefile", "path": S}  # the path part leaves one out: it isn't counted either
code, out = cli("find", dict(PAGES, limit=10))
page1 = names(out)
check("find: a first page says how to get the next", "25 matches, the best 10:" in out and "(15 more: offset=10 " in out and len(page1) == 10, out)
code, out = cli("find", dict(PAGES, limit=10, offset=10))
page2 = names(out)
check("find: offset gives the next page", "25 matches, 11 to 20:" in out and "offset=20 " in out and len(page2) == 10, out)
code, out = cli("find", dict(PAGES, limit=10, offset=20))
page3 = names(out)
check("find: the last page says no more", "25 matches, 21 to 25:" in out and "more" not in out and len(page3) == 5, out)
code, out = cli("find", dict(PAGES, limit=25))
check("find: the pages are the whole list, in order", page1 + page2 + page3 == names(out), out)
check("find: limit at the count shows them all, no more", out.splitlines()[0].endswith("25 matches:") and "more" not in out, out)
code, out = cli("find", dict(PAGES, offset=30))
check("find: an offset past the end says so", "offset 30 is past the last one" in out, out)
code, out = cli("find", dict(PAGES, summary=True))
check("find: summary counts what it shows", out.splitlines()[0].endswith(": 25 matches.") and "about" not in out, out)
code, out = cli("find", {"query": ".gitignore", "path": S})
check("find: a dotfile by its own name", "repo\\.gitignore" in out and "repo\\src\\.gitignore" in out, out)
code, out = cli("find", {"query": "src/engine.cpp", "path": S})
check("find: a path with the name at its end", "repo\\src\\engine.cpp" in out and "engine.h" not in out, out)
code, out = cli("find", {"query": "src/*.h", "path": S})
check("find: a path with a wildcard at its end", "repo\\src\\engine.h" in out and "engine.cpp" not in out, out)
code, out = cli("find", {"query": "src/", "path": S})
check("find: a path alone asks for a name", code != 0 and "where to look" in out, out)
code, out = cli("find", {"query": "*.{cpp,h}", "path": S})
check("find: braces in a wildcard", "engine.cpp" in out and "engine.h" in out and "notes_utf16" not in out, out)

# ---- grep
code, out = cli("grep", {"pattern": "brown fox", "path": S + "\\repo"})
check("grep: finds the text", "engine.cpp" in out and "2: // the quick brown fox" in out, out)
check("grep: other encodings", "notes_utf16.txt" in out, out)
check("grep: .gitignore folders and files", "out.txt" not in out and "debug.log" not in out, out)
check("grep: ! rule in .gitignore", "keep.log" in out, out)
check("grep: dependencies skipped", "node_modules" not in out, out)
check("grep: binaries skipped", "blob.bin" not in out, out)
code, out = cli("grep", {"pattern": "canción", "path": S + "\\repo"})
check("grep: ANSI text", "cancion.txt" in out, out)
code, out = cli("grep", {"pattern": "Engine", "path": S + "\\repo", "context": 0})
check("grep: smart case (capital = exact case)", "engine.cpp" in out and "1: int Engine() {" in out, out)
code, out = cli("grep", {"pattern": "ENGINE", "path": S + "\\repo", "case": "sensitive"})
check("grep: exact case with no match falls back to ignoring case", "ignoring case" in out and "engine.cpp" in out, out)
code, out = cli("grep", {"pattern": r"Render\w+", "path": S + "\\repo", "regex": True})
check("grep: regex", "RenderFrame" in out, out)
code, out = cli("grep", {"pattern": "Engine|RenderFrame", "path": S + "\\repo", "context": 0})
check("grep: a literal with regex signs and no match is tried as a regex", "| is a regex sign, so as a regex" in out and "5: void RenderFrame() {}" in out, out)
code, out = cli("grep", {"pattern": "zzqq|xxyy", "path": S + "\\repo"})
check("grep: nothing as a regex either says so", "Nothing as a regex either" in out, out)
code, out = cli("grep", {"pattern": "zzqq(|", "path": S + "\\repo"})
check("grep: a regex that doesn't compile says so", "Tried as a regex too" in out and "doesn't compile" in out, out)
code, out = cli("grep", {"pattern": "| a |", "path": S + "\\repo"})
check("grep: a literal | that matches stays literal", "table.md" in out and "regex sign" not in out, out)
code, out = cli("grep", {"pattern": "src\\bin", "path": S + "\\repo"})
check("grep: a backslash inside a path isn't a regex sign", "regex sign" not in out, out)
code, out = cli("grep", {"pattern": "Engine", "path": S + "\\repo", "glob": "*.{cpp,h}", "context": 0})
check("grep: braces in a glob", "engine.cpp" in out and "engine.h" in out and "searched 2 files" in out, out)
code, out = cli("grep", {"pattern": "fox", "path": S + "\\repo", "glob": "*.nothing"})
check("grep: a glob that leaves every file out says so", "the glob left every file out" in out, out)
code, out = cli("grep", {"pattern": "filler", "path": S + "\\bigs\\huge.log"})
check("grep: a file over 16 MB given directly is named as not read", "1 file over 16 MB wasn't read: huge.log (17 MB)" in out, out)
code, out = cli("grep", {"pattern": "filler", "path": S + "\\bigs"})
check("grep: a file over 16 MB met on a walk is named too", "small.log" in out and "wasn't read: huge.log (17 MB)" in out, out)


def numbered(out):  # the line numbers an answer writes, with ':' (a match) or '-' (context)
    return [m.groups() for m in re.finditer(r"^  (\d+)([:-]) ", out, re.M)]


code, out = cli("grep", {"pattern": "marker", "path": S + "\\ctx\\near.txt", "context": 3})
check("grep: close matches share their context, each line once",
      numbered(out) == [(str(n), ":" if n in (13, 15) else "-") for n in range(10, 19)] and "--" not in out, out)
check("grep: the header says how long it took, and 1 file", re.search(r"\(searched 1 file in \d+ ms", out) is not None, out)
code, out = cli("grep", {"pattern": "tick", "path": S + "\\ctx\\dense.txt", "context": 1})
check("grep: four matches in five lines, one block", numbered(out) == [(str(n), ":" if n in (5, 6, 8, 9) else "-") for n in range(4, 11)], out)
code, out = cli("grep", {"pattern": "spot", "path": S + "\\ctx\\apart.txt", "context": 1})
check("grep: groups that don't touch are split by --", "  4- row 4\n  --\n  19- row 19" in out, out)
code, out = cli("grep", {"pattern": "spot", "path": S + "\\ctx\\apart.txt", "context": 0})
check("grep: no -- without context", "--" not in out and len(numbered(out)) == 2, out)
for args in ({"pattern": "needle7"}, {"pattern": r"needle\d", "regex": True}):
    code, out = cli("grep", dict(args, path=S + "\\ctx\\long.txt", context=0))
    check("grep: a long line shows a window for each match, then counts the rest" + (" (regex)" if "regex" in args else ""),
          [m.group(1) for m in re.finditer(r"col (\d+): …a?b*needle7", out)] == ["371", "641", "911"] and "(2 more matches in this line)" in out
          and out.split("\n", 1)[1].count("needle7") == 4, out)
code, out = cli("grep", {"pattern": "fox", "path": S + "\\repo", "glob": "*.cpp"})
check("grep: glob include", "engine.cpp" in out and "notes_utf16" not in out, out)
code, out = cli("grep", {"pattern": "fox", "path": S + "\\repo", "glob": ["!*.txt"]})
check("grep: glob exclude", "notes_utf16" not in out and "engine.cpp" in out, out)
code, out = cli("grep", {"pattern": "needle", "path": S + "\\repo\\many.txt", "per_file": 60, "limit": 10, "context": 0})
check("grep: one file, a page", "60 matching lines" in out and "More: offset=10" in out, out)
code, out = cli("grep", {"pattern": "needle", "path": S + "\\repo\\many.txt", "per_file": 60, "limit": 10, "offset": 50, "context": 0})
check("grep: offset pages", "51: line 50 needle" in out and "More:" not in out, out)
code, out = cli("grep", {"pattern": "fox", "path": S + "\\repo", "summary": True})
check("grep: summary", "Most matches" in out, out)
code, out = cli("grep", {"pattern": "signs the lease", "path": S + "\\docs"})
pdf_ok = "lease.pdf" in out and "text taken from the document" in out
check("grep: a PDF, through Windows' text filter" + ("" if pdf_ok else " (no PDF text filter on this machine)"), pdf_ok or (CI and "couldn't be read" in out), out)
code, out = cli("grep", {"pattern": "fee every month", "path": S + "\\docs"})
docs_ok = "contract.docx" in out
check("grep: a Word document" + ("" if docs_ok else " (no .docx text filter on this PC?)"), docs_ok or "couldn't be read" in out, out)
code, out = cli("grep", {"pattern": "engine", "path": S + "\\docs"})
check("grep: no text, names hinted", "No text matches" in out or "0 matching lines" in out, out)
code, out = cli("grep", {"pattern": "brown fox", "path": S + "\\repo", "limit": 50})
check("grep: .gitignore names, *.ext and folder/ rules", "generated.txt" not in out and "x.tmp" not in out and "scratch\\a.txt" not in out
      and "sub\\scratch" in out, out)
check("grep: .git/info/exclude and .ignore", "excluded_dir" not in out and "dotignored.txt" not in out, out)
code, out = cli("grep", {"pattern": "brown fox", "path": S + "\\repo", "limit": 60, "ignored": True})
check("grep: ignored searches all of that", all(n in out for n in ("excluded_dir", "dotignored.txt", "out.txt", "debug.log", "node_modules", "generated.txt")), out)
code, out = cli("grep", {"pattern": "brown fox", "path": S + "\\repo\\excluded_dir"})
check("grep: a path inside an ignored folder is searched", "x.txt" in out, out)
code, out = cli("grep", {"pattern": "needle at the end", "path": S + "\\repo"})
check("grep: a long file, read past its start", "big.txt" in out and "3001:" in out, out)
code, out = cli("grep", {"pattern": "QUICK brown", "path": S + "\\repo", "case": "insensitive"})
check("grep: ignoring case", "engine.cpp" in out and "ignoring case" not in out, out)
code, out = cli("grep", {"pattern": "ordermark", "path": S + "\\repo\\order"})
at = [out.find(n) for n in ("a.txt", "z.txt", "sub\\m.txt")]
check("grep: walk order, a folder's files before its subfolders", -1 not in at and at == sorted(at), out)
code, out = cli("grep", {"pattern": "x", "path": os.path.join(os.environ["USERPROFILE"], ".ssh")})
check("grep: private places refused", code != 0 and ("private" in out or "Not found" in out), out)

# ---- recent: the dates on disk below a folder
code, out = cli("recent", {"path": S, "since": "1h", "limit": 100})
check("recent: dates on disk", "engine.cpp" in out and "modified" in out, out)
code, out = cli("recent", {"path": S, "since": "1h", "query": ".docx"})
check("recent: filtered by name", "contract.docx" in out and "engine.cpp" not in out, out)

# ---- MCP over stdio, through the background process
p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def rpc(obj):
    p.stdin.write((json.dumps(obj) + "\n").encode("utf-8"))
    p.stdin.flush()
    if "id" not in obj:
        return None
    return json.loads(p.stdout.readline().decode("utf-8"))
r = rpc({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "seek-test", "version": "1"}}})
check("mcp: initialize", r.get("result", {}).get("serverInfo", {}).get("name") == "seek" and "instructions" in r["result"], json.dumps(r))
rpc({"jsonrpc": "2.0", "method": "notifications/initialized"})
r = rpc({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
names = [t["name"] for t in r.get("result", {}).get("tools", [])]
check("mcp: tools/list", names == ["find", "grep", "recent"], json.dumps(r)[:500])
t0 = time.time()
r = rpc({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "find", "arguments": {"query": "engine.cpp", "path": S}}})
text = r.get("result", {}).get("content", [{}])[0].get("text", "")
check("mcp: tools/call find (starts the background process)", "engine.cpp" in text and r["result"]["isError"] is False, json.dumps(r)[:800])
print("      (first call, with the start, %.2f s)" % (time.time() - t0))
t0 = time.time()
# on all drives: with Jolt, its index is there at once; without it, the daemon walks the drives first
end = time.time() + 600
for i in range(100000):
    r = rpc({"jsonrpc": "2.0", "id": 1000 + i, "method": "tools/call", "params": {"name": "find", "arguments": {"query": "notepad.exe", "limit": 3}}})
    text = r.get("result", {}).get("content", [{}])[0].get("text", "")
    if "Still indexing" not in text or time.time() > end:
        break
    time.sleep(2)
print("      (the index of every drive ready after %.1f s)" % (time.time() - t0))
check("mcp: find on all drives, a program first", "notepad.exe" in text.lower() and "program" in text, text)
t0 = time.time()
r = rpc({"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "find", "arguments": {"query": "notepad.exe", "limit": 3}}})
check("mcp: find on all drives, warm", "notepad.exe" in r["result"]["content"][0]["text"].lower(), json.dumps(r)[:800])
print("      (warm, %.3f s)" % (time.time() - t0))
r = rpc({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "grep", "arguments": {"pattern": "quick brown", "path": S + "\\repo"}}})
check("mcp: tools/call grep", "engine.cpp" in r["result"]["content"][0]["text"], json.dumps(r)[:800])
# a change while the background process watches: recent sees it
time.sleep(1.0)
put("repo/src/fresh_change.cpp", "// just written\n")
time.sleep(1.5)
r = rpc({"jsonrpc": "2.0", "id": 6, "method": "tools/call", "params": {"name": "recent", "arguments": {"since": "10m", "include_system": True, "query": "fresh_change"}}})
check("mcp: recent sees a change live", "fresh_change.cpp" in r["result"]["content"][0]["text"], r["result"]["content"][0]["text"])
put("repo/build/fresh_output.txt", "made by a build\n")
put("repo/src/scratch/fresh_scratch.txt", "in a folder .gitignore leaves out\n")
time.sleep(1.5)
r = rpc({"jsonrpc": "2.0", "id": 61, "method": "tools/call", "params": {"name": "recent", "arguments": {"since": "10m", "query": "fresh", "path": S}}})
text = r["result"]["content"][0]["text"]
check("mcp: recent leaves out what .gitignore does", "fresh_change.cpp" in text and "fresh_output" not in text and "fresh_scratch" not in text, text)
r = rpc({"jsonrpc": "2.0", "id": 62, "method": "tools/call", "params": {"name": "recent", "arguments": {"since": "10m", "query": "fresh", "path": S, "include_system": True}}})
text = r["result"]["content"][0]["text"]
check("mcp: recent with include_system shows it", "fresh_output" in text and "fresh_scratch" in text, text)
r = rpc({"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "find", "arguments": {"query": "fresh_change"}}})
check("mcp: find knows a file created after the index", "fresh_change.cpp" in r["result"]["content"][0]["text"], r["result"]["content"][0]["text"])
os.remove(os.path.join(ROOT, "repo", "src", "fresh_change.cpp"))
time.sleep(1.5)
r = rpc({"jsonrpc": "2.0", "id": 71, "method": "tools/call", "params": {"name": "find", "arguments": {"query": "fresh_change"}}})
check("mcp: and forgets it when it is deleted", "fresh_change.cpp" not in r["result"]["content"][0]["text"], r["result"]["content"][0]["text"])
put("repo/src/fresh_change.cpp", "// written again\n")
time.sleep(1.5)
r = rpc({"jsonrpc": "2.0", "id": 72, "method": "tools/call", "params": {"name": "find", "arguments": {"query": "fresh_change"}}})
check("mcp: and knows it again when it is made again, once", r["result"]["content"][0]["text"].count("fresh_change.cpp") == 1, r["result"]["content"][0]["text"])
r = rpc({"jsonrpc": "2.0", "id": 8, "method": "tools/call", "params": {"name": "nope", "arguments": {}}})
check("mcp: unknown tool is a protocol error", r.get("error", {}).get("code") == -32602, json.dumps(r))
r = rpc({"jsonrpc": "2.0", "id": 9, "method": "ping"})
check("mcp: ping", r.get("result") == {}, json.dumps(r))
r = rpc({"jsonrpc": "2.0", "id": 10, "method": "resources/list"})
check("mcp: unknown method", r.get("error", {}).get("code") == -32601, json.dumps(r))
r = rpc({"jsonrpc": "2.0", "id": 11, "method": "initialize", "params": {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "seek-test", "version": "1"}}})
check("mcp: an older protocol version is kept", r.get("result", {}).get("protocolVersion") == "2024-11-05", json.dumps(r))
r = rpc({"jsonrpc": "2.0", "id": 12, "method": "initialize", "params": {"protocolVersion": "1.0", "capabilities": {}, "clientInfo": {"name": "seek-test", "version": "1"}}})
check("mcp: an unknown protocol version gets Seek's latest", r.get("result", {}).get("protocolVersion", "").startswith("20"), json.dumps(r))
check("mcp: server info", r.get("result", {}).get("serverInfo", {}).get("websiteUrl") == "https://seek.synrgs.tech", json.dumps(r))
p.stdin.write((json.dumps([{"jsonrpc": "2.0", "id": 13, "method": "ping"}, {"jsonrpc": "2.0", "method": "notifications/initialized"},
                           {"jsonrpc": "2.0", "id": 14, "method": "tools/list"}]) + "\n").encode("utf-8"))
p.stdin.flush()
r = json.loads(p.stdout.readline().decode("utf-8"))
check("mcp: a batch gets one reply per request", isinstance(r, list) and [x.get("id") for x in r] == [13, 14], json.dumps(r)[:300])
r = rpc({"jsonrpc": "2.0", "id": 15, "method": "tools/list"})
annotations = [t.get("annotations", {}) for t in r["result"]["tools"]]
check("mcp: every tool only reads", all(a.get("readOnlyHint") and a.get("destructiveHint") is False for a in annotations), json.dumps(annotations))
p.stdin.close()
p.wait(timeout=10)
log = open(os.path.join(os.environ["SEEK_HOME"], "log.txt"), encoding="utf-8", errors="replace").read()
check("log: the calls are written down", "seek-test  find" in log, log[-800:])

# ---- folders kept in memory by the background process, and kept current by its watchers
put("live/a.txt", "alpha one\n")
put("live/sub/b.txt", "alpha two\n")
put("live/.gitignore", "ign/\n")
put("live/ign/c.txt", "alpha ignored\n")
put("live/mv/f.txt", "alpha moved\n")
L = os.path.join(ROOT, "live")


def g(pattern="alpha"):
    return cli("grep", {"pattern": pattern, "path": L, "limit": 50}, via="--call")[1]


def wait(pattern, cond, timeout=10):  # the watchers see a change a moment after it is made
    end = time.time() + timeout
    while True:
        out = g(pattern)
        if cond(out) or time.time() > end:
            return out
        time.sleep(0.2)


g()  # the first search walks the folder, and then it is read into memory
out = wait("alpha", lambda o: "from Seek's memory" in o)
check("cache: a folder searched again comes from memory", "from Seek's memory" in out and "a.txt" in out and "b.txt" in out and "c.txt" not in out, out)
time.sleep(0.5)
put("live/a.txt", "beta one\n")
out = wait("beta", lambda o: "a.txt" in o)
check("cache: a changed file is read again", "a.txt" in out and "from Seek's memory" in out, out)
put("live/new.txt", "alpha new\n")
out = wait("alpha", lambda o: "new.txt" in o)
check("cache: a new file is added", "new.txt" in out and "from Seek's memory" in out, out)
os.remove(os.path.join(L, "sub", "b.txt"))
out = wait("alpha", lambda o: "b.txt" not in o)
check("cache: a deleted file goes", "b.txt" not in out and "from Seek's memory" in out, out)
os.rename(os.path.join(L, "mv"), os.path.join(L, "moved"))
out = wait("alpha", lambda o: "moved\\f.txt" in o)
check("cache: a renamed folder is followed", "moved\\f.txt" in out and "mv\\f.txt" not in out and "from Seek's memory" in out, out)
put("live/ign/c2.txt", "alpha ignored too\n")
time.sleep(1)
out = g()
check("cache: what .gitignore leaves out stays out", "c2.txt" not in out and "from Seek's memory" in out, out)
put("live/.gitignore", "ign/\nnew.txt\n")
out = wait("alpha", lambda o: "new.txt" not in o)
check("cache: a changed .gitignore applies", "new.txt" not in out and "moved\\f.txt" in out, out)
out = cli("grep", {"pattern": "alpha", "path": L, "ignored": True, "limit": 50}, via="--call")[1]
check("cache: ignored still walks the folder", "c2.txt" in out and "from Seek's memory" not in out, out)


# ---- the background process and the sessions of other builds: an older one is served, a newer one makes it step aside
def pipe_call(build):
    key = os.environ["SEEK_HOME"].replace("/", "\\").rstrip("\\").lower()  # Key(SeekHome()), hashed as InstanceTag does
    h = 2166136261
    for ch in key:
        h = ((h ^ ord(ch)) * 16777619) & 0xFFFFFFFF
    req = json.dumps({"build": build, "tool": "find", "args": {"query": "engine.cpp", "path": S}, "client": "test"}).encode()
    with open(r"\\.\pipe\seek-%s-%08x" % (os.environ["USERNAME"], h), "r+b", buffering=0) as p:
        p.write(len(req).to_bytes(4, "little") + req)
        data = b""
        while len(data) < 4 or len(data) < 4 + int.from_bytes(data[:4], "little"):
            data += p.read(65536)
        return json.loads(data[4:])


r = pipe_call("0.0.1 Jan  1 2026 00:00:00")
check("daemon: a session of an older Seek is served", "engine.cpp" in r.get("text", ""), json.dumps(r)[:300])
r = pipe_call("99.0.0 Jan  1 2030 00:00:00")
check("daemon: a session of a newer Seek makes it step aside", r.get("restart") is True, json.dumps(r)[:300])
time.sleep(0.5)
code, out = cli("find", {"query": "engine.cpp", "path": S}, via="--call")
check("daemon: and the next call starts it again", "engine.cpp" in out, out)
subprocess.run([EXE, "--stop"], timeout=60)
shutil.rmtree(ROOT, ignore_errors=True)
shutil.rmtree(os.environ["SEEK_HOME"], ignore_errors=True)
print("\n%s" % ("all passed" if not fails else "%d failed" % fails))
sys.exit(1 if fails else 0)
