// Content search under a folder (or in one file): text and code in any encoding, and PDF/Office documents through the
// text filters Windows has installed (IFilter). It follows .gitignore (also the ones above the folder, up to the
// repository's root), skips dependency and cache folders, binaries and private places, and stops at a time budget.

// ---- .gitignore

struct IgnRule {
    std::wstring pat;  // lowercase, '\' separators
    bool dirOnly = false, anchored = false, negate = false;
};
struct IgnSet {
    std::wstring baseKey;
    std::vector<IgnRule> rules;
    // Without "!" rules the order doesn't matter, and most rules are plain names or "*.ext": those are looked up
    // instead of matched one by one against every entry.
    bool negates = false;
    std::unordered_set<std::wstring> names, dirNames;
    std::vector<std::wstring> suffixes;  // "*.log" -> ".log"
    std::vector<IgnRule> globs;  // the rest
};
using IgnChain = std::vector<std::shared_ptr<const IgnSet>>;

// The rules of one ignore file, for the folder they are relative to.
std::shared_ptr<const IgnSet> LoadIgnore(const std::wstring& file, const std::wstring& dir) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return nullptr;
    auto set = std::make_shared<IgnSet>();
    set->baseKey = Key(dir);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::wstring l = Wide(line);
        while (!l.empty() && l.back() == L' ' && !(l.size() > 1 && l[l.size() - 2] == L'\\')) l.pop_back();
        if (l.empty() || l[0] == L'#') continue;
        IgnRule r;
        if (l[0] == L'!') {
            r.negate = true;
            l = l.substr(1);
        } else if (l.size() > 1 && l[0] == L'\\' && (l[1] == L'#' || l[1] == L'!')) {
            l = l.substr(1);
        }
        std::replace(l.begin(), l.end(), L'/', L'\\');
        if (!l.empty() && l.back() == L'\\') {
            r.dirOnly = true;
            l.pop_back();
        }
        if (l.empty()) continue;
        if (l.find(L'\\') != std::wstring::npos) {
            r.anchored = true;
            if (l[0] == L'\\') l = l.substr(1);
        }
        r.pat = Lower(l);
        set->rules.push_back(std::move(r));
    }
    if (set->rules.empty()) return nullptr;
    for (const IgnRule& r : set->rules) set->negates = set->negates || r.negate;
    for (const IgnRule& r : set->rules) {
        const bool wild = r.pat.find_first_of(L"*?[") != std::wstring::npos;
        if (!r.anchored && !wild) (r.dirOnly ? set->dirNames : set->names).insert(r.pat);
        else if (!r.anchored && !r.dirOnly && r.pat.size() > 2 && r.pat.compare(0, 2, L"*.") == 0 && r.pat.find_first_of(L"*?[", 1) == std::wstring::npos)
            set->suffixes.push_back(r.pat.substr(1));
        else set->globs.push_back(r);
    }
    return set;
}

bool Ignored(const IgnChain& chain, const std::wstring& key, const std::wstring& nameKey, bool dir) {
    bool ign = false;
    for (const auto& s : chain) {
        if (key.size() <= s->baseKey.size() + 1) continue;
        const wchar_t* rel = key.c_str() + s->baseKey.size() + (s->baseKey.back() == L'\\' ? 0 : 1);
        if (!s->negates) {  // only "ignore" rules: once one matches, nothing here can undo it
            if (ign) continue;
            ign = s->names.count(nameKey) || (dir && s->dirNames.count(nameKey));
            for (size_t i = 0; !ign && i < s->suffixes.size(); ++i) ign = nameKey.ends_with(s->suffixes[i]);
            for (size_t i = 0; !ign && i < s->globs.size(); ++i) {
                const IgnRule& r = s->globs[i];
                ign = !(r.dirOnly && !dir) && GlobMatch(r.pat.c_str(), r.anchored ? rel : nameKey.c_str());
            }
            continue;
        }
        for (const IgnRule& r : s->rules) {
            if ((r.dirOnly && !dir) || ign == !r.negate) continue;  // can't change the answer
            if (GlobMatch(r.pat.c_str(), r.anchored ? rel : nameKey.c_str())) ign = !r.negate;
        }
    }
    return ign;
}

// The user's own excludes for every repository: core.excludesFile in .gitconfig, or git's default place for it.
const std::wstring& GlobalExcludes() {
    static const std::wstring path = [] {
        const std::wstring home = Expand(L"%USERPROFILE%");
        std::ifstream in(home + L"\\.gitconfig", std::ios::binary);
        std::string line;
        bool core = false;
        while (std::getline(in, line)) {
            std::wstring l = Lower(Trim(Wide(line)));
            if (!l.empty() && l[0] == L'[') core = l.rfind(L"[core]", 0) == 0;
            const size_t eq = l.find(L'=');
            if (!core || eq == std::wstring::npos || Trim(l.substr(0, eq)) != L"excludesfile") continue;
            std::wstring v = Trim(Wide(line).substr(Wide(line).find(L'=') + 1));
            if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
            if (!v.empty() && v[0] == L'~') v = home + v.substr(1);
            return Normalize(Expand(v));
        }
        const std::wstring xdg = Expand(L"%XDG_CONFIG_HOME%");
        return (xdg.find(L'%') == std::wstring::npos && !xdg.empty() ? xdg : home + L"\\.config") + L"\\git\\ignore";
    }();
    return path;
}

