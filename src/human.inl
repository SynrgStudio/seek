// The command line for people: "seek report pdf", "seek g TODO src", "seek r 2h", and "seek i report pdf", which asks
// step by step: each line typed (-p ~/Projects, -n 50, a word more, -word) changes the search and shows it again.
// It asks the background process with the commands for programs (PROTOCOL.md), whose rows don't change between
// versions, and draws them: in colors in a terminal, one path a line in a pipe.

// ---- the terminal

struct Term {
    HANDLE out = nullptr;
    bool console = false;  // stdout is a console (not a pipe or a file)
    bool color = false;
    bool pretty = false;  // colors and numbers, not one path a line: a console, or --color always
    bool links = false;  // clickable paths (OSC 8): Windows Terminal, VS Code, WezTerm
    int width = 120;
};
Term g_term;
std::wstring g_cwd, g_home;

bool EnvSet(const wchar_t* name) {
    wchar_t buf[4];
    return GetEnvironmentVariableW(name, buf, 4) > 0;
}

std::wstring EnvText(const wchar_t* name) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n && n < 1024 ? std::wstring(buf, n) : L"";
}

void TermInit(const std::wstring& color) {
    g_term.out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    g_term.console = GetFileType(g_term.out) == FILE_TYPE_CHAR && GetConsoleMode(g_term.out, &mode);
    bool vt = false;
    if (g_term.console) {
        vt = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) || SetConsoleMode(g_term.out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        CONSOLE_SCREEN_BUFFER_INFO bi;
        if (GetConsoleScreenBufferInfo(g_term.out, &bi)) g_term.width = std::max(40, bi.srWindow.Right - bi.srWindow.Left + 1);
    }
    g_term.color = color == L"always" || (color != L"never" && vt && !EnvSet(L"NO_COLOR"));
    g_term.pretty = g_term.console || color == L"always";
    const std::wstring prog = Lower(EnvText(L"TERM_PROGRAM"));
    g_term.links = g_term.color && g_term.console && (EnvSet(L"WT_SESSION") || prog == L"vscode" || prog == L"wezterm");
    wchar_t d[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, d)) g_cwd = Normalize(d);
    g_home = Normalize(EnvText(L"USERPROFILE"));
}

std::wstring g_outBuf;  // what is printed goes out at once: thousands of small writes to a console are slow

void Flush() {
    if (g_outBuf.empty()) return;
    DWORD w;
    if (g_term.console) {
        for (size_t at = 0; at < g_outBuf.size(); at += 8192)
            WriteConsoleW(g_term.out, g_outBuf.data() + at, static_cast<DWORD>(std::min<size_t>(8192, g_outBuf.size() - at)), &w, nullptr);
    } else {
        const std::string u = Utf8(g_outBuf);
        WriteFile(g_term.out, u.data(), static_cast<DWORD>(u.size()), &w, nullptr);
    }
    g_outBuf.clear();
}

void Out(const std::wstring& s) {
    g_outBuf += s;
    if (g_outBuf.size() > (1 << 16)) Flush();
}

// The colors: the terminal's own 16, so they suit its theme, light or dark.
namespace Ink {
const wchar_t* const kGray = L"90";      // folders on the way, counts, hints
const wchar_t* const kMatch = L"1;33";   // the letters of a name that match
const wchar_t* const kFolder = L"1;34";  // a folder found
const wchar_t* const kFile = L"35";      // grep: the file
const wchar_t* const kLine = L"32";      // grep: the line number
const wchar_t* const kHit = L"1;31";     // grep: the text that matches
const wchar_t* const kTime = L"36";      // recent: when
const wchar_t* const kNote = L"33";      // close, not exact: a typo, letters scattered
const wchar_t* const kError = L"1;31";
const wchar_t* const kPrompt = L"1;36";
const wchar_t* const kBold = L"1";
}  // namespace Ink

std::wstring Paint(const wchar_t* ink, const std::wstring& s) {
    if (!g_term.color || s.empty()) return s;
    return std::wstring(L"\x1b[") + ink + L"m" + s + L"\x1b[0m";
}

std::wstring FileUrl(const std::wstring& path) {
    std::wstring u = L"file:///";
    for (const unsigned char c : Utf8(path)) {
        if (isalnum(c) || strchr("-._~:/", c)) u += static_cast<wchar_t>(c);
        else if (c == '\\') u += L'/';
        else {
            wchar_t b[4];
            swprintf(b, 4, L"%%%02X", c);
            u += b;
        }
    }
    return u;
}

// Text that opens the path when clicked, where the terminal can.
std::wstring Linked(const std::wstring& path, const std::wstring& text) {
    if (!g_term.links) return text;
    return L"\x1b]8;;" + FileUrl(path) + L"\x1b\\" + text + L"\x1b]8;;\x1b\\";
}

// A path as a person reads it: below the current folder, from there; below the home folder, from ~.
std::wstring ShortPath(const std::wstring& p) {
    if (!g_cwd.empty() && IsUnder(Key(p), Key(g_cwd)) && p.size() > g_cwd.size()) return Relative(p, g_cwd);
    if (!g_home.empty() && IsUnder(Key(p), Key(g_home))) return L"~" + p.substr(g_home.size());
    return p;
}

// What the person typed as a folder, as a full path: ~ is the home folder, the rest is from the current folder.
std::wstring FullPath(std::wstring p) {
    if (p.empty()) return p;
    if (p[0] == L'~' && (p.size() == 1 || p[1] == L'\\' || p[1] == L'/')) p = g_home + p.substr(1);
    wchar_t buf[4096];
    const DWORD n = GetFullPathNameW(p.c_str(), 4096, buf, nullptr);
    return Normalize(n && n < 4096 ? std::wstring(buf, n) : p);
}

