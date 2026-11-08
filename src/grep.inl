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
};
using IgnChain = std::vector<std::shared_ptr<const IgnSet>>;

std::shared_ptr<const IgnSet> LoadIgnore(const std::wstring& dir) {
    std::ifstream in(Join(dir, L".gitignore"), std::ios::binary);
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
    return set;
}

bool Ignored(const IgnChain& chain, const std::wstring& key, const std::wstring& nameKey, bool dir) {
    bool ign = false;
    for (const auto& s : chain) {
        if (key.size() <= s->baseKey.size() + 1) continue;
        const wchar_t* rel = key.c_str() + s->baseKey.size() + (s->baseKey.back() == L'\\' ? 0 : 1);
        for (const IgnRule& r : s->rules) {
            if ((r.dirOnly && !dir) || ign == !r.negate) continue;  // can't change the answer
            if (GlobMatch(r.pat.c_str(), r.anchored ? rel : nameKey.c_str())) ign = !r.negate;
        }
    }
    return ign;
}

// The .gitignore files that apply to root from above it: from the repository's root down.
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
    for (size_t i = dirs.size(); i-- > 0;)
        if (auto s = LoadIgnore(dirs[i])) chain.push_back(s);
    return chain;
}

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

enum class ReadOutcome { Ok, Binary, Big, Fail };

ReadOutcome ReadText(const std::wstring& path, uint64_t size, std::wstring& out) {
    if (size > kTextMax) return ReadOutcome::Big;
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ReadOutcome::Fail;
    std::string b(static_cast<size_t>(size), '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(h, b.data(), static_cast<DWORD>(size), &got, nullptr);
    CloseHandle(h);
    if (!ok) return ReadOutcome::Fail;
    b.resize(got);
    const auto* u = reinterpret_cast<const unsigned char*>(b.data());
    if (got >= 2 && u[0] == 0xFF && u[1] == 0xFE) {
        out.assign(reinterpret_cast<const wchar_t*>(b.data() + 2), (got - 2) / 2);
        return ReadOutcome::Ok;
    }
    if (got >= 2 && u[0] == 0xFE && u[1] == 0xFF) {
        out.resize((got - 2) / 2);
        for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<wchar_t>((u[2 + 2 * i] << 8) | u[3 + 2 * i]);
        return ReadOutcome::Ok;
    }
    size_t start = 0;
    if (got >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) start = 3;
    else if (memchr(b.data(), 0, std::min<size_t>(got, 8000))) return ReadOutcome::Binary;
    const int len = static_cast<int>(got - start);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = len ? MultiByteToWideChar(cp, flags, b.data() + start, len, nullptr, 0) : 0;
    if (n == 0 && len) {
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, b.data() + start, len, nullptr, 0);
    }
    out.resize(n);
    if (n) MultiByteToWideChar(cp, flags, b.data() + start, len, out.data(), n);
    return ReadOutcome::Ok;
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
    std::wstring text;
    std::vector<std::pair<int, std::wstring>> before, after;
};
struct FileHits {
    size_t seq = 0;
    std::wstring path;
    int matches = 0;  // lines that match
    bool doc = false;
    std::vector<LineHit> lines;
};

struct GrepQuery {
    std::wstring pattern, root;
    bool regex = false, ignoreCase = true, documents = true, hidden = false;
    std::vector<std::wstring> include, exclude;  // lowercase globs: on the name, or on the path below root with a '\'
    int context = 1, perFile = 5;
    uint64_t budgetMs = 6000;
};

struct GrepResult {
    std::vector<FileHits> files;  // in the order they were walked
    uint64_t searched = 0, docs = 0, big = 0, binaries = 0, unreadable = 0;
    std::unordered_map<std::wstring, int> noFilter;  // documents without a text filter, by extension
    bool timedOut = false;
    std::wstring error;
};

