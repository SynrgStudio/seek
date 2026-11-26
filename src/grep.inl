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
        L".idx", L".pack", L".stl",  // .3mf: a package, read by its parts
        // game engines' imported and packed assets (Godot, Unreal)
        L".ctex", L".stex", L".scn", L".mesh", L".sample", L".oggvorbisstr", L".pck", L".uasset", L".umap", L".wasm"};
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
// The same goes for a regex that is only plain texts joined by |: a file with none of them can't match.
struct BytePre {
    bool on = false, icase = false;
    struct Alt {
        std::string pat;  // lowercase when icase
        size_t anchor = 0;  // its rarest character: the one looked for first
    };
    std::vector<Alt> alts;  // the pattern; or each of the texts of such a regex
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

// texts: the literal pattern, or the plain texts of a regex (none for any other regex: no first look, then).
BytePre MakeBytePre(const std::vector<std::wstring>& texts, bool icase) {
    BytePre p;
    if (texts.empty()) return p;
    for (const std::wstring& t : texts) {
        if (t.empty()) return p;
        for (const wchar_t c : t)
            if (c == 0 || c >= 0x80) return p;
    }
    p.on = true;
    p.icase = icase;
    for (const std::wstring& t : texts) {
        BytePre::Alt a;
        for (const wchar_t c : t) a.pat += static_cast<char>(icase && c >= L'A' && c <= L'Z' ? c + 32 : c);
        for (size_t i = 1; i < a.pat.size(); ++i)
            if (Commonness(a.pat[i], icase) < Commonness(a.pat[a.anchor], icase)) a.anchor = i;
        p.alts.push_back(std::move(a));
    }
    return p;
}

// Are a text's bytes in these? Its rarest character is looked for with memchr, and the rest compared around it.
bool AltMayMatch(const BytePre::Alt& alt, bool icase, const char* h, size_t n) {
    const size_t m = alt.pat.size(), a = alt.anchor;
    if (n < m) return false;
    const char* first = h + a;            // the first place the anchor can be
    const char* last = h + (n - m) + a;   // and the last
    auto at = [&](const char* f) {
        const char* s = f - a;
        if (!icase) return memcmp(s, alt.pat.data(), m) == 0;
        for (size_t i = 0; i < m; ++i) {
            char c = s[i];
            if (static_cast<unsigned char>(c - 'A') < 26) c |= 0x20;
            if (c != alt.pat[i]) return false;
        }
        return true;
    };
    auto next = [&](const char* from, char c) -> const char* {
        return from > last ? nullptr : static_cast<const char*>(memchr(from, c, static_cast<size_t>(last - from) + 1));
    };
    const char lo = alt.pat[a], up = icase && lo >= 'a' && lo <= 'z' ? static_cast<char>(lo - 32) : lo;
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

// Can these bytes hold the pattern: are any of its texts in them?
bool BytesMayMatch(const BytePre& p, const char* h, size_t n) {
    for (const BytePre::Alt& alt : p.alts)
        if (AltMayMatch(alt, p.icase, h, n)) return true;
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

// ---- packages: a 3MF (3D printing) is a zip of XML parts, like an Office file, but Windows has no text filter for it,
// and none for deflate either: the parts are inflated here (RFC 1951, after zlib's "puff").

const std::unordered_set<std::wstring>& PackageExts() {
    static const std::unordered_set<std::wstring> s = {L".3mf"};
    return s;
}

constexpr uint64_t kPartMax = 256ull << 20;  // a part larger than this inflated (a huge mesh) is passed over

struct Inflate {
    const uint8_t* in;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    std::string& out;  // sized to the part's size beforehand: written by index, w bytes so far
    size_t cap, w = 0;
    bool bad = false;
    struct Huff {
        uint16_t count[16];
        uint16_t sym[288];
    };
    Inflate(const uint8_t* i, size_t len, std::string& o, size_t c) : in(i), n(len), out(o), cap(c) { out.resize(c); }
    int Bits(int need) {
        while (cnt < need) {
            if (pos >= n) {
                bad = true;
                return 0;
            }
            buf |= static_cast<uint32_t>(in[pos++]) << cnt;
            cnt += 8;
        }
        const int v = static_cast<int>(buf & ((1u << need) - 1));
        buf >>= need;
        cnt -= need;
        return v;
    }
    static bool Build(Huff& h, const uint8_t* len, int n) {
        memset(h.count, 0, sizeof(h.count));
        for (int i = 0; i < n; ++i) ++h.count[len[i]];
        if (h.count[0] == n) return true;  // no codes: only an error if one is used
        int left = 1;
        for (int b = 1; b < 16; ++b) {
            left = (left << 1) - h.count[b];
            if (left < 0) return false;  // more codes than lengths allow
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int b = 1; b < 15; ++b) offs[b + 1] = static_cast<uint16_t>(offs[b] + h.count[b]);
        for (int i = 0; i < n; ++i)
            if (len[i]) h.sym[offs[len[i]]++] = static_cast<uint16_t>(i);
        return true;
    }
    int Decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= Bits(1);
            const int count = h.count[len];
            if (code - count < first) return h.sym[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
            if (bad) return -1;
        }
        return -1;
    }
    bool Codes(const Huff& lens, const Huff& dists) {
        static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                           4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lens);
            if (sym < 0 || bad) return false;
            if (sym < 256) {
                if (w >= cap) return false;
                out[w++] = static_cast<char>(sym);
                continue;
            }
            if (sym == 256) return true;
            sym -= 257;
            if (sym >= 29) return false;
            const size_t len = lbase[sym] + static_cast<size_t>(Bits(lext[sym]));
            const int ds = Decode(dists);
            if (ds < 0 || ds >= 30) return false;
            const size_t dist = dbase[ds] + static_cast<size_t>(Bits(dext[ds]));
            if (bad || dist > w || w + len > cap) return false;
            char* o = out.data();
            for (size_t k = 0; k < len; ++k, ++w) o[w] = o[w - dist];  // byte by byte: a copy may overlap itself
        }
    }
    bool Run() {
        Huff lens, dists;
        for (int last = 0; !last;) {
            last = Bits(1);
            const int type = Bits(2);
            if (bad) return false;
            if (type == 0) {  // stored
                buf = 0;
                cnt = 0;
                if (pos + 4 > n) return false;
                const size_t len = in[pos] | (in[pos + 1] << 8);
                if ((len ^ 0xFFFF) != static_cast<size_t>(in[pos + 2] | (in[pos + 3] << 8))) return false;
                pos += 4;
                if (pos + len > n || w + len > cap) return false;
                memcpy(out.data() + w, in + pos, len);
                w += len;
                pos += len;
            } else if (type == 1) {  // fixed codes
                uint8_t l[288];
                for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                Build(lens, l, 288);
                for (int i = 0; i < 30; ++i) l[i] = 5;
                Build(dists, l, 30);
                if (!Codes(lens, dists)) return false;
            } else if (type == 2) {  // dynamic codes
                const int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
                if (bad || nlen > 286 || ndist > 30) return false;
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t l[320] = {};
                for (int i = 0; i < ncode; ++i) l[order[i]] = static_cast<uint8_t>(Bits(3));
                if (!Build(lens, l, 19)) return false;
                for (int i = 0; i < nlen + ndist;) {
                    const int sym = Decode(lens);
                    if (sym < 0 || bad) return false;
                    if (sym < 16) {
                        l[i++] = static_cast<uint8_t>(sym);
                        continue;
                    }
                    uint8_t v = 0;
                    int rep;
                    if (sym == 16) {
                        if (i == 0) return false;
                        v = l[i - 1];
                        rep = 3 + Bits(2);
                    } else {
                        rep = sym == 17 ? 3 + Bits(3) : 11 + Bits(7);
                    }
                    if (i + rep > nlen + ndist) return false;
                    while (rep--) l[i++] = v;
                }
                if (!Build(lens, l, nlen) || !Build(dists, l + nlen, ndist)) return false;
                if (!Codes(lens, dists)) return false;
            } else {
                return false;
            }
        }
        out.resize(w);
        return true;
    }
};