bool LooksLikePath(const std::wstring& t) {
    if (t == L"." || t == L".." || t == L"~") return true;
    if (t.size() >= 2 && (t[1] == L':' && iswalpha(t[0])) && (t.size() == 2 || t[2] == L'\\' || t[2] == L'/')) return true;
    for (const wchar_t* start : {L".\\", L"./", L"..\\", L"../", L"~\\", L"~/", L"\\\\"})
        if (t.rfind(start, 0) == 0) return true;
    return false;
}

bool IsSpan(const std::wstring& t) {  // 30m, 2h, 3d, 2w
    if (t.size() < 2 || !wcschr(L"mhdw", towlower(t.back()))) return false;
    for (size_t i = 0; i + 1 < t.size(); ++i)
        if (!iswdigit(t[i])) return false;
    return true;
}

// ---- what is asked

struct Ask {
    wchar_t tool = L'f';  // f: find, g: grep, r: recent
    std::vector<std::wstring> words;  // find and recent: the name's words; grep: the pattern's, joined by spaces
    std::wstring path, nearTo, since, glob, without;
    std::vector<std::wstring> exts;
    int type = 0;  // 1 folders, 2 files
    int kase = 0;  // grep: 1 ignore case, 2 match it, 0 as Seek decides (capitals match it)
    int limit = 0;  // 0: as fits the screen
    bool summary = false, all = false, regex = false, raw = false, open = false;
};

int Limit(const Ask& a) {
    if (a.limit > 0) return a.limit;
    if (!g_term.pretty) return a.tool == L'g' ? 2000 : 1000;
    return a.tool == L'g' ? 50 : 30;
}

std::wstring Quoted(const std::wstring& s) { return s.find_first_of(L" \t\"") == std::wstring::npos && !s.empty() ? s : L"\"" + s + L"\""; }

std::wstring JoinWords(const std::vector<std::wstring>& w, const wchar_t* sep = L" ") {
    std::wstring s;
    for (const auto& x : w) s += (s.empty() ? L"" : sep) + x;
    return s;
}

// The same search as one command, to type again or keep in a script.
std::wstring CommandLine(const Ask& a) {
    std::wstring s = L"seek";
    if (a.tool == L'g') s += L" g";
    if (a.tool == L'r') s += L" r";
    if (a.tool == L'g') s += L" " + Quoted(JoinWords(a.words));
    else
        for (const auto& w : a.words) s += L" " + Quoted(w);
    if (a.tool == L'r' && !a.since.empty()) s += L" " + a.since;
    if (!a.path.empty()) s += (a.tool == L'g' ? L" " : L" -p ") + Quoted(ShortPath(a.path));
    if (!a.exts.empty()) s += L" -e " + JoinWords(a.exts, L",");
    if (a.type == 1) s += L" -d";
    if (a.type == 2) s += L" -f";
    if (a.kase == 1) s += L" -i";
    if (a.kase == 2) s += L" -c";
    if (a.regex) s += L" -x";
    if (a.all) s += L" -a";
    if (a.summary) s += L" -s";
    if (!a.glob.empty()) s += L" -g " + Quoted(a.glob);  // several, joined by commas: one -g says them all
    if (!a.without.empty()) s += L" -v " + Quoted(a.without);
    if (!a.nearTo.empty()) s += L" --near " + Quoted(ShortPath(a.nearTo));
    if (a.limit > 0) s += L" -n " + std::to_wstring(a.limit);
    return s;
}

// What the prompt shows of the search: what it looks for, and where.
std::wstring AskText(const Ask& a) {
    std::wstring s = a.tool == L'g' ? L"grep " + Quoted(JoinWords(a.words)) : a.tool == L'r' ? L"recent" : JoinWords(a.words);
    if (a.tool == L'r') {
        if (!a.words.empty()) s += L" " + JoinWords(a.words);
        s += L" " + (a.since.empty() ? std::wstring(L"7d") : a.since);
    }
    std::vector<std::wstring> more;
    if (!a.exts.empty()) more.push_back(JoinWords(a.exts, L","));
    if (a.tool == L'g' || !a.path.empty()) more.push_back(ShortPath(a.path.empty() ? g_cwd : a.path));
    if (a.type) more.push_back(a.type == 1 ? L"folders" : L"files");
    if (a.kase) more.push_back(a.kase == 1 ? L"any case" : L"case");
    if (a.regex) more.push_back(L"regex");
    if (a.all) more.push_back(L"all");
    if (!a.glob.empty()) more.push_back(a.glob);
    if (!a.without.empty()) more.push_back(L"without " + a.without);
    if (a.limit) more.push_back(L"-n " + std::to_wstring(a.limit));
    if (a.summary) more.push_back(L"summary");
    for (const auto& m : more) s += L" · " + m;
    return Trim(s);
}

struct Parsed {
    std::wstring error;
    std::wstring color;  // --color
    bool help = false;
};