// The ignore files that start applying in a folder. In a repository's root, first the user's global excludes and the
// repository's .git\info\exclude; then .gitignore, then .ignore (the file ripgrep, fd and fff also follow). Later
// rules win, as in git.
void AddIgnores(const std::wstring& dir, IgnChain& chain) {
    const DWORD git = GetFileAttributesW(Join(dir, L".git").c_str());
    if (git != INVALID_FILE_ATTRIBUTES) {
        if (auto s = LoadIgnore(GlobalExcludes(), dir)) chain.push_back(s);
        if (git & FILE_ATTRIBUTE_DIRECTORY)
            if (auto s = LoadIgnore(Join(dir, L".git\\info\\exclude"), dir)) chain.push_back(s);
    }
    if (auto s = LoadIgnore(Join(dir, L".gitignore"), dir)) chain.push_back(s);
    if (auto s = LoadIgnore(Join(dir, L".ignore"), dir)) chain.push_back(s);
}

// The ignore files that apply to root from above it: from the repository's root down.
IgnChain IgnoresAbove(const std::wstring& root) {
    std::vector<std::wstring> dirs;
    for (std::wstring d = ParentOf(root); !d.empty(); d = ParentOf(d)) {
        dirs.push_back(d);
        if (GetFileAttributesW(Join(d, L".git").c_str()) != INVALID_FILE_ATTRIBUTES) break;
        if (d.size() <= 3) {  // no repository above: no .gitignore applies from there
            dirs.clear();
            break;
        }
    }
    IgnChain chain;
    for (size_t i = dirs.size(); i-- > 0;) AddIgnores(dirs[i], chain);
    return chain;
}

// Is a path left out by the ignore files of the repository it is in, itself or a folder on the way to it? Outside a
// repository nothing is. The answers are kept per folder, for a tool that asks about many paths (recent).
class RepoIgnore {
public:
    bool Check(const std::wstring& path, bool dir) {
        const std::wstring parent = ParentOf(path);
        if (parent.empty() || parent.size() >= path.size()) return false;
        const Dir* d = Get(parent);
        if (!d) return false;
        return d->ignored || Ignored(d->chain, Key(path), Lower(LeafOf(path)), dir);
    }

private:
    struct Dir {
        bool inRepo = false, ignored = false;
        IgnChain chain;  // the rules for its entries
    };
    std::unordered_map<std::wstring, Dir> dirs_;  // by key; pointers to its elements stay valid as it grows

    const Dir* Get(const std::wstring& dir) {
        const std::wstring key = Key(dir);
        if (auto it = dirs_.find(key); it != dirs_.end()) return it->second.inRepo ? &it->second : nullptr;
        Dir d;
        if (GetFileAttributesW(Join(dir, L".git").c_str()) != INVALID_FILE_ATTRIBUTES) {  // a repository's root
            d.inRepo = true;
            AddIgnores(dir, d.chain);
        } else if (dir.size() > 3) {
            const std::wstring up = ParentOf(dir);
            if (const Dir* p = up.empty() || up.size() >= dir.size() ? nullptr : Get(up)) {
                d.inRepo = true;
                d.ignored = p->ignored || Ignored(p->chain, key, Lower(LeafOf(dir)), true);
                d.chain = p->chain;
                if (!d.ignored) AddIgnores(dir, d.chain);
            }
        }
        Dir& slot = dirs_[key] = std::move(d);
        return slot.inRepo ? &slot : nullptr;
    }
};

// ---- reading

const std::unordered_set<std::wstring>& JunkDirs() {
    static const std::unordered_set<std::wstring> s = {L".git", L".hg", L".svn", L"node_modules", L"__pycache__", L".venv", L".tox",
                                                       L".mypy_cache", L".pytest_cache", L".vs", L".gradle", L".next", L".nuxt", L".cache"};
    return s;
}
const std::unordered_set<std::wstring>& BinaryExts() {
    static const std::unordered_set<std::wstring> s = {
        L".exe", L".dll", L".pdb", L".obj", L".lib", L".bin", L".iso", L".zip", L".7z", L".rar", L".gz", L".tar", L".xz", L".zst", L".png",
        L".jpg", L".jpeg", L".gif", L".bmp", L".ico", L".webp", L".tif", L".tiff", L".mp3", L".mp4", L".mkv", L".avi", L".mov", L".wav",
        L".flac", L".ogg", L".ttf", L".otf", L".woff", L".woff2", L".class", L".jar", L".pyc", L".so", L".a", L".o", L".blend", L".blend1",
        L".psd", L".exr", L".hdr", L".dds", L".glb", L".fbx", L".db", L".sqlite", L".pak", L".msi", L".cab", L".pch", L".ipch", L".res",
        L".idx", L".pack", L".3mf", L".stl"};
    return s;
}
const std::unordered_set<std::wstring>& DocExts() {
    static const std::unordered_set<std::wstring> s = {L".pdf", L".doc", L".docx", L".docm", L".xls", L".xlsx", L".xlsm", L".ppt",
                                                       L".pptx", L".rtf", L".odt", L".ods", L".odp", L".msg", L".epub"};
    return s;
}

constexpr uint64_t kTextMax = 16ull << 20, kDocMax = 64ull << 20;

std::wstring LongPath(const std::wstring& p) { return p.size() >= MAX_PATH - 12 ? L"\\\\?\\" + p : p; }

enum class ReadOutcome { Ok, Binary, Big, Fail, NoMatch };

// A first look at a file's raw bytes, for a literal pattern in plain ASCII. Those characters are the same bytes in
// UTF-8 and in the system code page, so a file whose bytes don't hold the pattern can't match, and it is passed over
// without being decoded. That is most files, and decoding was most of the time.
struct BytePre {
    bool on = false, icase = false;
    std::string pat;  // lowercase when icase
    size_t anchor = 0;  // its rarest character: the one looked for first
};