std::wstring LineAround(const std::wstring& text, size_t s, size_t e, size_t col) {
    while (s < e && (text[s] == L' ' || text[s] == L'\t')) ++s;
    while (e > s && (text[e - 1] == L' ' || text[e - 1] == L'\t' || text[e - 1] == L'\r')) --e;
    if (e - s <= 240) return text.substr(s, e - s);
    const size_t at = col > s ? col : s, from = at > s + 80 ? at - 80 : s, to = std::min(e, from + 240);
    return (from > s ? L"…" : L"") + text.substr(from, to - from) + (to < e ? L"…" : L"");
}

void AddHit(FileHits& fh, const std::wstring& text, size_t pos, int line, int context) {
    LineHit h;
    h.line = line;
    size_t s = text.rfind(L'\n', pos == 0 ? 0 : pos - 1);
    s = (s == std::wstring::npos || pos == 0) ? 0 : s + 1;
    if (pos > 0 && text[pos - 1] == L'\n') s = pos;
    size_t e = text.find(L'\n', pos);
    if (e == std::wstring::npos) e = text.size();
    h.text = LineAround(text, s, e, pos);
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

void FindInText(const Matcher& m, const std::wstring& text, FileHits& fh, int perFile, int context) {
    if (!m.regex) {
        std::wstring lowered;
        if (m.icase) {
            lowered = text;
            if (!lowered.empty()) CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
        }
        const std::wstring& hay = m.icase ? lowered : text;
        size_t pos = 0, counted = 0;
        int line = 1;
        while ((pos = hay.find(m.pat, pos)) != std::wstring::npos) {
            line += static_cast<int>(std::count(text.begin() + counted, text.begin() + pos, L'\n'));
            counted = pos;
            ++fh.matches;
            if (static_cast<int>(fh.lines.size()) < perFile) AddHit(fh, text, pos, line, context);
            const size_t eol = text.find(L'\n', pos);  // one per line
            if (eol == std::wstring::npos) break;
            pos = eol;
        }
        return;
    }
    size_t s = 0;
    int line = 1;
    while (s <= text.size()) {
        size_t e = text.find(L'\n', s);
        if (e == std::wstring::npos) e = text.size();
        const wchar_t* b = text.data() + s;
        const wchar_t* end = text.data() + std::min(e, s + 4096);  // very long lines: their start
        std::wcmatch mm;
        if (std::regex_search(b, end, mm, m.re)) {
            ++fh.matches;
            if (static_cast<int>(fh.lines.size()) < perFile) AddHit(fh, text, s + static_cast<size_t>(mm.position(0)), line, context);
        }
        if (e >= text.size()) break;
        s = e + 1;
        ++line;
    }
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
    const uint64_t deadline = GetTickCount64() + q.budgetMs;
    struct Job {
        size_t seq;
        std::wstring path;
        uint64_t size;
        bool doc;
    };
    std::mutex qm, fm;
    std::condition_variable cv, room;
    std::deque<Job> queue;
    bool walked = false;
    std::atomic<bool> late{false};
    std::atomic<uint64_t> searched{0}, docs{0}, big{0}, binaries{0}, unreadable{0};
    auto work = [&] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            Job j;
            {
                std::unique_lock lk(qm);
                cv.wait(lk, [&] { return !queue.empty() || walked; });
                if (queue.empty()) break;
                j = std::move(queue.front());
                queue.pop_front();
            }
            room.notify_one();
            if (GetTickCount64() > deadline) {
                late = true;
                continue;
            }
            std::wstring text;
            FileHits fh;
            fh.seq = j.seq;
            fh.path = j.path;
            fh.doc = j.doc;
            if (j.doc) {
                if (!DocText(j.path, text, std::min(deadline, GetTickCount64() + 4000))) {
                    std::lock_guard lk(fm);
                    ++res.noFilter[ExtOf(j.path)];
                    continue;
                }
                ++docs;
            } else {
                const ReadOutcome r = ReadText(j.path, j.size, text);
                if (r == ReadOutcome::Binary) ++binaries;
                if (r == ReadOutcome::Big) ++big;
                if (r == ReadOutcome::Fail) ++unreadable;
                if (r != ReadOutcome::Ok) continue;
            }
            ++searched;
            FindInText(m, text, fh, q.perFile, q.context);
            if (fh.matches) {
                std::lock_guard lk(fm);
                res.files.push_back(std::move(fh));
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
    };
    const unsigned nth = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < nth; ++i) workers.emplace_back(work);

    size_t seq = 0;
    auto push = [&](const std::wstring& path, uint64_t size, bool doc) {
        std::unique_lock lk(qm);
        room.wait(lk, [&] { return queue.size() < 4096; });
        queue.push_back({seq++, path, size, doc});
        lk.unlock();
        cv.notify_one();
    };
    auto consider = [&](const std::wstring& path, const std::wstring& nameKey, const std::wstring& relKey, uint64_t size) {
        if (!GlobsPass(q, nameKey, relKey)) return;
        const std::wstring ext = ExtOf(nameKey);
        const bool doc = DocExts().count(ext) != 0;
        if (doc) {
            if (q.documents && size <= kDocMax) push(path, size, true);
            return;
        }
        if (BinaryExts().count(ext)) {
            ++binaries;
            return;
        }
        push(path, size, false);
    };

    const std::wstring rootKey = Key(q.root);
    const DWORD ra = GetFileAttributesW(LongPath(q.root).c_str());
    if (ra != INVALID_FILE_ATTRIBUTES && !(ra & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        FileInfo(q.root, d);
        const std::wstring nameKey = Lower(LeafOf(q.root));
        const std::wstring ext = ExtOf(nameKey);
        push(q.root, (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow, DocExts().count(ext) != 0);
    } else {
        const bool inJunk = AnySegment(rootKey, std::vector<std::wstring>(JunkDirs().begin(), JunkDirs().end()));
        struct Frame {
            std::wstring dir;
            IgnChain chain;
        };
        std::vector<Frame> stack{{q.root, IgnoresAbove(q.root)}};
        WIN32_FIND_DATAW fd;
        while (!stack.empty() && GetTickCount64() < deadline) {
            Frame fr = std::move(stack.back());
            stack.pop_back();
            if (auto s = LoadIgnore(fr.dir)) fr.chain.push_back(s);
            const std::wstring pattern = LongPath(fr.dir) + (fr.dir.back() == L'\\' ? L"*" : L"\\*");
            HANDLE f = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (f == INVALID_HANDLE_VALUE) continue;
            std::vector<std::wstring> sub;
            do {
                if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
                const std::wstring path = Join(fr.dir, fd.cFileName), key = Key(path), nameKey = Lower(fd.cFileName);
                if (Private(key)) continue;
                if (!q.hidden && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (isDir) {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    if (!inJunk && JunkDirs().count(nameKey)) continue;
                    if (Ignored(fr.chain, key, nameKey, true)) continue;
                    sub.push_back(path);
                    continue;
                }
                if (Ignored(fr.chain, key, nameKey, false)) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) continue;  // only in the cloud: reading it downloads it
                const std::wstring relKey = key.substr(std::min(key.size(), rootKey.size() + (rootKey.back() == L'\\' ? 0 : 1)));
                consider(path, nameKey, relKey, (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
            for (size_t i = sub.size(); i-- > 0;) stack.push_back({sub[i], fr.chain});
        }
        if (!stack.empty()) late = true;
    }
    {
        std::lock_guard lk(qm);
        walked = true;
    }
    cv.notify_all();
    for (auto& t : workers) t.join();
    std::sort(res.files.begin(), res.files.end(), [](const FileHits& a, const FileHits& b) { return a.seq < b.seq; });
    res.searched = searched;
    res.docs = docs;
    res.big = big;
    res.binaries = binaries;
    res.unreadable = unreadable;
    res.timedOut = late;
    return res;
}