// Applies what was typed to the search: flags, words, a folder. On the command line once; in "seek i", line after
// line, so a flag given again undoes itself (-d, then -d: folders, then everything again) and -word takes a word away.
Parsed Apply(Ask& a, const std::vector<std::wstring>& tokens, bool stepping) {
    Parsed r;
    std::vector<std::wstring> loose;  // not flags
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::wstring& t = tokens[i];
        auto value = [&](std::wstring& into) {
            if (i + 1 < tokens.size()) into = tokens[++i];
            else if (stepping) into.clear();  // "-p" alone in a step: not below any folder any more
            else r.error = t + L" needs a value";
        };
        auto toggle = [&](bool& b) { b = stepping ? !b : true; };
        auto toggleType = [&](int v) { a.type = stepping && a.type == v ? 0 : v; };
        auto toggleCase = [&](int v) { a.kase = stepping && a.kase == v ? 0 : v; };
        if (t == L"/?") {
            r.help = true;
            continue;
        }
        if (t.size() < 2 || t[0] != L'-' || (t.size() > 1 && iswdigit(t[1]))) {
            loose.push_back(t);
            continue;
        }
        if (t == L"-h" || t == L"--help") r.help = true;
        else if (t == L"-p" || t == L"--path") {
            std::wstring p;
            value(p);
            a.path = FullPath(p);
        } else if (t == L"--near") {
            std::wstring p;
            value(p);
            a.nearTo = FullPath(p);
        } else if (t == L"-n" || t == L"--limit") {
            std::wstring n;
            value(n);
            a.limit = n.empty() ? 0 : std::clamp(_wtoi(n.c_str()), 0, 100000);
            if (!n.empty() && a.limit == 0) r.error = L"-n takes how many: -n 50";
        } else if (t == L"-e" || t == L"--ext") {
            std::wstring e;
            value(e);
            a.exts.clear();
            for (size_t at = 0; at < e.size();) {
                size_t end = e.find(L',', at);
                if (end == std::wstring::npos) end = e.size();
                std::wstring x = Trim(e.substr(at, end - at));
                if (!x.empty() && x[0] == L'.') x.erase(0, 1);
                if (!x.empty()) a.exts.push_back(Lower(x));
                at = end + 1;
            }
        } else if (t == L"-g" || t == L"--glob") {  // each one more: -g *.md -g !*.min.js; "-g" alone in a step, none
            std::wstring g;
            value(g);
            a.glob = g.empty() ? L"" : a.glob.empty() ? g : a.glob + L"," + g;
        }
        else if (t == L"-v" || t == L"--without") value(a.without);
        else if (t == L"-t" || t == L"--since") value(a.since);
        else if (t == L"--color") {
            value(r.color);
            if (r.color != L"always" && r.color != L"never" && r.color != L"auto") r.error = L"--color is always, never or auto";
        } else if (t == L"--rows") toggle(a.raw);
        else if (t == L"-o" || t == L"--open") toggle(a.open);
        else if (t == L"-d" || t == L"--dirs" || t == L"--folders") toggleType(1);
        else if (t == L"-f" || t == L"--files") toggleType(2);
        else if (t == L"-s" || t == L"--summary") toggle(a.summary);
        else if (t == L"-a" || t == L"--all") toggle(a.all);
        else if (t == L"-x" || t == L"--regex") toggle(a.regex);
        else if (t == L"-i" || t == L"--ignore-case") toggleCase(1);
        else if (t == L"-c" || t == L"--case") toggleCase(2);
        else if (t[1] != L'-' && t.find_first_not_of(L"dfsaxico", 1) == std::wstring::npos) {  // -ds: -d and -s
            for (size_t k = 1; k < t.size(); ++k) {
                switch (t[k]) {
                    case L'd': toggleType(1); break;
                    case L'f': toggleType(2); break;
                    case L's': toggle(a.summary); break;
                    case L'a': toggle(a.all); break;
                    case L'x': toggle(a.regex); break;
                    case L'i': toggleCase(1); break;
                    case L'c': toggleCase(2); break;
                    case L'o': toggle(a.open); break;
                }
            }
        } else if (stepping && t[1] != L'-') {  // -word: that word out of the search
            const std::wstring w = Lower(t.substr(1));
            const auto it = std::find_if(a.words.begin(), a.words.end(), [&](const std::wstring& x) { return Lower(x) == w; });
            if (it == a.words.end()) r.error = L"Not a flag, nor a word of the search: " + t;
            else a.words.erase(it);
        } else {
            r.error = L"Unknown flag: " + t + L" (seek -h lists them)";
        }
        if (!r.error.empty()) return r;
    }
    // The rest: what to look for, and where
    for (size_t k = 0; k < loose.size(); ++k) {
        const std::wstring& t = loose[k];
        if (a.tool == L'r' && IsSpan(t)) a.since = t;
        else if (LooksLikePath(t)) a.path = FullPath(t);
        else if (a.tool == L'g' && !stepping && !a.words.empty() && a.path.empty()) a.path = FullPath(t);  // seek g TODO src
        else a.words.push_back(t);
    }
    return r;
}