// How common a character is in text and code; the rarest one of a pattern has the fewest places to check around it.
int Commonness(char c, bool icase) {
    static const char order[] = "etaoinsrlcdhupmfgybwvkxjqz";  // letters, the most common first
    if (c == ' ') return 100;
    if (c >= 'a' && c <= 'z') return 60 - static_cast<int>(strchr(order, c) - order);
    if (c >= 'A' && c <= 'Z') return icase ? 60 - static_cast<int>(strchr(order, c + 32) - order) : 22 - static_cast<int>(strchr(order, c + 32) - order) / 2;
    if (c >= '0' && c <= '9') return 30;
    if (strchr("_.,;()=\"'-/:\t", c)) return 40;
    return 15;
}

BytePre MakeBytePre(const std::wstring& pattern, bool regex, bool icase) {
    BytePre p;
    if (regex || pattern.empty()) return p;
    for (const wchar_t c : pattern)
        if (c == 0 || c >= 0x80) return p;
    p.on = true;
    p.icase = icase;
    for (const wchar_t c : pattern) p.pat += static_cast<char>(icase && c >= L'A' && c <= L'Z' ? c + 32 : c);
    for (size_t i = 1; i < p.pat.size(); ++i)
        if (Commonness(p.pat[i], icase) < Commonness(p.pat[p.anchor], icase)) p.anchor = i;
    return p;
}

// Are the pattern's bytes in these? Its rarest character is looked for with memchr, and the rest compared around it.
bool BytesMayMatch(const BytePre& p, const char* h, size_t n) {
    const size_t m = p.pat.size(), a = p.anchor;
    if (n < m) return false;
    const char* first = h + a;            // the first place the anchor can be
    const char* last = h + (n - m) + a;   // and the last
    auto at = [&](const char* f) {
        const char* s = f - a;
        if (!p.icase) return memcmp(s, p.pat.data(), m) == 0;
        for (size_t i = 0; i < m; ++i) {
            char c = s[i];
            if (static_cast<unsigned char>(c - 'A') < 26) c |= 0x20;
            if (c != p.pat[i]) return false;
        }
        return true;
    };
    auto next = [&](const char* from, char c) -> const char* {
        return from > last ? nullptr : static_cast<const char*>(memchr(from, c, static_cast<size_t>(last - from) + 1));
    };
    const char lo = p.pat[a], up = p.icase && lo >= 'a' && lo <= 'z' ? static_cast<char>(lo - 32) : lo;
    if (lo == up) {
        for (const char* f = next(first, lo); f; f = next(f + 1, lo))
            if (at(f)) return true;
        return false;
    }
    const char *fl = next(first, lo), *fu = next(first, up);
    while (fl || fu) {
        const bool low = fl && (!fu || fl < fu);
        const char* f = low ? fl : fu;
        if (at(f)) return true;
        if (low) fl = next(f + 1, lo);
        else fu = next(f + 1, up);
    }
    return false;
}

// A file's bytes: all of them for text, only the start when that shows it is binary.
struct Raw {
    std::unique_ptr<char[]> p;
    size_t n = 0;
};

ReadOutcome ReadRaw(const std::wstring& path, uint64_t size, Raw& raw) {
    if (size > kTextMax) return ReadOutcome::Big;
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ReadOutcome::Fail;
    raw.p.reset(new char[static_cast<size_t>(size) + 1]);
    const auto* u = reinterpret_cast<const unsigned char*>(raw.p.get());
    const DWORD head = static_cast<DWORD>(std::min<uint64_t>(size, 8192));
    DWORD got = 0;
    BOOL ok = ReadFile(h, raw.p.get(), head, &got, nullptr);
    const bool utf16 = got >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF));
    const bool bom8 = got >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF;
    if (ok && !utf16 && !bom8 && memchr(raw.p.get(), 0, std::min<size_t>(got, 8000))) {
        CloseHandle(h);
        raw = {};
        return ReadOutcome::Binary;
    }
    if (ok && got == head && size > head) {
        DWORD more = 0;
        ok = ReadFile(h, raw.p.get() + got, static_cast<DWORD>(size - got), &more, nullptr);
        got += more;
    }
    CloseHandle(h);
    if (!ok) {
        raw = {};
        return ReadOutcome::Fail;
    }
    raw.n = got;
    return ReadOutcome::Ok;
}

// Bytes without a mark: UTF-8, or the system code page when they aren't valid UTF-8.
void DecodeMultiByte(const char* b, size_t n, std::wstring& out) {
    const int len = static_cast<int>(n);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int w = len ? MultiByteToWideChar(cp, flags, b, len, nullptr, 0) : 0;
    if (w == 0 && len) {
        cp = CP_ACP;
        flags = 0;
        w = MultiByteToWideChar(cp, flags, b, len, nullptr, 0);
    }
    out.resize(static_cast<size_t>(w));
    if (w) MultiByteToWideChar(cp, flags, b, len, out.data(), w);
}

// The text in a file's bytes, in whichever encoding they are: UTF-16 with its mark, UTF-8, or the system code page.
// NoMatch: the pattern can't be in it, so it wasn't decoded.
ReadOutcome DecodeBytes(const char* b, size_t got, std::wstring& out, const BytePre* pre) {
    const auto* u = reinterpret_cast<const unsigned char*>(b);
    if (got >= 2 && u[0] == 0xFF && u[1] == 0xFE) {
        out.assign(reinterpret_cast<const wchar_t*>(b + 2), (got - 2) / 2);
        return ReadOutcome::Ok;
    }
    if (got >= 2 && u[0] == 0xFE && u[1] == 0xFF) {
        out.resize((got - 2) / 2);
        for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<wchar_t>((u[2 + 2 * i] << 8) | u[3 + 2 * i]);
        return ReadOutcome::Ok;
    }
    const size_t start = got >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF ? 3 : 0;
    if (pre && pre->on && !BytesMayMatch(*pre, b + start, got - start)) return ReadOutcome::NoMatch;
    DecodeMultiByte(b + start, got - start, out);
    return ReadOutcome::Ok;
}