// The text parts of a package, one at a time: onPart(name, bytes). Only those parts are read from the disk: not the
// images and thumbnails. false: not a zip Seek can read (damaged, or ZIP64, past 4 GB). tooBig counts the parts
// passed over for their size.
template <class OnPart>
bool ForEachPackagePart(const std::wstring& path, uint64_t deadline, int& tooBig, OnPart&& onPart) {
    HANDLE f = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{f};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 22 || size.QuadPart >= (4ll << 30)) return false;
    const uint64_t n = static_cast<uint64_t>(size.QuadPart);
    auto readAt = [&](uint64_t at, size_t len, std::string& into) {  // false: past the end, or a read that failed
        if (at > n || len > n - at) return false;  // checked before the buffer is made: a bad size asked for 4 GB
        into.resize(len);
        OVERLAPPED o{};
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        for (size_t done = 0; done < len;) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(len - done, 64u << 20));
            if (!ReadFile(f, into.data() + done, want, &got, &o) || got == 0) return false;
            done += got;
            const uint64_t next = at + done;
            o.Offset = static_cast<DWORD>(next);
            o.OffsetHigh = static_cast<DWORD>(next >> 32);
        }
        return true;
    };
    auto u16 = [](const std::string& b, size_t at) { return static_cast<uint32_t>(static_cast<uint8_t>(b[at]) | (static_cast<uint8_t>(b[at + 1]) << 8)); };
    auto u32 = [&](const std::string& b, size_t at) { return u16(b, at) | (u16(b, at + 2) << 16); };
    auto u64 = [&](const std::string& b, size_t at) { return u32(b, at) | (static_cast<uint64_t>(u32(b, at + 4)) << 32); };
    // the end of the central directory, in the last 64 KB (its comment is at most that)
    std::string tail;
    const size_t tailLen = static_cast<size_t>(std::min<uint64_t>(n, 22 + 65535));
    if (!readAt(n - tailLen, tailLen, tail)) return false;
    size_t end = tailLen - 22;
    while (end > 0 && u32(tail, end) != 0x06054b50) --end;
    if (u32(tail, end) != 0x06054b50) return false;
    uint64_t count = u16(tail, end + 10), dirLen = u32(tail, end + 12), dirAt = u32(tail, end + 16);
    std::string dir, head, packed, data;
    // ZIP64 (slicers write it): the real counts in a record of its own, found by a locator just before the end record
    if ((count == 0xFFFF || dirLen == 0xFFFFFFFF || dirAt == 0xFFFFFFFF) && end >= 20 && u32(tail, end - 20) == 0x07064b50) {
        if (!readAt(u64(tail, end - 20 + 8), 56, head) || u32(head, 0) != 0x06064b50) return false;
        count = u64(head, 32);
        dirLen = u64(head, 40);
        dirAt = u64(head, 48);
    }
    if (dirLen > (256u << 20) || !readAt(dirAt, static_cast<size_t>(dirLen), dir)) return false;
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        if (GetTickCount64() > deadline) return true;  // the caller's budget is over: what was read stands
        if (at + 46 > dir.size() || u32(dir, at) != 0x02014b50) return false;
        const uint32_t method = u16(dir, at + 10);
        uint64_t packedLen = u32(dir, at + 20), full = u32(dir, at + 24), local = u32(dir, at + 42);
        const size_t nameLen = u16(dir, at + 28), extraLen = u16(dir, at + 30);
        if (at + 46 + nameLen + extraLen > dir.size()) return false;
        const std::string name = dir.substr(at + 46, nameLen);
        // ZIP64: the sizes and the offset that don't fit in 32 bits are in an extra field (0x0001), in that order
        for (size_t x = at + 46 + nameLen, xEnd = x + extraLen; x + 4 <= xEnd;) {
            const uint32_t id = u16(dir, x), len = u16(dir, x + 2);
            if (id == 0x0001) {
                size_t v = x + 4;
                const size_t vEnd = std::min(xEnd, v + len);
                if (full == 0xFFFFFFFF && v + 8 <= vEnd) full = u64(dir, v), v += 8;
                if (packedLen == 0xFFFFFFFF && v + 8 <= vEnd) packedLen = u64(dir, v), v += 8;
                if (local == 0xFFFFFFFF && v + 8 <= vEnd) local = u64(dir, v);
                break;
            }
            x += 4 + len;
        }
        at += 46 + nameLen + extraLen + u16(dir, at + 32);
        const size_t dot = name.find_last_of('.');
        std::string ext = dot == std::string::npos ? "" : name.substr(dot);
        for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (ext != ".model" && ext != ".config" && ext != ".xml" && ext != ".rels" && ext != ".json" && ext != ".txt") continue;
        if (full > kPartMax) {
            ++tooBig;
            continue;
        }
        static std::mutex bigOne;  // a part this big inflated on every thread at once ran out of memory
        std::unique_lock<std::mutex> one(bigOne, std::defer_lock);
        if (full > (32u << 20)) one.lock();
        if (!readAt(local, 30, head) || u32(head, 0) != 0x04034b50) continue;
        if (!readAt(local + 30ull + u16(head, 26) + u16(head, 28), packedLen, packed)) continue;
        if (method == 0) {
            data.swap(packed);
        } else if (method == 8) {
            Inflate inf(reinterpret_cast<const uint8_t*>(packed.data()), packed.size(), data, full);
            if (!inf.Run()) continue;
        } else {
            continue;
        }
        onPart(name, data);
    }
    return true;
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
    int col = 0;  // where its first match starts in the line, from 1
    std::wstring text;  // the line; a long one around its first match
    std::vector<std::pair<size_t, std::wstring>> more;  // a long line: around the matches past that, by column
    int beyond = 0;  // and the matches past those
    std::vector<std::pair<int, std::wstring>> before, after;
    bool blockCut = false;  // the block it opens goes on past what is shown
};