// Splits a typed line into words, keeping "what is in quotes" together.
std::vector<std::wstring> Tokens(const std::wstring& line) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quoted = false, any = false;
    for (const wchar_t c : line) {
        if (c == L'"') {
            quoted = !quoted;
            any = true;
        } else if (iswspace(c) && !quoted) {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

// ---- asking Seek

// A command for programs, through the background process (started when it isn't running), or in this process when
// it can't be reached.
ToolOut AskSeek(const std::string& cmd, const std::string& argText) {
    const std::string req = "{\"api\":1,\"build\":" + JsonQuote(kBuild) + ",\"tool\":" + JsonQuote(cmd) + ",\"args\":" + argText + "}";
    if (!g_local) {
        HANDLE pipe = ConnectDaemon(0);
        if (!pipe) {
            StartDaemon();
            pipe = ConnectDaemon(15000);
        }
        if (pipe) {
            std::string reply;
            const bool got = WriteFrame(pipe, req) && ReadFrame(pipe, reply);
            CloseHandle(pipe);
            Json j;
            if (got && JsonParse(reply, j)) {
                const Json* t = j.at("text");
                const Json* e = j.at("error");
                const Json* c = j.at("code");
                ToolOut o{t ? Wide(t->s) : L"", e && e->b};
                if (c && c->kind == Json::Str) o.code = Wide(c->s);
                return o;
            }
        }
        g_local = true;
    }
    if (cmd == "opened") return {L"ok"};
    LocalReady();
    Json args;
    JsonParse(argText, args);
    try {
        if (cmd == "grep.rows") return ToolGrep(args, true);
        if (cmd == "recent.rows") return ToolRecent(args, true);
        const Json* m = args.at("max");
        return ToolFind(args, m && m->kind == Json::Num ? static_cast<size_t>(std::clamp(m->n, 1.0, 100000.0)) : 5000);
    } catch (const std::exception& e) {
        return {L"Seek failed: " + Wide(e.what()), true};
    }
}

std::string Field(const char* k, const std::wstring& v) { return std::string(",\"") + k + "\":" + JsonQuote(Utf8(v)); }
std::string Field(const char* k, int v) { return std::string(",\"") + k + "\":" + std::to_string(v); }
std::string Field(const char* k, bool v) { return std::string(",\"") + k + "\":" + (v ? "true" : "false"); }

std::wstring NameQueryText(const Ask& a) {
    std::wstring q;
    for (const auto& w : a.words)  // "final draft", one word given in quotes, is looked for as typed
        q += (q.empty() ? L"" : L" ") + (w.find(L' ') != std::wstring::npos && w.find(L'"') == std::wstring::npos ? L"\"" + w + L"\"" : w);
    if (!a.exts.empty()) q += (q.empty() ? L"" : L" ") + std::wstring(L"ext:") + JoinWords(a.exts, L",");
    return q;
}

std::vector<std::vector<std::wstring>> RowsOf(const std::wstring& text, std::vector<std::wstring>& head) {
    std::vector<std::vector<std::wstring>> rows;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::vector<std::wstring> f;
        for (size_t s = at; s <= end;) {
            size_t tab = text.find(L'\t', s);
            if (tab == std::wstring::npos || tab > end) tab = end;
            f.push_back(text.substr(s, tab - s));
            s = tab + 1;
        }
        if (!f.empty() && f[0] == L"#rows") head = f;
        else if (!(f.size() == 1 && f[0].empty())) rows.push_back(f);
        at = end + 1;
    }
    return rows;
}

// ---- drawing the results

struct Shown {  // a result on screen, by its number: what "3", "o 3" and "c 3" act on
    std::wstring path;
    int line = 0;
};

// The letters of a name that the words typed match, to light them up.
std::vector<bool> Marks(const std::wstring& name, const NameQuery& q) {
    std::vector<bool> on(name.size());
    std::vector<int> pos;
    for (const auto& w : q.words) {
        int score = 0;
        if (FuzzyMatch(name.c_str(), name.size(), w, score, &pos) || NearPositions(name.c_str(), name.size(), w, 50, pos))
            for (const int p : pos)
                if (p >= 0 && static_cast<size_t>(p) < name.size()) on[p] = true;
    }
    const std::wstring ln = Lower(name);
    for (const auto& e : q.exts)
        if (ln.size() >= e.size() && ln.compare(ln.size() - e.size(), e.size(), e) == 0)
            for (size_t k = ln.size() - e.size(); k < ln.size(); ++k) on[k] = true;
    return on;
}

std::wstring Lit(const std::wstring& s, const std::vector<bool>& on, const wchar_t* base, const wchar_t* hit) {
    if (!g_term.color) return s;
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        size_t j = i;
        while (j < s.size() && on[j] == on[i]) ++j;
        out += on[i] || base ? Paint(on[i] ? hit : base, s.substr(i, j - i)) : s.substr(i, j - i);
        i = j;
    }
    return out;
}

std::wstring Number(size_t n, size_t width) {
    std::wstring s = std::to_wstring(n);
    return L"  " + Paint(Ink::kGray, std::wstring(width > s.size() ? width - s.size() : 0, L' ') + s) + L"  ";
}

// A found path: its folder dim, its name bright, the letters typed lit.
std::wstring PathLine(const std::wstring& path, bool folder, const NameQuery& q) {
    const std::wstring shown = ShortPath(path);
    const size_t cut = shown.find_last_of(L'\\');
    std::wstring dir = cut == std::wstring::npos ? L"" : shown.substr(0, cut + 1);
    const std::wstring leaf = cut == std::wstring::npos ? shown : shown.substr(cut + 1);
    const size_t room = static_cast<size_t>(std::max(20, g_term.width - 8));
    if (g_term.console && dir.size() + leaf.size() > room && leaf.size() + 12 < room)  // too long for a line: its middle goes
        dir = dir.substr(0, 6) + L"…" + dir.substr(dir.size() - (room - leaf.size() - 7));
    std::wstring s = Paint(Ink::kGray, dir) + Lit(leaf, Marks(leaf, q), folder ? Ink::kFolder : nullptr, Ink::kMatch);
    if (folder) s += Paint(Ink::kFolder, L"\\");
    return Linked(path, s);
}

std::wstring Footer(size_t from, size_t to, const std::wstring& total, bool more) {
    std::wstring s = total;
    if (to > from && (from > 0 || more)) s += L" · showing " + (from > 0 ? std::to_wstring(from + 1) + L"–" : std::wstring()) + std::to_wstring(to);
    return Paint(Ink::kGray, s) + L"\n";
}

struct Page {
    std::vector<Shown> shown;  // what has a number, from the first page on
    bool more = false;  // Enter shows more
    bool found = false;
    bool error = false;
};

void Fail(const std::wstring& text) { Out(Paint(Ink::kError, Trim(text)) + L"\n"); }