ReadOutcome ReadText(const std::wstring& path, uint64_t size, std::wstring& out, const BytePre* pre = nullptr) {
    Raw raw;
    const ReadOutcome r = ReadRaw(path, size, raw);
    return r == ReadOutcome::Ok ? DecodeBytes(raw.p.get(), raw.n, out, pre) : r;
}

using LoadIFilterFn = HRESULT(WINAPI*)(PCWSTR, IUnknown*, void**);
LoadIFilterFn LoadIFilterProc() {
    static const LoadIFilterFn fn = [] {
        HMODULE m = LoadLibraryW(L"query.dll");
        return m ? reinterpret_cast<LoadIFilterFn>(reinterpret_cast<void*>(GetProcAddress(m, "LoadIFilter"))) : nullptr;
    }();
    return fn;
}

// The filter registered for an extension: .ext -> PersistentHandler -> the IFilter class.
bool FilterClass(const std::wstring& ext, CLSID& out) {
    wchar_t handler[64], filter[64];
    DWORD n = sizeof(handler);
    if (RegGetValueW(HKEY_CLASSES_ROOT, (ext + L"\\PersistentHandler").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, handler, &n) != ERROR_SUCCESS) return false;
    const std::wstring key = std::wstring(L"CLSID\\") + handler + L"\\PersistentAddinsRegistered\\{89BCB740-6119-101A-BCB7-00DD010655AF}";
    n = sizeof(filter);
    if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, filter, &n) != ERROR_SUCCESS) return false;
    return SUCCEEDED(CLSIDFromString(filter, &out));
}

// A document's filter, ready to read. The newer ones (Windows' own PDF filter, among them) only take the file as a
// stream, which LoadIFilter doesn't do: then the filter is created directly and given the stream.
IFilter* OpenFilter(const std::wstring& path) {
    IFilter* f = nullptr;
    if (LoadIFilterFn load = LoadIFilterProc(); load && SUCCEEDED(load(path.c_str(), nullptr, reinterpret_cast<void**>(&f))) && f) return f;
    CLSID c;
    if (!FilterClass(ExtOf(path), c) || FAILED(CoCreateInstance(c, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFilter), reinterpret_cast<void**>(&f))) || !f)
        return nullptr;
    bool ready = false;
    IInitializeWithStream* iws = nullptr;
    if (SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&iws)))) {
        IStream* s = nullptr;
        if (SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &s))) {
            ready = SUCCEEDED(iws->Initialize(s, STGM_READ));
            s->Release();
        }
        iws->Release();
    }
    IPersistFile* pf = nullptr;
    if (!ready && SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&pf)))) {
        ready = SUCCEEDED(pf->Load(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE));
        pf->Release();
    }
    if (!ready) {
        f->Release();
        return nullptr;
    }
    return f;
}

// The text of a document through its IFilter. false: there is no filter for this kind of file on this PC.
bool DocText(const std::wstring& path, std::wstring& out, uint64_t deadline) {
    IFilter* f = OpenFilter(path);
    if (!f) return false;
    ULONG flags = 0;
    if (FAILED(f->Init(IFILTER_INIT_CANON_PARAGRAPHS | IFILTER_INIT_HARD_LINE_BREAKS | IFILTER_INIT_CANON_HYPHENS | IFILTER_INIT_CANON_SPACES |
                           IFILTER_INIT_INDEXING_ONLY | IFILTER_INIT_APPLY_INDEX_ATTRIBUTES,
                       0, nullptr, &flags))) {
        f->Release();
        return false;
    }
    STAT_CHUNK st;
    std::vector<wchar_t> buf(16384);
    for (;;) {
        HRESULT hr = f->GetChunk(&st);
        if (hr == FILTER_E_END_OF_CHUNKS) break;
        if (FAILED(hr)) {
            if (hr == FILTER_E_EMBEDDING_UNAVAILABLE || hr == FILTER_E_LINK_UNAVAILABLE) continue;
            break;
        }
        if (st.breakType != CHUNK_NO_BREAK && !out.empty() && out.back() != L'\n') out += L'\n';
        if (!(st.flags & CHUNK_TEXT)) continue;
        for (;;) {
            ULONG n = static_cast<ULONG>(buf.size());
            hr = f->GetText(&n, buf.data());
            if (n) out.append(buf.data(), n);
            if (FAILED(hr) || hr == FILTER_E_NO_MORE_TEXT || hr == FILTER_S_LAST_TEXT || out.size() > (8u << 20)) break;
        }
        if (out.size() > (8u << 20) || GetTickCount64() > deadline) break;
    }
    f->Release();
    for (wchar_t& c : out)
        if (c == L'\r' || c == 0x2029 || c == 0x0B) c = L'\n';  // paragraph marks: lines
    return true;
}

// ---- matching

struct LineHit {
    int line = 0;
    std::wstring text;  // the line; a long one around its first match
    std::vector<std::pair<size_t, std::wstring>> more;  // a long line: around the matches past that, by column
    int beyond = 0;  // and the matches past those
    std::vector<std::pair<int, std::wstring>> before, after;
};
struct FileHits {
    std::wstring path;
    int matches = 0;  // lines that match
    bool doc = false;
    std::vector<LineHit> lines;
};

struct GrepQuery {
    std::wstring pattern, root;
    bool regex = false, ignoreCase = true, documents = true, hidden = false;
    bool ignored = false;  // also what .gitignore leaves out, and dependency and cache folders
    std::vector<std::wstring> include, exclude;  // lowercase globs: on the name, or on the path below root with a '\'
    int context = 1, perFile = 5;
    uint64_t budgetMs = 6000;
};

constexpr size_t kNamedSkips = 5;  // how many of the files passed over are named in the answer