// The lines shown around a match.
struct Around {
    int before = 1, after = 1;
    bool block = false;  // after it: the block the matching line opens, instead of `after` lines
    int Ahead() const;   // how many lines after a match its context can take
};
constexpr int kBlockMax = 300;  // lines of a block, at most
int Around::Ahead() const { return block ? kBlockMax : after; }
struct FileHits {
    std::wstring path;
    int matches = 0;  // lines that match
    bool doc = false;
    std::vector<LineHit> lines;
};

struct GrepQuery {
    std::wstring pattern, root;
    std::wstring without;  // lines with this are left out: literal or a regex like the pattern, the same case rule
    bool regex = false, ignoreCase = true, documents = true, hidden = false;
    bool ignored = false;  // also what .gitignore leaves out, and dependency and cache folders
    std::vector<std::wstring> include, exclude;  // lowercase globs: on the name, or on the path below root with a '\'
    Around context;
    int perFile = 5;
    uint64_t budgetMs = 6000;
    // The files to search, picked from the drive index by name (globs that are all names: *.gradle, Cargo.toml): then
    // the folder isn't walked. A whole hard disk took minutes to walk for a few hundred files the index names at once.
    std::shared_ptr<const std::vector<std::wstring>> picked;
};

constexpr size_t kNamedSkips = 5;  // how many of the files passed over are named in the answer