void Summary(const std::vector<std::vector<std::wstring>>& rows, const std::wstring& total) {
    std::map<std::wstring, size_t> exts, dirs;
    size_t files = 0, folders = 0;
    for (const auto& r : rows) {
        if (r.size() < 2) continue;
        const bool folder = r[0] == L"D";
        (folder ? folders : files)++;
        const std::wstring e = folder ? L"folder" : ExtOf(LeafOf(r[1]));
        ++exts[e.empty() ? L"(none)" : e];
        ++dirs[ParentOf(r[1])];
    }
    auto top = [](const std::map<std::wstring, size_t>& m, bool paths) {
        std::vector<std::pair<std::wstring, size_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
        std::wstring s;
        const size_t n = std::min<size_t>(v.size(), paths ? 8 : 10);
        for (size_t i = 0; i < n; ++i)
            s += L"   " + Paint(Ink::kBold, Thousands(v[i].second)) + L"  " + (paths ? Linked(v[i].first, ShortPath(v[i].first)) : v[i].first) + L"\n";
        if (v.size() > n) s += Paint(Ink::kGray, L"   … " + Thousands(v.size() - n) + L" more\n");
        return s;
    };
    Out(Paint(Ink::kBold, total) + Paint(Ink::kGray, L"  (" + Thousands(files) + L" files, " + Thousands(folders) + L" folders)") + L"\n");
    Out(Paint(Ink::kGray, L"by type\n") + top(exts, false));
    Out(Paint(Ink::kGray, L"by folder\n") + top(dirs, true));
}

void ShowFind(const Ask& a, Page& pg, size_t from, bool stepping) {
    const NameQuery q = ParseNameQuery(NameQueryText(a));
    if (!q.Names() && q.pathParts.empty()) {
        Fail(L"What to look for? A word of the name, or -e pdf.");
        pg.error = true;
        return;
    }
    const size_t n = static_cast<size_t>(Limit(a));
    std::string args = "{\"query\":" + JsonQuote(Utf8(NameQueryText(a)));
    if (!a.path.empty()) args += Field("path", a.path);
    if (!a.nearTo.empty()) args += Field("near", a.nearTo);
    if (a.type) args += Field("type", std::wstring(a.type == 1 ? L"folder" : L"file"));
    if (a.all) args += Field("ignored", true);
    args += Field("max", static_cast<int>(a.summary ? 100000 : std::min<size_t>(from + n, 100000))) + "}";
    const ToolOut o = AskSeek("find.rows", args);
    if (o.error) {
        Fail(o.text);
        pg.error = true;
        return;
    }
    if (a.raw) {
        Out(o.text);
        pg.found = true;
        return;
    }
    std::vector<std::wstring> head;
    const auto rows = RowsOf(o.text, head);
    const size_t total = head.size() > 1 ? static_cast<size_t>(_wtoi64(head[1].c_str())) : rows.size();
    const bool exact = head.size() > 2 && head[2] == L"1";
    const std::wstring kind = head.size() > 3 ? head[3] : L"real";
    pg.found = !rows.empty();
    const std::wstring count = (exact ? L"" : L"about ") + Thousands(exact ? total : std::max(total, rows.size())) + L" found";
    if (!g_term.pretty && !stepping && !a.summary) {  // a pipe: the paths, and nothing else
        for (size_t i = from; i < rows.size(); ++i)
            if (rows[i].size() > 1) Out(rows[i][1] + L"\n");
        return;
    }
    if (rows.empty()) {
        Out(Paint(Ink::kGray, L"Nothing found" + (a.path.empty() ? std::wstring() : L" below " + ShortPath(a.path)) +
                                  (a.all || a.path.empty() ? L"." : L". -a also looks in ignored folders.")) + L"\n");
        return;
    }
    if (a.summary) {
        Summary(rows, count);
        return;
    }
    if (from == 0 && kind == L"near") Out(Paint(Ink::kNote, L"No name has that; these are close to it, as if it had a typo:") + L"\n");
    if (from == 0 && kind == L"scattered")
        Out(Paint(Ink::kNote, L"Nothing named like that; these have its letters scattered along them, most likely by chance:") + L"\n");
    const size_t to = std::min(rows.size(), from + n);
    const size_t width = std::to_wstring(to).size();
    for (size_t i = from; i < to; ++i) {
        if (rows[i].size() < 2) continue;
        if (stepping) {
            pg.shown.push_back({rows[i][1]});
            Out(Number(pg.shown.size(), width));
        } else {
            Out(L"  ");
        }
        Out(PathLine(rows[i][1], rows[i][0] == L"D", q) + L"\n");
    }
    pg.more = to < rows.size() || (rows.size() >= from + n && (!exact || total > to));
    Out(Footer(from, to, count, pg.more));
    if (pg.more && !stepping) Out(Paint(Ink::kGray, L"  -n " + std::to_wstring(std::min<size_t>(to * 4, 100000)) + L" shows more\n"));
}

// Where a line's text matches what was asked, to light it up.
std::vector<bool> HitMarks(const std::wstring& text, const Ask& a, const std::wstring& how) {
    std::vector<bool> on(text.size());
    const std::wstring pat = JoinWords(a.words);
    if (pat.empty()) return on;
    bool icase = a.kase == 1 || how == L"nocase";
    if (a.kase == 0 && std::none_of(pat.begin(), pat.end(), iswupper)) icase = true;
    if (a.regex || how == L"regex") {
        try {
            const std::wregex re(pat, icase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript);
            for (auto it = std::wsregex_iterator(text.begin(), text.end(), re); it != std::wsregex_iterator(); ++it) {
                if (it->length(0) == 0) continue;
                for (auto k = it->position(0); k < it->position(0) + it->length(0); ++k) on[static_cast<size_t>(k)] = true;
            }
        } catch (const std::regex_error&) {
        }
        return on;
    }
    const std::wstring t = icase ? Lower(text) : text, p = icase ? Lower(pat) : pat;
    for (size_t at = t.find(p); at != std::wstring::npos; at = t.find(p, at + p.size()))
        for (size_t k = at; k < at + p.size(); ++k) on[k] = true;
    return on;
}