struct GrepResult {
    std::vector<FileHits> files;  // in the order they were walked
    uint64_t searched = 0, docs = 0, big = 0, binaries = 0, unreadable = 0;
    std::vector<std::pair<std::wstring, uint64_t>> bigFiles;  // the first few over the size limit, with their size
    std::vector<std::wstring> unreadableFiles;                // the first few that couldn't be read
    std::unordered_map<std::wstring, int> noFilter;  // documents without a text filter, by extension
    bool timedOut = false, fromMemory = false;
    std::wstring error;
};

// A line is shown whole up to kWide characters; a longer one only around a match, kLead of them before it.
constexpr size_t kWide = 240, kLead = 80;
constexpr size_t kWindows = 4;  // windows of one long line, the first match's included

void TrimLine(const std::wstring& text, size_t& s, size_t& e) {
    while (s < e && (text[s] == L' ' || text[s] == L'\t')) ++s;
    while (e > s && (text[e - 1] == L' ' || text[e - 1] == L'\t' || text[e - 1] == L'\r')) --e;
}
std::wstring Window(const std::wstring& text, size_t s, size_t e, size_t from, size_t to) {
    return (from > s ? L"…" : L"") + text.substr(from, to - from) + (to < e ? L"…" : L"");
}
std::wstring LineAround(const std::wstring& text, size_t s, size_t e, size_t col) {
    TrimLine(text, s, e);
    if (e - s <= kWide) return text.substr(s, e - s);
    const size_t at = col > s ? col : s, from = at > s + kLead ? at - kLead : s;
    return Window(text, s, e, from, std::min(e, from + kWide));
}

// A matching line, with its context. at: where its matches start, and how long they are (the first one at least).
void AddHit(FileHits& fh, const std::wstring& text, const std::vector<std::pair<size_t, size_t>>& at, int line, int context) {
    LineHit h;
    h.line = line;
    const size_t pos = at[0].first;
    size_t s = text.rfind(L'\n', pos == 0 ? 0 : pos - 1);
    s = (s == std::wstring::npos || pos == 0) ? 0 : s + 1;
    if (pos > 0 && text[pos - 1] == L'\n') s = pos;
    size_t e = text.find(L'\n', pos);
    if (e == std::wstring::npos) e = text.size();
    h.text = LineAround(text, s, e, pos);
    size_t ts = s, te = e;
    TrimLine(text, ts, te);
    if (te - ts > kWide) {  // a long line: a window for each match the ones before don't show
        size_t shown = std::min(te, (pos > ts + kLead ? pos - kLead : ts) + kWide);
        for (size_t i = 1; i < at.size(); ++i) {
            const auto [p, n] = at[i];
            if (p + n <= shown) continue;
            if (h.more.size() + 1 >= kWindows) {
                ++h.beyond;
                continue;
            }
            const size_t from = std::max(shown, p > ts + kLead ? p - kLead : ts), to = std::min(te, from + kWide);
            h.more.push_back({p - s + 1, Window(text, ts, te, from, to)});
            shown = to;
        }
    }
    size_t bs = s;
    for (int k = 1; k <= context && bs > 0; ++k) {
        const size_t pe = bs - 1;
        size_t ps = text.rfind(L'\n', pe == 0 ? 0 : pe - 1);
        ps = (ps == std::wstring::npos || pe == 0) ? 0 : ps + 1;
        h.before.insert(h.before.begin(), {line - k, LineAround(text, ps, pe, ps)});
        bs = ps;
    }
    size_t ae = e;
    for (int k = 1; k <= context && ae < text.size(); ++k) {
        const size_t ns = ae + 1;
        size_t ne = text.find(L'\n', ns);
        if (ne == std::wstring::npos) ne = text.size();
        h.after.push_back({line + k, LineAround(text, ns, ne, ns)});
        ae = ne;
    }
    fh.lines.push_back(std::move(h));
}

struct Matcher {
    std::wstring pat;  // lowercase when ignoring case
    bool icase = true, regex = false;
    std::wregex re;
};

// The matching lines of text[from, to), the first line of text being firstLine; the context around them may come from
// outside that range (a file read in parts keeps the lines around each part at hand).
void FindInText(const Matcher& m, const std::wstring& text, FileHits& fh, int perFile, int context, size_t from = 0, size_t to = std::wstring::npos,
                int firstLine = 1) {
    if (!m.regex) {
        std::wstring lowered;
        if (m.icase) {
            lowered = text;
            if (!lowered.empty()) CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
        }
        const std::wstring& hay = m.icase ? lowered : text;
        size_t pos = from, counted = 0;
        int line = firstLine;
        std::vector<std::pair<size_t, size_t>> at;
        while ((pos = hay.find(m.pat, pos)) != std::wstring::npos && pos < to) {
            line += static_cast<int>(std::count(text.begin() + counted, text.begin() + pos, L'\n'));
            counted = pos;
            ++fh.matches;
            size_t eol = text.find(L'\n', pos);  // one per line
            if (eol == std::wstring::npos) eol = text.size();
            if (static_cast<int>(fh.lines.size()) < perFile) {  // shown: where else in the line it is
                at.clear();
                for (size_t p = pos; p != std::wstring::npos && p < eol && at.size() < 1000; p = hay.find(m.pat, p + m.pat.size()))
                    at.push_back({p, m.pat.size()});
                AddHit(fh, text, at, line, context);
            }
            if (eol >= text.size()) break;
            pos = eol;
        }
        return;
    }
    size_t s = 0;
    int line = firstLine;
    while (s <= text.size() && s < to) {
        size_t e = text.find(L'\n', s);
        if (e == std::wstring::npos) e = text.size();
        const wchar_t* b = text.data() + s;
        const wchar_t* end = text.data() + std::min(e, s + 4096);  // very long lines: their start
        std::wcmatch mm;
        if (e > from && std::regex_search(b, end, mm, m.re)) {  // a line before `from` is context, searched already
            ++fh.matches;
            if (static_cast<int>(fh.lines.size()) < perFile) {  // shown: where else in the line it matches
                std::vector<std::pair<size_t, size_t>> at;
                for (std::wcregex_iterator it(b, end, m.re), stop; it != stop && at.size() < 1000; ++it)
                    at.push_back({s + static_cast<size_t>(it->position(0)), static_cast<size_t>(it->length(0))});
                AddHit(fh, text, at, line, context);
            }
        }
        if (e >= text.size()) break;
        s = e + 1;
        ++line;
    }
}