struct GrepResult {
    std::vector<FileHits> files;  // in the order they were walked
    uint64_t searched = 0, docs = 0, big = 0, binaries = 0, unreadable = 0;
    std::vector<std::pair<std::wstring, uint64_t>> bigFiles;  // the first few over the size limit, with their size
    std::vector<std::wstring> unreadableFiles;                // the first few that couldn't be read
    std::unordered_map<std::wstring, int> noFilter;  // documents without a text filter, by extension
    std::unordered_map<std::wstring, int> kinds;     // the files met that aren't binary, by extension: for a glob to suggest
    bool timedOut = false, fromMemory = false, picked = false;
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

// ---- a block: what a line opens, like a function's body after its signature. Told by indentation and brackets, so
// the same rule serves braces on the line (Rust, Go, JS), on the next one (C#, C++), and none (Python), and "end".

int IndentOf(const std::wstring& t, size_t s, size_t e) {
    int w = 0;
    for (size_t i = s; i < e && (t[i] == L' ' || t[i] == L'\t'); ++i) w += t[i] == L'\t' ? 4 - w % 4 : 1;
    return w;
}
int BracketsOf(const std::wstring& t, size_t s, size_t e) {  // opened less closed; strings and comments aren't told apart
    int n = 0;
    for (size_t i = s; i < e; ++i) {
        const wchar_t c = t[i];
        if (c == L'{' || c == L'(' || c == L'[') ++n;
        else if (c == L'}' || c == L')' || c == L']') --n;
    }
    return n;
}
bool BlankLine(const std::wstring& t, size_t s, size_t e) {
    for (size_t i = s; i < e; ++i)
        if (t[i] != L' ' && t[i] != L'\t' && t[i] != L'\r') return false;
    return true;
}
bool ClosesBlock(const std::wstring& t, size_t s, size_t e) {  // starts with a closing bracket, "end" or "</"
    while (s < e && (t[s] == L' ' || t[s] == L'\t')) ++s;
    if (s >= e) return false;
    if (t[s] == L'}' || t[s] == L')' || t[s] == L']') return true;
    if (e - s >= 2 && t[s] == L'<' && t[s + 1] == L'/') return true;
    return e - s >= 3 && t.compare(s, 3, L"end") == 0 && (e - s == 3 || !iswalnum(t[s + 3]));
}

// The lines after a matching line [s, e) that belong to the block it opens: the ones indented deeper, the ones inside
// a bracket it (or a line after it) opens, and the line that closes them. Blank lines at its end are left out.
void AddBlock(LineHit& h, const std::wstring& text, size_t s, size_t e) {
    const int indent = IndentOf(text, s, e);
    int depth = BracketsOf(text, s, e), k = 0;
    bool opened = depth > 0, ended = false;
    size_t ae = e, kept = 0;
    while (ae < text.size() && k < kBlockMax) {
        const size_t ns = ae + 1;
        size_t ne = text.find(L'\n', ns);
        if (ne == std::wstring::npos) ne = text.size();
        ++k;
        ae = ne;
        if (BlankLine(text, ns, ne)) {
            h.after.push_back({h.line + k, L""});
            continue;
        }
        const int in = IndentOf(text, ns, ne), net = BracketsOf(text, ns, ne);
        if (in <= indent && depth <= 0 && net <= 0 && !ClosesBlock(text, ns, ne)) {  // back where the match is: not in it
            --k;
            ended = true;
            break;
        }
        h.after.push_back({h.line + k, LineAround(text, ns, ne, ns)});
        kept = h.after.size();
        depth += net;
        opened = opened || net > 0;
        if (opened && depth <= 0 && in <= indent) {  // the line that closes it
            ended = true;
            break;
        }
    }
    h.after.resize(kept);
    h.blockCut = !ended && ae < text.size();
}

// A matching line, with its context. at: where its matches start, and how long they are (the first one at least).
void AddHit(FileHits& fh, const std::wstring& text, const std::vector<std::pair<size_t, size_t>>& at, int line, const Around& context) {
    LineHit h;
    h.line = line;
    const size_t pos = at[0].first;
    size_t s = text.rfind(L'\n', pos == 0 ? 0 : pos - 1);
    s = (s == std::wstring::npos || pos == 0) ? 0 : s + 1;
    if (pos > 0 && text[pos - 1] == L'\n') s = pos;
    size_t e = text.find(L'\n', pos);
    if (e == std::wstring::npos) e = text.size();
    h.text = LineAround(text, s, e, pos);
    h.col = static_cast<int>(pos - s) + 1;
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
    for (int k = 1; k <= context.before && bs > 0; ++k) {
        const size_t pe = bs - 1;
        size_t ps = text.rfind(L'\n', pe == 0 ? 0 : pe - 1);
        ps = (ps == std::wstring::npos || pe == 0) ? 0 : ps + 1;
        h.before.insert(h.before.begin(), {line - k, LineAround(text, ps, pe, ps)});
        bs = ps;
    }
    if (context.block) {
        AddBlock(h, text, s, e);
    } else {
        size_t ae = e;
        for (int k = 1; k <= context.after && ae < text.size(); ++k) {
            const size_t ns = ae + 1;
            size_t ne = text.find(L'\n', ns);
            if (ne == std::wstring::npos) ne = text.size();
            h.after.push_back({line + k, LineAround(text, ns, ne, ns)});
            ae = ne;
        }
    }
    fh.lines.push_back(std::move(h));
}

struct Matcher {
    std::wstring pat;  // lowercase when ignoring case
    bool icase = true, regex = false;
    std::wregex re;
    std::vector<std::wstring> alts;  // a regex that is only plain texts joined by |: those, looked for as text (lowercase when ignoring case)
    bool without = false;  // and lines with this are left out
    std::wstring withoutPat;
    std::wregex withoutRe;
};

// Does the line [s, e) have what `without` leaves out? hay: the text, lowered when ignoring case.
bool LeftOut(const Matcher& m, const std::wstring& text, const std::wstring& hay, size_t s, size_t e) {
    if (!m.without) return false;
    if (!m.regex) {
        const size_t at = hay.find(m.withoutPat, s);
        return at != std::wstring::npos && at + m.withoutPat.size() <= e;
    }
    return std::regex_search(text.data() + s, text.data() + std::min(e, s + 4096), m.withoutRe);
}

// The matching lines of text[from, to), the first line of text being firstLine; the context around them may come from
// outside that range (a file read in parts keeps the lines around each part at hand).
void FindInText(const Matcher& m, const std::wstring& text, FileHits& fh, int perFile, const Around& context, size_t from = 0,
                size_t to = std::wstring::npos, int firstLine = 1) {
    if (!m.regex || !m.alts.empty()) {  // text: the pattern, or the plain texts of a regex, without the regex engine
        std::wstring lowered;
        if (m.icase) {
            lowered = text;
            if (!lowered.empty()) CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
        }
        const std::wstring& hay = m.icase ? lowered : text;
        const std::wstring* pats = m.alts.empty() ? &m.pat : m.alts.data();
        const size_t np = m.alts.empty() ? 1 : m.alts.size();
        std::vector<size_t> next(np);  // where each text is next
        for (size_t k = 0; k < np; ++k) next[k] = hay.find(pats[k], from);
        size_t counted = 0;
        int line = firstLine;
        std::vector<std::pair<size_t, size_t>> at;
        for (;;) {
            const size_t pos = *std::min_element(next.begin(), next.end());  // the first of them
            if (pos == std::wstring::npos || pos >= to) break;
            line += static_cast<int>(std::count(text.begin() + counted, text.begin() + pos, L'\n'));
            counted = pos;
            size_t eol = text.find(L'\n', pos);  // one per line
            if (eol == std::wstring::npos) eol = text.size();
            const size_t sol = pos == 0 ? 0 : text.rfind(L'\n', pos - 1) + 1;  // the line's start (npos + 1 is 0)
            if (!LeftOut(m, text, hay, sol, eol)) {
                ++fh.matches;
                if (static_cast<int>(fh.lines.size()) < perFile) {  // shown: where else in the line they are
                    at.clear();
                    for (size_t k = 0; k < np; ++k)
                        for (size_t p = next[k]; p != std::wstring::npos && p < eol && at.size() < 1000; p = hay.find(pats[k], p + pats[k].size()))
                            at.push_back({p, pats[k].size()});
                    std::sort(at.begin(), at.end());
                    AddHit(fh, text, at, line, context);
                }
            }
            if (eol >= text.size()) break;
            for (size_t k = 0; k < np; ++k)
                if (next[k] < eol) next[k] = hay.find(pats[k], eol);
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
        if (e > from && std::regex_search(b, end, mm, m.re) && !LeftOut(m, text, text, s, e)) {  // a line before `from` is context, searched already
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
ReadOutcome StreamFile(const Matcher& m, const BytePre& pre, const std::wstring& path, FileHits& fh, int perFile, const Around& context,
                       uint64_t deadline, bool& cut) {
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
            const size_t hold = eof ? text.size() : LinesBack(text, text.size(), context.Ahead());  // wait for their context after
            FindInText(m, text, fh, perFile, context, searchFrom, hold, lineBase);
            const size_t keepFrom = LinesBack(text, hold, context.before);
            lineBase += static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(keepFrom), L'\n'));
            carry = text.substr(keepFrom);
            searchFrom = hold - keepFrom;
        } else {  // the pattern isn't in this part: only the lines waiting from the last one can match
            const std::wstring text = carry + part.substr(0, LinesOn(part, 0, context.Ahead()));
            FindInText(m, text, fh, perFile, context, searchFrom, carry.size(), lineBase);
            const size_t tail = LinesBack(part, part.size(), context.before);
            lineBase += static_cast<int>(std::count(carry.begin(), carry.end(), L'\n') + std::count(part.begin(), part.begin() + static_cast<std::ptrdiff_t>(tail), L'\n'));
            carry = part.substr(tail);
            searchFrom = carry.size();
        }
        bytes.erase(0, complete);
    }
    CloseHandle(h);
    return outcome;
}

// A package's text parts, searched as files of their own: a match's path is the package's and then the part's
// (model.3mf\3D\Objects\object_1.model), so its line numbers are the part's. false: it couldn't be read.
bool SearchPackage(const std::wstring& path, const Matcher& m, const BytePre& pre, int perFile, const Around& context, uint64_t deadline,
                   std::vector<FileHits>& out, int& tooBig) {
    try {
        std::wstring text;
        return ForEachPackagePart(path, deadline, tooBig, [&](const std::string& name, const std::string& data) {
            FileHits fh;
            std::wstring part = Wide(name);
            std::replace(part.begin(), part.end(), L'/', L'\\');
            fh.path = path + L"\\" + part;
            fh.doc = true;
            // A mesh is hundreds of MB of short lines: searched a few MB at a time, cut at a line's end, each piece
            // first looked at as bytes. Context doesn't cross from one piece to the next.
            int line = 1;
            for (size_t at = 0; at < data.size();) {
                size_t end = std::min(data.size(), at + (4u << 20));
                if (end < data.size()) {
                    const size_t eol = data.rfind('\n', end - 1);
                    end = eol != std::string::npos && eol > at ? eol + 1 : end;
                }
                const char* b = data.data() + at;
                const size_t len = end - at;
                if (!pre.on || BytesMayMatch(pre, b, len)) {
                    DecodeMultiByte(b, len, text);
                    FindInText(m, text, fh, perFile, context, 0, std::wstring::npos, line);
                }
                line += static_cast<int>(std::count(b, b + len, '\n'));
                at = end;
            }
            if (fh.matches) out.push_back(std::move(fh));
        });
    } catch (const std::exception& e) {  // out of memory with a huge mesh, mostly: that package is passed over, not the search
        fwprintf(stderr, L"package %s: %hs\n", path.c_str(), e.what());
        return false;
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
    bool doc = false, binaryExt = false, sidecar = false;
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
    // Big files, after every other file and folder: under a time budget, the many small ones (the code) are read
    // before a few big ones (assets, data, logs) take it all, wherever they are in the tree.
    // Before those, the small files a game engine keeps beside each asset (Godot's .import and .uid, Unity's .meta): a
    // project has several for each script, and they would share the budget with the scripts.
    std::deque<WalkFile> aside, later;
    constexpr uint64_t kLater = 256 << 10;
    dirs.push_back({root, std::move(chain)});
    unsigned busy = 0;     // threads holding a job; none, with nothing queued, is the end
    unsigned listing = 0;  // threads listing a folder
    std::atomic<bool> late{false};
    const std::wstring rootKey = Key(root);
    const size_t relAt = rootKey.size() + (rootKey.back() == L'\\' ? 0 : 1);
    const bool globs = !q.include.empty() || !q.exclude.empty();

    auto listDir = [&](DirJob& d, std::vector<WalkFile>& outFiles, std::vector<DirJob>& outDirs, std::vector<WalkFile>& outLater) {
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
            const bool binaryExt = BinaryExts().count(ext) != 0;
            const bool sidecar = ext == L".import" || ext == L".uid" || ext == L".meta";
            ((size > kLater && !binaryExt) || sidecar ? outLater : outFiles)
                .push_back({std::move(path), std::move(key), std::move(nameKey), size, doc, binaryExt, sidecar});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    };

    auto work = [&] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<WalkFile> newFiles, newLater;
        std::vector<DirJob> newDirs;
        for (;;) {
            WalkFile fj;
            DirJob dj;
            bool isFile = false;
            {
                std::unique_lock lk(qm);
                // a big file waits while a thread still lists a folder: that may bring small ones
                cv.wait(lk, [&] { return !files.empty() || !dirs.empty() || ((!aside.empty() || !later.empty()) && !listing) || busy == 0; });
                if (files.empty() && dirs.empty() && aside.empty() && later.empty()) break;  // and no thread busy: nothing more will come
                if (!files.empty()) {
                    fj = std::move(files.front());
                    files.pop_front();
                    isFile = true;
                } else if (!dirs.empty()) {
                    dj = std::move(dirs.back());
                    dirs.pop_back();
                    ++listing;
                } else {
                    std::deque<WalkFile>& from = aside.empty() ? later : aside;
                    fj = std::move(from.front());
                    from.pop_front();
                    isFile = true;
                }
                ++busy;
            }
            if (stop || GetTickCount64() > deadline) late = true;
            else if (isFile) onFile(fj);
            else listDir(dj, newFiles, newDirs, newLater);
            bool wake;
            {
                std::lock_guard lk(qm);
                for (WalkFile& x : newFiles) files.push_back(std::move(x));
                for (WalkFile& x : newLater) (x.sidecar ? aside : later).push_back(std::move(x));
                for (size_t i = newDirs.size(); i-- > 0;) dirs.push_back(std::move(newDirs[i]));  // the first subfolder on top
                --busy;
                if (!isFile) --listing;
                wake = !newFiles.empty() || !newDirs.empty() || (!isFile && !listing && (!aside.empty() || !later.empty())) || busy == 0;
            }
            newFiles.clear();
            newLater.clear();
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

// Searches the files picked from the index (q.picked) as a walk would have met them: hidden files and files in hidden
// folders only with q.hidden, nothing only in the cloud, and the big ones last. false: the deadline cut it short.
template <class OnFile>
bool SearchPicked(const GrepQuery& q, uint64_t deadline, OnFile&& onFile) {
    std::vector<WalkFile> soon, big;  // the big ones last, as a walk takes them
    std::unordered_map<std::wstring, bool> hiddenDir;  // folders below the root, by key: hidden or system
    const std::wstring rootKey = Key(q.root);
    auto hiddenOn = [&](const std::wstring& path) {  // a folder on the way from the root to the file
        for (size_t at = path.find(L'\\', rootKey.size() + 1); at != std::wstring::npos; at = path.find(L'\\', at + 1)) {
            const std::wstring dir = path.substr(0, at), key = Key(dir);
            auto it = hiddenDir.find(key);
            if (it == hiddenDir.end()) {
                const DWORD a = GetFileAttributesW(LongPath(dir).c_str());
                it = hiddenDir.emplace(key, a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))).first;
            }
            if (it->second) return true;
        }
        return false;
    };
    for (const std::wstring& path : *q.picked) {
        if (GetTickCount64() > deadline) return false;
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &d)) continue;  // gone since the index
        if (d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) continue;
        if (!q.hidden && ((d.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) || hiddenOn(path))) continue;
        WalkFile f;
        f.path = path;
        f.key = Key(path);
        f.nameKey = Lower(LeafOf(path));
        const std::wstring ext = ExtOf(f.nameKey);
        f.size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        f.doc = DocExts().count(ext) != 0;
        if (f.doc && f.size > kDocMax) continue;
        f.binaryExt = BinaryExts().count(ext) != 0;
        (f.size > (256 << 10) && !f.binaryExt ? big : soon).push_back(std::move(f));
    }
    for (WalkFile& f : big) soon.push_back(std::move(f));
    std::atomic<size_t> next{0};
    std::atomic<bool> late{false};
    auto work = [&] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (size_t i; (i = next.fetch_add(1)) < soon.size();) {
            if (GetTickCount64() > deadline) {
                late = true;
                break;
            }
            onFile(soon[i]);
        }
        if (SUCCEEDED(co)) CoUninitialize();
    };
    const unsigned nth = static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>({12, std::thread::hardware_concurrency(), soon.size()})));
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < nth; ++i) workers.emplace_back(work);
    for (auto& t : workers) t.join();
    return !late;
}