void ShowGrep(const Ask& a, Page& pg, size_t from, bool stepping) {
    if (a.words.empty()) {
        Fail(L"What text? seek g TODO src");
        pg.error = true;
        return;
    }
    const size_t n = static_cast<size_t>(Limit(a));
    std::string args = "{\"pattern\":" + JsonQuote(Utf8(JoinWords(a.words))) + Field("path", a.path.empty() ? g_cwd : a.path);
    if (a.regex) args += Field("regex", true);
    if (a.kase) args += Field("case", std::wstring(a.kase == 1 ? L"insensitive" : L"sensitive"));
    std::wstring gl = a.glob;  // *.md,*.txt or "*.md *.txt"; -e cpp,h is *.cpp and *.h
    for (const auto& e : a.exts) gl += L",*." + e;
    if (!Trim(gl).empty()) {
        std::replace(gl.begin(), gl.end(), L',', L' ');
        std::string list;
        for (const auto& g : Tokens(gl)) list += (list.empty() ? "" : ",") + JsonQuote(Utf8(g));
        args += ",\"glob\":[" + list + "]";
    }
    if (!a.without.empty()) args += Field("without", a.without);
    if (a.all) args += Field("ignored", true) + Field("hidden", true);
    args += Field("limit", static_cast<int>(std::min<size_t>(from + n, 100000))) + "}";
    const ToolOut o = AskSeek("grep.rows", args);
    if (o.error) {
        Fail(o.text);
        pg.error = true;
        return;
    }
    if (a.raw) {
        Out(o.text);
        pg.found = true;
        return;
    }
    std::vector<std::wstring> head;
    auto rows = RowsOf(o.text, head);
    const std::wstring how = head.size() > 4 ? head[4] : L"asked";
    if (a.kase == 2 && how == L"nocase") rows.clear();  // -c: this case, not the lines Seek tries next in any case
    const bool cut = head.size() > 3 && head[3] == L"1";
    pg.found = !rows.empty();
    if (!g_term.pretty && !stepping) {  // a pipe: path:line:text, as editors and ripgrep's users expect
        for (size_t i = from; i < rows.size(); ++i)
            if (rows[i].size() > 3) Out(rows[i][0] + L":" + rows[i][1] + L":" + rows[i][3] + L"\n");
        return;
    }
    if (rows.empty()) {
        Out(Paint(Ink::kGray, L"No line has it below " + ShortPath(a.path.empty() ? g_cwd : a.path) + L".") + L"\n");
        return;
    }
    if (from == 0 && how == L"nocase") Out(Paint(Ink::kNote, L"Not in that case; in any case:") + L"\n");
    if (from == 0 && how == L"regex") Out(Paint(Ink::kNote, L"Not as typed; as a regex:") + L"\n");
    const size_t to = std::min(rows.size(), from + n);
    const size_t width = std::to_wstring(to).size();
    std::wstring file;
    for (size_t i = from; i < to; ++i) {
        const auto& r = rows[i];
        if (r.size() < 4) continue;
        if (r[0] != file) {
            file = r[0];
            Out((i > from ? L"\n" : L"") + Linked(file, Paint(Ink::kFile, ShortPath(file))) + L"\n");
        }
        if (stepping) {
            pg.shown.push_back({r[0], _wtoi(r[1].c_str())});
            Out(Number(pg.shown.size(), width));
        } else {
            Out(L"  ");
        }
        Out(Paint(Ink::kLine, r[1]) + Paint(Ink::kGray, L":") + L" " + Lit(r[3], HitMarks(r[3], a, how), nullptr, Ink::kHit) + L"\n");
    }
    const size_t files = head.size() > 1 ? static_cast<size_t>(_wtoi64(head[1].c_str())) : 0;
    const size_t lines = head.size() > 2 ? static_cast<size_t>(_wtoi64(head[2].c_str())) : rows.size();
    pg.more = to < rows.size() || (cut && to >= rows.size() && rows.size() >= from + n);
    std::wstring count = Thousands(lines) + (lines == 1 ? L" line in " : L" lines in ") + Thousands(files) + (files == 1 ? L" file" : L" files");
    if (cut) count += L", not all of them listed";
    Out(L"\n" + Footer(from, to, count, pg.more));
}

std::wstring PadRight(const std::wstring& s, size_t n) { return s.size() >= n ? s : s + std::wstring(n - s.size(), L' '); }

void ShowRecent(const Ask& a, Page& pg, size_t from, bool stepping) {
    const size_t n = static_cast<size_t>(Limit(a));
    std::string args = "{\"since\":" + JsonQuote(Utf8(a.since.empty() ? std::wstring(L"7d") : a.since));
    if (!a.path.empty()) args += Field("path", a.path);
    if (!NameQueryText(a).empty()) args += Field("query", NameQueryText(a));
    if (a.all) args += Field("include_system", true);
    args += Field("limit", static_cast<int>(std::min<size_t>(from + n, 100000))) + "}";
    const ToolOut o = AskSeek("recent.rows", args);
    if (o.error) {
        Fail(o.text);
        pg.error = true;
        return;
    }
    if (a.raw) {
        Out(o.text);
        pg.found = true;
        return;
    }
    std::vector<std::wstring> head;
    const auto rows = RowsOf(o.text, head);
    pg.found = !rows.empty();
    if (!g_term.pretty && !stepping) {
        for (size_t i = from; i < rows.size(); ++i)
            if (rows[i].size() > 1) Out(rows[i][1] + L"\n");
        return;
    }
    if (rows.empty()) {
        Out(Paint(Ink::kGray, L"Nothing in that time" + (a.path.empty() ? std::wstring() : L" below " + ShortPath(a.path)) + L".") + L"\n");
        return;
    }
    const NameQuery q = ParseNameQuery(NameQueryText(a));
    const size_t to = std::min(rows.size(), from + n);
    const size_t width = std::to_wstring(to).size();
    for (size_t i = from; i < to; ++i) {
        const auto& r = rows[i];
        if (r.size() < 4) continue;
        const int64_t ft = (_wtoi64(r[2].c_str()) + 11644473600LL) * kSecond;
        const wchar_t* ink = r[3] == L"created" ? Ink::kLine : r[3] == L"renamed" ? Ink::kFile : r[3] == L"opened" ? Ink::kFolder : Ink::kNote;
        if (stepping) {
            pg.shown.push_back({r[1]});
            Out(Number(pg.shown.size(), width));
        } else {
            Out(L"  ");
        }
        Out(Paint(Ink::kTime, PadRight(Ago(ft), 13)) + Paint(ink, PadRight(r[3], 9)) + PathLine(r[1], r[0] == L"D", q) + L"\n");
    }
    const size_t total = head.size() > 1 ? static_cast<size_t>(_wtoi64(head[1].c_str())) : rows.size();
    pg.more = to < rows.size() || total > to;
    Out(Footer(from, to, Thousands(total) + L" changed or opened", pg.more));
}