// ---- a file too big to read whole, when it is the path given

// Where the n-th line before `end` begins (end being a line's start, or the text's end).
size_t LinesBack(const std::wstring& text, size_t end, int n) {
    size_t p = end;
    for (int k = 0; k < n && p > 0; ++k) {
        const size_t nl = p >= 2 ? text.rfind(L'\n', p - 2) : std::wstring::npos;
        p = nl == std::wstring::npos ? 0 : nl + 1;
    }
    return p;
}

// Just past the n-th line break from `start`, or the text's end.
size_t LinesOn(const std::wstring& text, size_t start, int n) {
    size_t p = start;
    for (int k = 0; k < n && p < text.size(); ++k) {
        const size_t nl = text.find(L'\n', p);
        p = nl == std::wstring::npos ? text.size() : nl + 1;
    }
    return p;
}

enum : int { kEncUnknown, kEncUtf16Le, kEncUtf16Be, kEncBytes };

// Just past the last line break in b[0, n), or 0 without one.
size_t LastBreak(const std::string& b, size_t n, int encoding) {
    if (encoding == kEncBytes) {
        const size_t at = std::string_view(b.data(), n).rfind('\n');
        return at == std::string::npos ? 0 : at + 1;
    }
    for (size_t i = n & ~size_t{1}; i >= 2;) {
        i -= 2;
        if (encoding == kEncUtf16Le ? (b[i] == '\n' && b[i + 1] == 0) : (b[i] == 0 && b[i + 1] == '\n')) return i + 2;
    }
    return 0;
}

void DecodePart(const std::string& b, size_t n, int encoding, std::wstring& out) {
    if (encoding == kEncBytes) return DecodeMultiByte(b.data(), n, out);
    const auto* u = reinterpret_cast<const unsigned char*>(b.data());
    out.resize(n / 2);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = encoding == kEncUtf16Le ? static_cast<wchar_t>(u[2 * i] | (u[2 * i + 1] << 8)) : static_cast<wchar_t>((u[2 * i] << 8) | u[2 * i + 1]);
}

// The file is read in parts of 8 MB, each cut at a line break, and each part is searched with the lines around it at
// hand: the last lines of a part wait for the next one, so that their context after is there, and the lines before
// them come along as context. The line numbers and the context are the same as if the file were read whole. A
// literal pattern isn't found across a line break. cut: the time budget ended it.
ReadOutcome StreamFile(const Matcher& m, const BytePre& pre, const std::wstring& path, FileHits& fh, int perFile, int context, uint64_t deadline,
                       bool& cut) {
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ReadOutcome::Fail;
    constexpr size_t kPart = 8u << 20, kLineMax = 64u << 20;
    std::string bytes;    // read and not yet taken: the start of the line the last part ended in, then the next part
    std::wstring carry;   // kept from the last part: lines of context, then the lines still to search
    size_t searchFrom = 0;  // where those lines begin in carry
    int lineBase = 1;       // the line number of carry's first line
    int encoding = kEncUnknown;
    bool eof = false;
    ReadOutcome outcome = ReadOutcome::Ok;
    while (!eof) {
        if (GetTickCount64() > deadline) {
            cut = true;
            break;
        }
        const size_t have = bytes.size();
        bytes.resize(have + kPart);
        DWORD got = 0;
        if (!ReadFile(h, bytes.data() + have, static_cast<DWORD>(kPart), &got, nullptr)) {
            outcome = ReadOutcome::Fail;
            break;
        }
        bytes.resize(have + got);
        eof = got < kPart;
        if (encoding == kEncUnknown) {
            const auto* u = reinterpret_cast<const unsigned char*>(bytes.data());
            if (bytes.size() >= 2 && u[0] == 0xFF && u[1] == 0xFE) encoding = kEncUtf16Le;
            else if (bytes.size() >= 2 && u[0] == 0xFE && u[1] == 0xFF) encoding = kEncUtf16Be;
            else encoding = kEncBytes;
            if (encoding != kEncBytes) bytes.erase(0, 2);
            else if (bytes.size() >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) bytes.erase(0, 3);
            if (encoding == kEncBytes && memchr(bytes.data(), 0, std::min<size_t>(bytes.size(), 8000))) {
                outcome = ReadOutcome::Binary;
                break;
            }
        }
        const size_t complete = eof || bytes.size() > kLineMax ? bytes.size() : LastBreak(bytes, bytes.size(), encoding);
        if (complete == 0) continue;  // the line goes on: read more
        std::wstring part;
        DecodePart(bytes, complete, encoding, part);
        if (encoding != kEncBytes || !pre.on || BytesMayMatch(pre, bytes.data(), complete)) {
            std::wstring text = carry + part;
            const size_t hold = eof ? text.size() : LinesBack(text, text.size(), context);  // wait for their context after
            FindInText(m, text, fh, perFile, context, searchFrom, hold, lineBase);
            const size_t keepFrom = LinesBack(text, hold, context);
            lineBase += static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(keepFrom), L'\n'));
            carry = text.substr(keepFrom);
            searchFrom = hold - keepFrom;
        } else {  // the pattern isn't in this part: only the lines waiting from the last one can match
            const std::wstring text = carry + part.substr(0, LinesOn(part, 0, context));
            FindInText(m, text, fh, perFile, context, searchFrom, carry.size(), lineBase);
            const size_t tail = LinesBack(part, part.size(), context);
            lineBase += static_cast<int>(std::count(carry.begin(), carry.end(), L'\n') + std::count(part.begin(), part.begin() + static_cast<std::ptrdiff_t>(tail), L'\n'));
            carry = part.substr(tail);
            searchFrom = carry.size();
        }
        bytes.erase(0, complete);
    }
    CloseHandle(h);
    return outcome;
}