// Folders kept in memory (cache.inl): an answer from there, or nothing; and a folder to keep after a walk.
std::optional<GrepResult> CachedGrep(const GrepQuery& q, const Matcher& m, const BytePre& pre);
void CacheAfterWalk(const GrepQuery& q, uint64_t textBytes);

// A leading inline flag group, as other regex engines take it: "(?i)foo" is "foo" ignoring case. The standard
// library's engine (ECMAScript) has none. m and s change nothing here, since each line is searched apart: they are taken
// and dropped.
std::wstring TakeInlineFlags(const std::wstring& p, bool& icase) {
    if (p.size() < 4 || p[0] != L'(' || p[1] != L'?') return p;
    size_t i = 2;
    while (i < p.size() && (p[i] == L'i' || p[i] == L'm' || p[i] == L's')) ++i;
    if (i == 2 || i >= p.size() || p[i] != L')') return p;
    if (p.find(L'i', 2) < i) icase = true;
    return p.substr(i + 1);
}

// For a regex that doesn't compile: what it most likely used that this engine lacks, and what to use instead.
std::wstring RegexHint(const std::wstring& p) {
    if (p.find(L"(?<=") != std::wstring::npos || p.find(L"(?<!") != std::wstring::npos)
        return L" Lookbehind, (?<=...) and (?<!...), isn't supported by this engine (ECMAScript); lookahead, (?=...) and (?!...), is.";
    if (p.find(L"(?P<") != std::wstring::npos || p.find(L"(?<") != std::wstring::npos || p.find(L"(?'") != std::wstring::npos)
        return L" Named groups aren't supported: use a plain group ( ).";
    for (size_t at = p.find(L"(?"); at != std::wstring::npos; at = p.find(L"(?", at + 2)) {
        size_t i = at + 2;
        while (i < p.size() && iswalpha(p[i])) ++i;
        if (i > at + 2 && i < p.size() && (p[i] == L')' || p[i] == L':'))
            return L" Inline flags like (?i) are only taken at the start of the pattern; for case, use case: \"insensitive\".";
    }
    if (p.find(L"\\A") != std::wstring::npos || p.find(L"\\z") != std::wstring::npos || p.find(L"\\Z") != std::wstring::npos)
        return L" \\A and \\z aren't supported: use ^ and $ (each line is searched apart).";
    return L"";
}