void Show(const Ask& a, Page& pg, size_t from, bool stepping) {
    if (a.tool == L'g') ShowGrep(a, pg, from, stepping);
    else if (a.tool == L'r') ShowRecent(a, pg, from, stepping);
    else ShowFind(a, pg, from, stepping);
    Flush();
}

// ---- acting on a result

void Opened(const std::wstring& path) { AskSeek("opened", "{\"path\":" + JsonQuote(Utf8(path)) + "}"); }

// Opens a file with what Windows opens it with; with SEEK_EDITOR set (code -g {path}:{line}), with that.
void OpenPath(const Shown& s) {
    const std::wstring editor = EnvText(L"SEEK_EDITOR");
    if (!editor.empty() && !(GetFileAttributesW(s.path.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring cmd = editor;
        auto put = [&](const std::wstring& k, const std::wstring& v) {
            for (size_t at = cmd.find(k); at != std::wstring::npos; at = cmd.find(k, at + v.size())) cmd.replace(at, k.size(), v);
        };
        if (cmd.find(L"{path}") == std::wstring::npos) cmd += L" \"{path}\"";
        put(L"{line}", std::to_wstring(std::max(1, s.line)));
        put(L"{path}", s.path);
        std::wstring line = L"cmd.exe /d /c " + cmd;
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    } else {
        ShellExecuteW(nullptr, L"open", s.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    Opened(s.path);
}

void Reveal(const std::wstring& path) {
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

bool CopyText(const std::wstring& s) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

// ---- seek i: step by step

bool ReadLine(std::wstring& line) {
    line.clear();
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(in, &mode)) {
        wchar_t buf[1024];
        DWORD got = 0;
        do {
            if (!ReadConsoleW(in, buf, 1024, &got, nullptr) || got == 0) return !line.empty();
            line.append(buf, got);
        } while (line.back() != L'\n');
    } else {
        std::string u;
        char c;
        DWORD got = 0;
        while (ReadFile(in, &c, 1, &got, nullptr) && got == 1 && c != '\n') u += c;
        if (u.empty() && got == 0) return false;
        line = Wide(u);
    }
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
    if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);  // PowerShell starts what it pipes with a BOM
    return true;
}

const wchar_t* const kStepHelp =
    L"Each line changes the search and shows it again:\n"
    L"  word         one more word: fewer results          -word   that word out again\n"
    L"  !word        leave out names with it               .pdf    that extension\n"
    L"  -p folder    only below it (-p alone: everywhere)  -n 50   how many to show\n"
    L"  -d  -f       folders or files only (again: both)   -s      counts by type and folder\n"
    L"  -e pdf,docx  those extensions                      -a      also ignored folders\n"
    L"  g text       look for the text inside the files (-i any case, -x regex, -g *.md)\n"
    L"  f words      find names again                      r 2h    what changed lately\n"
    L"  Enter        more results       3  open the 3rd    o 3  show it in its folder    c 3  copy its path\n"
    L"  u            one step back      ?  the search as one command      q  quit\n";

int StepByStep(Ask a) {
    g_term.pretty = true;  // numbered, a screen at a time, even read from a pipe
    std::vector<Ask> before;
    Page pg;
    size_t from = 0;
    auto run = [&](bool fresh) {
        if (fresh) {
            pg = Page{};
            from = 0;
        }
        Show(a, pg, from, true);
    };
    if (a.words.empty() && a.exts.empty() && a.tool != L'r') Out(Paint(Ink::kGray, L"Type what to look for (h for help, q to quit).") + L"\n");
    else run(true);
    const bool echo = !g_term.console;  // read from a pipe: the lines show in what is printed
    for (;;) {
        Out(L"\n" + Paint(Ink::kPrompt, (a.words.empty() && a.exts.empty() && a.tool != L'r' ? L"seek" : AskText(a)) + L" ›") + L" ");
        Flush();
        std::wstring line;
        if (!ReadLine(line)) break;
        if (echo) Out(line + L"\n");
        line = Trim(line);
        const std::vector<std::wstring> t = Tokens(line);
        const std::wstring first = t.empty() ? L"" : Lower(t[0]);
        if (line.empty()) {
            if (!pg.more) {
                Out(Paint(Ink::kGray, L"That is all.") + L"\n");
                continue;
            }
            from += static_cast<size_t>(Limit(a));
            Show(a, pg, from, true);
            continue;
        }
        if (first == L"q" || first == L"quit" || first == L"exit") break;
        if (first == L"h" || first == L"help" || first == L"-h") {
            Out(kStepHelp);
            continue;
        }
        if (first == L"?") {
            Out(CommandLine(a) + L"\n");
            continue;
        }
        if (first == L"u") {
            if (before.empty()) {
                Out(Paint(Ink::kGray, L"This is the first step.") + L"\n");
                continue;
            }
            a = before.back();
            before.pop_back();
            run(true);
            continue;
        }
        const bool num = std::all_of(line.begin(), line.end(), iswdigit);
        if (num || ((first == L"o" || first == L"c") && t.size() == 2 && std::all_of(t[1].begin(), t[1].end(), iswdigit))) {
            const size_t k = static_cast<size_t>(_wtoi((num ? line : t[1]).c_str()));
            if (k == 0 || k > pg.shown.size()) {
                Out(Paint(Ink::kError, L"No result " + std::to_wstring(k) + L" on screen.") + L"\n");
                continue;
            }
            const Shown& s = pg.shown[k - 1];
            if (num) {
                OpenPath(s);
                Out(Paint(Ink::kGray, L"Opened " + ShortPath(s.path)) + L"\n");
            } else if (first == L"o") {
                Reveal(s.path);
                Out(Paint(Ink::kGray, L"Shown in its folder: " + ShortPath(s.path)) + L"\n");
            } else {
                Out(Paint(Ink::kGray, (CopyText(s.path) ? L"Copied: " : L"Couldn't copy: ") + s.path) + L"\n");
            }
            continue;
        }
        Ask next = a;
        std::vector<std::wstring> rest = t;
        if (first == L"g" || first == L"f" || first == L"r") {  // another tool: where and how many stay
            next = Ask{};
            next.tool = first[0];
            next.path = a.path;
            next.limit = a.limit;
            next.all = a.all;
            if (first == L"g" && a.tool == L'f' && a.path.empty() && a.type == 1 && !pg.shown.empty()) next.path = pg.shown[0].path;
            rest.erase(rest.begin());
        }
        const Parsed p = Apply(next, rest, true);
        if (!p.error.empty()) {
            Out(Paint(Ink::kError, p.error) + L"\n");
            continue;
        }
        if (p.help) {
            Out(kStepHelp);
            continue;
        }
        before.push_back(a);
        a = next;
        run(true);
    }
    Flush();
    return 0;
}

// ---- seek ...

const wchar_t* const kHumanHelp =
    L"Seek " SEEK_VERSION L": find files by name, and text inside them, on this PC.\n"
    L"\n"
    L"  seek report pdf            names with those words, on every drive (typos are forgiven)\n"
    L"  seek report -p ~/Docs      only below a folder (or give it as .\\x, ~\\x, C:\\x)\n"
    L"  seek g TODO src            the lines with that text in the files below src (. if none)\n"
    L"  seek r 2h                  what changed or was opened in the last 2 hours (30m, 3d, 2w)\n"
    L"  seek i report pdf          step by step: each line typed changes the search (h there for help)\n"
    L"\n"
    L"  -p folder   only below it                -n 50     how many\n"
    L"  -d / -f     folders / files only          -e pdf,md those extensions\n"
    L"  -s          counts by type and folder     -a        also ignored folders and system places\n"
    L"  -o          open the first one            --near x  rank what is near that folder first\n"
    L"  grep:  -i any case   -c this case   -x regex   -g *.md (or !*.min.js)   -v text (lines without it)\n"
    L"  --color always|never   --rows (the rows of PROTOCOL.md, for scripts)\n"
    L"  In names: !word leaves it out, .pdf an extension, \"as typed\", * and ?, src/name below a folder called src.\n"
    L"\n"
    L"In a pipe it prints one path a line (grep: path:line:text):  seek report pdf | select -first 1 | ii\n"
    L"SEEK_EDITOR opens results in an editor:  $env:SEEK_EDITOR = 'code -g {path}:{line}'\n"
    L"\n"
    L"For AI agents and programs:\n"
    L"  seek                         MCP server: what an agent runs\n"
    L"  seek --cli <tool> [json]     run a tool here: find, grep or recent\n"
    L"  seek --call <tool> [json]    the same, through the shared background process\n"
    L"  seek --stop [here]           stop the background process (here: only if it runs from this file)\n"
    L"  seek --status                how it is: indexes, watchers, folders in memory\n"
    L"  seek --stats [days|all]      what agents asked, by day: calls, times, what found nothing\n"
    L"  seek --reindex [C D ...]     walk the drives again now (only where Jolt's index isn't used)\n"
    L"  seek --version\n";

int HumanMain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args(argv + 1, argv + argc);
    Ask a;
    bool stepping = false;
    if (!args.empty()) {
        const std::wstring first = Lower(args[0]);
        if (first == L"i") stepping = true;
        if (first == L"i" || first == L"f" || first == L"find") args.erase(args.begin());
        else if (first == L"g" || first == L"grep") a.tool = L'g', args.erase(args.begin());
        else if (first == L"r" || first == L"recent") a.tool = L'r', args.erase(args.begin());
        if (stepping && !args.empty()) {  // seek i g TODO, seek i r 2h
            const std::wstring t = Lower(args[0]);
            if (t == L"g" || t == L"r" || t == L"f") a.tool = t[0], args.erase(args.begin());
        }
    }
    std::wstring color;  // --color is read first: the error about another flag is drawn with it
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == L"--color") color = args[i + 1];
    TermInit(color);
    const Parsed p = Apply(a, args, false);
    if (p.help) {
        Out(kHumanHelp);
        Flush();
        return 0;
    }
    if (!p.error.empty()) {
        Fail(p.error);
        Flush();
        return 2;
    }
    if (stepping) return StepByStep(a);
    if (a.words.empty() && a.exts.empty() && a.tool == L'f') {
        Out(kHumanHelp);
        Flush();
        return 2;
    }
    if (a.open) {  // the first one, opened
        a.limit = 1;
        const bool pretty = g_term.pretty;
        g_term.pretty = true;  // a numbered list, so the first is known
        Page pg;
        Show(a, pg, 0, true);
        g_term.pretty = pretty;
        if (!pg.shown.empty()) OpenPath(pg.shown[0]);
        return pg.error ? 2 : pg.shown.empty() ? 1 : 0;
    }
    Page pg;
    Show(a, pg, 0, false);
    return pg.error ? 2 : pg.found ? 0 : 1;
}