bool GlobsPass(const GrepQuery& q, const std::wstring& nameKey, const std::wstring& relKey) {
    auto hits = [&](const std::wstring& g) { return GlobMatch(g.c_str(), g.find(L'\\') != std::wstring::npos ? relKey.c_str() : nameKey.c_str()); };
    for (const std::wstring& g : q.exclude)
        if (hits(g)) return false;
    if (q.include.empty()) return true;
    for (const std::wstring& g : q.include)
        if (hits(g)) return true;
    return false;
}

// Does path a come before b in a walk that lists a folder's files before going into its subfolders, by name the way
// NTFS lists them?
bool WalkBefore(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    for (;;) {
        size_t ea = a.find(L'\\', i), eb = b.find(L'\\', j);
        const bool lastA = ea == std::wstring::npos, lastB = eb == std::wstring::npos;
        if (lastA) ea = a.size();
        if (lastB) eb = b.size();
        const int c = CompareStringOrdinal(a.data() + i, static_cast<int>(ea - i), b.data() + j, static_cast<int>(eb - j), TRUE);
        if (c != CSTR_EQUAL) return lastA != lastB ? lastA : c == CSTR_LESS_THAN;
        if (lastA || lastB) return lastA && !lastB;
        i = ea + 1;
        j = eb + 1;
    }
}

// One file met on a walk.
struct WalkFile {
    std::wstring path, key, nameKey;
    uint64_t size = 0;
    bool doc = false, binaryExt = false;
};

// Walks a folder the way grep searches it, on a pool of threads that both list folders and take files: hidden entries
// only with q.hidden; ignore files, dependency and cache folders followed unless q.ignored; private places never; q's
// globs pick the files. onDir(key, chain) gets each folder entered, with the rules for its entries; onFile(file) gets
// each file, on the pool's threads, which have COM ready for document filters. false: stop or the deadline cut it short.
template <class OnDir, class OnFile>
bool WalkFolder(const std::wstring& root, IgnChain chain, const GrepQuery& q, bool inJunk, uint64_t deadline, const std::atomic<bool>& stop,
                OnDir&& onDir, OnFile&& onFile) {
    struct DirJob {
        std::wstring dir;
        IgnChain chain;
    };
    std::mutex qm;
    std::condition_variable cv;
    std::deque<WalkFile> files;  // taken first, so the queue stays short
    std::vector<DirJob> dirs;
    dirs.push_back({root, std::move(chain)});
    unsigned busy = 0;  // threads holding a job; none, with nothing queued, is the end
    std::atomic<bool> late{false};
    const std::wstring rootKey = Key(root);
    const size_t relAt = rootKey.size() + (rootKey.back() == L'\\' ? 0 : 1);
    const bool globs = !q.include.empty() || !q.exclude.empty();

    auto listDir = [&](DirJob& d, std::vector<WalkFile>& outFiles, std::vector<DirJob>& outDirs) {
        if (!q.ignored) AddIgnores(d.dir, d.chain);
        onDir(Key(d.dir), d.chain);
        const std::wstring pattern = LongPath(d.dir) + (d.dir.back() == L'\\' ? L"*" : L"\\*");
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
            std::wstring path = Join(d.dir, fd.cFileName), key = Key(path), nameKey = Lower(fd.cFileName);
            if (PrivateLeaf(key, nameKey)) continue;  // the folders above were checked on the way down
            if (!q.hidden && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (!inJunk && JunkDirs().count(nameKey)) continue;
                if (Ignored(d.chain, key, nameKey, true)) continue;
                outDirs.push_back({std::move(path), d.chain});
                continue;
            }
            if (Ignored(d.chain, key, nameKey, false)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) continue;  // only in the cloud: reading it downloads it
            if (globs && !GlobsPass(q, nameKey, key.substr(std::min(key.size(), relAt)))) continue;
            const std::wstring ext = ExtOf(nameKey);
            const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            const bool doc = DocExts().count(ext) != 0;
            if (doc && size > kDocMax) continue;
            outFiles.push_back({std::move(path), std::move(key), std::move(nameKey), size, doc, BinaryExts().count(ext) != 0});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    };

    auto work = [&] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<WalkFile> newFiles;
        std::vector<DirJob> newDirs;
        for (;;) {
            WalkFile fj;
            DirJob dj;
            bool isFile = false;
            {
                std::unique_lock lk(qm);
                cv.wait(lk, [&] { return !files.empty() || !dirs.empty() || busy == 0; });
                if (files.empty() && dirs.empty()) break;  // and no thread busy: nothing more will come
                if (!files.empty()) {
                    fj = std::move(files.front());
                    files.pop_front();
                    isFile = true;
                } else {
                    dj = std::move(dirs.back());
                    dirs.pop_back();
                }
                ++busy;
            }
            if (stop || GetTickCount64() > deadline) late = true;
            else if (isFile) onFile(fj);
            else listDir(dj, newFiles, newDirs);
            bool wake;
            {
                std::lock_guard lk(qm);
                for (WalkFile& x : newFiles) files.push_back(std::move(x));
                for (size_t i = newDirs.size(); i-- > 0;) dirs.push_back(std::move(newDirs[i]));  // the first subfolder on top
                --busy;
                wake = !newFiles.empty() || !newDirs.empty() || (busy == 0 && files.empty() && dirs.empty());
            }
            newFiles.clear();
            newDirs.clear();
            if (wake) cv.notify_all();
        }
        if (SUCCEEDED(co)) CoUninitialize();
    };
    const unsigned nth = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < nth; ++i) workers.emplace_back(work);
    for (auto& t : workers) t.join();
    return !late;
}