// A regex that is only plain texts joined by |, like "foo|bar baz|x\.y", which is how agents write most of theirs:
// those texts, to look for them as text instead of through the regex engine, which is many times slower. Nothing
// when the regex is anything more.
std::vector<std::wstring> PlainAlternatives(const std::wstring& p) {
    std::vector<std::wstring> out(1);
    for (size_t i = 0; i < p.size(); ++i) {
        const wchar_t c = p[i];
        if (c == L'|') {
            out.emplace_back();
        } else if (c == L'\\') {
            if (i + 1 >= p.size() || iswalnum(p[i + 1])) return {};  // \d, \b, \1: a class, an anchor, a reference
            out.back() += p[++i];  // an escaped sign is itself
        } else if (wcschr(L"^$.*+?()[]{}", c)) {
            return {};
        } else {
            out.back() += c;
        }
    }
    if (out.size() > 16) return {};
    for (const std::wstring& t : out)
        if (t.empty()) return {};  // "a|" matches every line
    return out;
}

GrepResult RunGrep(const GrepQuery& q) {
    GrepResult res;
    Matcher m;
    m.regex = q.regex;
    bool icase = q.ignoreCase, withoutIcase = q.ignoreCase;
    const std::wstring pattern = q.regex ? TakeInlineFlags(q.pattern, icase) : q.pattern;
    const std::wstring without = q.regex ? TakeInlineFlags(q.without, withoutIcase) : q.without;
    m.icase = icase;
    m.pat = icase && !q.regex ? Lower(pattern) : pattern;
    const std::vector<std::wstring> plain = q.regex ? PlainAlternatives(pattern) : std::vector<std::wstring>{};
    for (const std::wstring& t : plain) m.alts.push_back(icase ? Lower(t) : t);
    auto flags = [](bool ic) { return ic ? std::regex::ECMAScript | std::regex::icase | std::regex::optimize : std::regex::ECMAScript | std::regex::optimize; };
    if (q.regex) {
        try {
            m.re = std::wregex(pattern, flags(icase));
        } catch (const std::regex_error& e) {
            res.error = L"The regex doesn't compile: " + Wide(e.what()) + L"." + RegexHint(pattern);
            return res;
        }
    }
    if (!without.empty()) {
        m.without = true;
        m.withoutPat = withoutIcase && !q.regex ? Lower(without) : without;
        if (q.regex) {
            try {
                m.withoutRe = std::wregex(without, flags(withoutIcase));
            } catch (const std::regex_error& e) {
                res.error = L"The regex in without doesn't compile: " + Wide(e.what()) + L"." + RegexHint(without);
                return res;
            }
        }
    }
    const BytePre pre = MakeBytePre(q.regex ? plain : std::vector<std::wstring>{pattern}, icase);
    if (auto kept = CachedGrep(q, m, pre)) return std::move(*kept);
    const uint64_t deadline = GetTickCount64() + q.budgetMs;
    std::mutex fm;
    std::atomic<uint64_t> searched{0}, docs{0}, big{0}, binaries{0}, unreadable{0}, textBytes{0};

    auto searchFile = [&](const WalkFile& j) {
        if (PackageExts().count(ExtOf(j.nameKey))) {  // a 3MF: its text parts
            std::vector<FileHits> parts;
            int tooBig = 0;
            const bool read = SearchPackage(j.path, m, pre, q.perFile, q.context, deadline, parts, tooBig);
            ++searched;
            ++docs;
            std::lock_guard lk(fm);
            if (!read) {
                ++unreadable;
                if (res.unreadableFiles.size() < kNamedSkips) res.unreadableFiles.push_back(j.path);
            }
            if (tooBig) {
                ++big;
                if (res.bigFiles.size() < kNamedSkips) res.bigFiles.push_back({j.path, j.size});
            }
            for (FileHits& f : parts) res.files.push_back(std::move(f));
            return;
        }
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
    } else if (q.picked) {
        res.picked = true;
        done = SearchPicked(q, deadline, [&](const WalkFile& f) {
            if (f.binaryExt) ++binaries;
            else if (!f.doc || q.documents) {
                {
                    std::lock_guard lk(fm);
                    ++res.kinds[ExtOf(f.nameKey)];
                }
                searchFile(f);
            }
        });
    } else {
        const bool inJunk = q.ignored || AnySegment(rootKey, std::vector<std::wstring>(JunkDirs().begin(), JunkDirs().end()));
        const std::atomic<bool> never{false};
        done = WalkFolder(q.root, q.ignored ? IgnChain{} : IgnoresAbove(q.root), q, inJunk, deadline, never,
                          [](const std::wstring&, const IgnChain&) {},
                          [&](const WalkFile& f) {
                              if (f.binaryExt) ++binaries;
                              else if (!f.doc || q.documents) {
                                  {
                                      std::lock_guard lk(fm);
                                      ++res.kinds[ExtOf(f.nameKey)];
                                  }
                                  searchFile(f);
                              }
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
    if (done && !q.picked && ra != INVALID_FILE_ATTRIBUTES && (ra & FILE_ATTRIBUTE_DIRECTORY)) CacheAfterWalk(q, textBytes);
    return res;
}