// Folders kept in memory (cache.inl): an answer from there, or nothing; and a folder to keep after a walk.
std::optional<GrepResult> CachedGrep(const GrepQuery& q, const Matcher& m, const BytePre& pre);
void CacheAfterWalk(const GrepQuery& q, uint64_t textBytes);

GrepResult RunGrep(const GrepQuery& q) {
    GrepResult res;
    Matcher m;
    m.regex = q.regex;
    m.icase = q.ignoreCase;
    m.pat = q.ignoreCase && !q.regex ? Lower(q.pattern) : q.pattern;
    if (q.regex) {
        try {
            m.re = std::wregex(q.pattern, q.ignoreCase ? std::regex::ECMAScript | std::regex::icase | std::regex::optimize
                                                       : std::regex::ECMAScript | std::regex::optimize);
        } catch (const std::regex_error& e) {
            res.error = L"The regex doesn't compile: " + Wide(e.what());
            return res;
        }
    }
    const BytePre pre = MakeBytePre(q.pattern, q.regex, q.ignoreCase);
    if (auto kept = CachedGrep(q, m, pre)) return std::move(*kept);
    const uint64_t deadline = GetTickCount64() + q.budgetMs;
    std::mutex fm;
    std::atomic<uint64_t> searched{0}, docs{0}, big{0}, binaries{0}, unreadable{0}, textBytes{0};

    auto searchFile = [&](const WalkFile& j) {
        std::wstring text;
        FileHits fh;
        fh.path = j.path;
        fh.doc = j.doc;
        if (j.doc) {
            if (!DocText(j.path, text, std::min(deadline, GetTickCount64() + 4000))) {
                std::lock_guard lk(fm);
                ++res.noFilter[ExtOf(j.nameKey)];
                return;
            }
            ++docs;
        } else {
            const ReadOutcome r = ReadText(j.path, j.size, text, &pre);
            if (r == ReadOutcome::Ok || r == ReadOutcome::NoMatch) textBytes += j.size;
            if (r == ReadOutcome::NoMatch) {
                ++searched;
                return;
            }
            if (r == ReadOutcome::Binary) ++binaries;
            if (r == ReadOutcome::Big || r == ReadOutcome::Fail) {  // passed over: the answer says so
                if (r == ReadOutcome::Big) ++big;
                else ++unreadable;
                std::lock_guard lk(fm);
                if (r == ReadOutcome::Big && res.bigFiles.size() < kNamedSkips) res.bigFiles.push_back({j.path, j.size});
                if (r == ReadOutcome::Fail && res.unreadableFiles.size() < kNamedSkips) res.unreadableFiles.push_back(j.path);
            }
            if (r != ReadOutcome::Ok) return;
        }
        ++searched;
        FindInText(m, text, fh, q.perFile, q.context);
        if (fh.matches) {
            std::lock_guard lk(fm);
            res.files.push_back(std::move(fh));
        }
    };

    const std::wstring rootKey = Key(q.root);
    const DWORD ra = GetFileAttributesW(LongPath(q.root).c_str());
    bool done = true;
    if (Private(rootKey)) {
        // nothing to search: the tools refuse private places before this
    } else if (ra != INVALID_FILE_ATTRIBUTES && !(ra & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        FileInfo(q.root, d);
        const std::wstring nameKey = Lower(LeafOf(q.root));
        const WalkFile one{q.root, rootKey, nameKey, (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow,
                           DocExts().count(ExtOf(nameKey)) != 0, false};
        std::thread([&] {  // with COM ready, for a document's filter
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (!one.doc && one.size > kTextMax) {  // the file asked for, over what is read whole: in parts
                FileHits fh;
                fh.path = one.path;
                bool cut = false;
                const ReadOutcome r = StreamFile(m, pre, one.path, fh, q.perFile, q.context, deadline, cut);
                if (r == ReadOutcome::Binary) ++binaries;
                if (r == ReadOutcome::Fail) {
                    ++unreadable;
                    res.unreadableFiles.push_back(one.path);
                }
                if (r == ReadOutcome::Ok) {
                    ++searched;
                    if (fh.matches) res.files.push_back(std::move(fh));
                    if (cut) done = false;
                }
            } else {
                searchFile(one);
            }
            if (SUCCEEDED(co)) CoUninitialize();
        }).join();
    } else {
        const bool inJunk = q.ignored || AnySegment(rootKey, std::vector<std::wstring>(JunkDirs().begin(), JunkDirs().end()));
        const std::atomic<bool> never{false};
        done = WalkFolder(q.root, q.ignored ? IgnChain{} : IgnoresAbove(q.root), q, inJunk, deadline, never,
                          [](const std::wstring&, const IgnChain&) {},
                          [&](const WalkFile& f) {
                              if (f.binaryExt) ++binaries;
                              else if (!f.doc || q.documents) searchFile(f);
                          });
    }
    // the order a single walk would give: a folder's files, then its subfolders, by name
    std::sort(res.files.begin(), res.files.end(), [](const FileHits& a, const FileHits& b) { return WalkBefore(a.path, b.path); });
    res.searched = searched;
    res.docs = docs;
    res.big = big;
    res.binaries = binaries;
    res.unreadable = unreadable;
    res.timedOut = !done;
    if (done && ra != INVALID_FILE_ATTRIBUTES && (ra & FILE_ATTRIBUTE_DIRECTORY)) CacheAfterWalk(q, textBytes);
    return res;
}
