// The drive index: every name on every fixed drive, the same format Jolt keeps. With Jolt installed, its index
// (%LOCALAPPDATA%\Jolt\index-X.bin) is copied, not used in place: Jolt replaces that file when it reindexes, and a
// file that another process has mapped can't be replaced. Without Jolt, the drives are walked once (minutes on a big
// one) and that is saved. What changes afterwards comes from the drive watchers (activity.inl).

struct IdxNode {
    uint32_t parent;  // UINT32_MAX: the root
    uint32_t nameOff;
    uint16_t nameLen;
    uint8_t dir;
    uint8_t depth;
};
static_assert(sizeof(IdxNode) == 12, "the file format");

struct Volume {
    wchar_t letter = 0;
    const IdxNode* node = nullptr;
    const wchar_t* name = nullptr;
    size_t count = 0;
    const void* view = nullptr;
    std::vector<IdxNode> nodes;  // while it is built here
    std::vector<wchar_t> names;
    int64_t stamp = 0;  // when the index was taken
    // Each folder's children, made the first time a search is scoped to a folder: the index lists names in the order
    // of the drive's file table, so a folder's contents are scattered through all of it.
    mutable std::once_flag kidsOnce;
    mutable std::vector<uint32_t> kidStart, kids;  // the children of n: kids[kidStart[n]] up to kids[kidStart[n + 1]]
    void Kids() const {
        std::call_once(kidsOnce, [this] {
            kidStart.assign(count + 1, 0);
            for (size_t i = 1; i < count; ++i)
                if (node[i].parent < count) ++kidStart[node[i].parent + 1];
            for (size_t i = 0; i < count; ++i) kidStart[i + 1] += kidStart[i];
            kids.resize(kidStart[count]);
            std::vector<uint32_t> at(kidStart.begin(), kidStart.end() - 1);
            for (size_t i = 1; i < count; ++i)
                if (node[i].parent < count) kids[at[node[i].parent]++] = static_cast<uint32_t>(i);
        });
    }
    Volume() = default;
    Volume(const Volume&) = delete;
    ~Volume() {
        if (view) UnmapViewOfFile(view);
    }
    void Seal() {
        node = nodes.data();
        name = names.data();
        count = nodes.size();
    }
};

std::mutex g_volM;
std::vector<std::shared_ptr<const Volume>> g_vols;
std::atomic<bool> g_indexReady{false};

std::vector<std::shared_ptr<const Volume>> Volumes() {
    std::lock_guard lk(g_volM);
    return g_vols;
}

void Publish(std::shared_ptr<const Volume> v) {
    std::lock_guard lk(g_volM);
    for (auto& x : g_vols)
        if (x->letter == v->letter) {
            x = std::move(v);
            return;
        }
    g_vols.push_back(std::move(v));
}

std::vector<wchar_t> FixedDrives() {
    std::vector<wchar_t> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_FIXED) out.push_back(root[0]);
    }
    return out;
}

std::shared_ptr<Volume> MapIndex(const std::wstring& file, wchar_t letter) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    HANDLE m = GetFileSizeEx(h, &size) && size.QuadPart >= 12 ? CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    CloseHandle(h);
    if (!m) return nullptr;
    const void* view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);
    if (!view) return nullptr;
    const auto* head = static_cast<const uint32_t*>(view);
    const uint64_t want = 12 + static_cast<uint64_t>(head[1]) * sizeof(IdxNode) + static_cast<uint64_t>(head[2]) * sizeof(wchar_t);
    if (head[0] != 0x58445952 || static_cast<uint64_t>(size.QuadPart) != want || head[1] == 0) {
        UnmapViewOfFile(view);
        return nullptr;
    }
    auto v = std::make_shared<Volume>();
    v->letter = letter;
    v->view = view;
    v->count = head[1];
    v->node = reinterpret_cast<const IdxNode*>(head + 3);
    v->name = reinterpret_cast<const wchar_t*>(v->node + head[1]);
    return v;
}

bool SaveIndex(const Volume& v, const std::wstring& file) {
    const std::wstring tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const uint32_t head[3] = {0x58445952, static_cast<uint32_t>(v.nodes.size()), static_cast<uint32_t>(v.names.size())};
    DWORD w;
    const bool ok = WriteFile(h, head, sizeof(head), &w, nullptr) &&
                    WriteFile(h, v.nodes.data(), static_cast<DWORD>(v.nodes.size() * sizeof(IdxNode)), &w, nullptr) &&
                    WriteFile(h, v.names.data(), static_cast<DWORD>(v.names.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void AddNode(Volume& v, uint32_t parent, const wchar_t* name, size_t len, bool dir) {
    IdxNode n;
    n.parent = parent;
    n.nameOff = static_cast<uint32_t>(v.names.size());
    n.nameLen = static_cast<uint16_t>(std::min<size_t>(len, 65535));
    n.dir = dir;
    n.depth = parent == UINT32_MAX ? 0 : static_cast<uint8_t>(std::min(250, v.nodes[parent].depth + 1));
    v.names.insert(v.names.end(), name, name + n.nameLen);
    v.nodes.push_back(n);
}

// Calls visit for everything below root, depth first, until the deadline (GetTickCount64). visit returns whether to
// go into a folder.
void WalkTree(const std::wstring& root, uint64_t deadline, const std::function<bool(const std::wstring&, const WIN32_FIND_DATAW&)>& visit) {
    std::vector<std::wstring> stack{root};
    WIN32_FIND_DATAW fd;
    while (!stack.empty()) {
        if (deadline && GetTickCount64() > deadline) return;
        const std::wstring dir = std::move(stack.back());
        stack.pop_back();
        const std::wstring pattern = (dir.size() >= MAX_PATH - 2 ? L"\\\\?\\" + dir : dir) + (dir.back() == L'\\' ? L"*" : L"\\*");
        HANDLE f = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> sub;
        do {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
            const std::wstring p = Join(dir, fd.cFileName);
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (visit(p, fd) && isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) sub.push_back(p);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        for (size_t i = sub.size(); i-- > 0;) stack.push_back(std::move(sub[i]));  // so they come out in order
    }
}

// A whole drive, without Jolt (no special permissions: minutes for millions of files).
std::shared_ptr<Volume> BuildIndex(wchar_t letter) {
    auto v = std::make_shared<Volume>();
    v->letter = letter;
    const wchar_t root[3] = {letter, L':', 0};
    AddNode(*v, UINT32_MAX, root, 2, true);
    std::vector<std::pair<uint32_t, std::wstring>> stack{{0, std::wstring(root) + L"\\"}};
    WIN32_FIND_DATAW fd;
    while (!stack.empty()) {
        auto [idx, dir] = std::move(stack.back());
        stack.pop_back();
        HANDLE f = FindFirstFileExW((dir + (dir.back() == L'\\' ? L"*" : L"\\*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const uint32_t me = static_cast<uint32_t>(v->nodes.size());
            AddNode(*v, idx, fd.cFileName, wcslen(fd.cFileName), isDir);
            if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back({me, Join(dir, fd.cFileName)});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    v->Seal();
    v->stamp = NowFt();
    return v;
}

std::wstring JoltIndex(wchar_t c) { return LocalAppData() + L"\\Jolt\\index-" + std::wstring(1, c) + L".bin"; }

// Its own copies carry the time they were taken in the name (index-C-<time>.bin), so a newer copy never has to
// overwrite one that is still mapped. Returns the newest; the older ones are deleted when nothing maps them.
std::wstring NewestCopy(wchar_t c, int64_t& stamp) {
    const std::wstring prefix = L"index-" + std::wstring(1, c) + L"-";
    std::vector<std::pair<int64_t, std::wstring>> all;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((DataDir() + L"\\" + prefix + L"*.bin").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do all.push_back({_wcstoi64(fd.cFileName + prefix.size(), nullptr, 10), DataDir() + L"\\" + fd.cFileName});
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i + 1 < all.size(); ++i) DeleteFileW(all[i].second.c_str());
    stamp = all.empty() ? 0 : all.back().first;
    return all.empty() ? L"" : all.back().second;
}

std::wstring CopyName(wchar_t c, int64_t stamp) { return DataDir() + L"\\index-" + std::wstring(1, c) + L"-" + std::to_wstring(stamp) + L".bin"; }

// Brings a drive's index up to date: a newer one from Jolt is copied; with none at all and build, it is walked.
// Returns whether the drive has an index now.
bool LoadVolume(wchar_t c, bool build) {
    int64_t ours = 0;
    std::wstring file = NewestCopy(c, ours);
    const int64_t jolt = MTimeOf(JoltIndex(c));
    if (jolt && jolt > ours) {
        const std::wstring f = CopyName(c, jolt), tmp = f + L".tmp";
        if (CopyFileW(JoltIndex(c).c_str(), tmp.c_str(), FALSE) && MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            file = f;
            ours = jolt;
        } else {
            DeleteFileW(tmp.c_str());
        }
    }
    {
        std::lock_guard lk(g_volM);
        for (auto& v : g_vols)
            if (v->letter == c && v->stamp >= ours && ours) return true;  // already that one
    }
    if (!file.empty())
        if (auto v = MapIndex(file, c)) {
            v->stamp = ours;
            Publish(v);
            NewestCopy(c, ours);  // the older copies go when nothing maps them
            return true;
        }
    if (!build) return false;
    auto v = BuildIndex(c);
    const int64_t stamp = v->stamp;
    Publish(v);
    const std::wstring f = CopyName(c, stamp);
    if (SaveIndex(*v, f))
        if (auto m = MapIndex(f, c)) {
            m->stamp = stamp;
            Publish(m);
        }
    return true;
}

std::wstring NodePath(const Volume& v, uint32_t i) {
    uint32_t chain[260];
    int n = 0;
    for (uint32_t c = i; c != UINT32_MAX && n < 260; c = v.node[c].parent) chain[n++] = c;
    std::wstring p;
    for (int k = n; k-- > 0;) {
        const IdxNode& nd = v.node[chain[k]];
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        p.append(v.name + nd.nameOff, nd.nameLen);
        if (k == n - 1) p += L'\\';  // "C:\"
    }
    return p;
}

// A folder's node in a volume: each step looks for a child of the one before.
bool FindNode(const Volume& v, const std::wstring& path, uint32_t& out) {
    if (path.size() < 3 || towupper(path[0]) != v.letter) return false;
    v.Kids();
    uint32_t cur = 0;
    size_t at = 3;
    while (at < path.size()) {
        size_t end = path.find(L'\\', at);
        if (end == std::wstring::npos) end = path.size();
        const size_t len = end - at;
        uint32_t found = UINT32_MAX;
        for (uint32_t k = v.kidStart[cur]; k < v.kidStart[cur + 1]; ++k) {
            const IdxNode& nd = v.node[v.kids[k]];
            if (nd.nameLen == len && _wcsnicmp(v.name + nd.nameOff, path.c_str() + at, len) == 0) {
                found = v.kids[k];
                break;
            }
        }
        if (found == UINT32_MAX) return false;
        cur = found;
        at = end + 1;
    }
    out = cur;
    return true;
}

bool InScope(const Volume& v, uint32_t i, uint32_t scope, uint8_t scopeDepth) {
    while (v.node[i].depth > scopeDepth) i = v.node[i].parent;
    return i == scope;
}

// ---- a name query: words are fuzzy (all of them must match), ".png" or "ext:png,jpg" the extension, "!word" leaves
// out names with it, "*" and "?" are wildcards on the name, "type:file|folder", and anything with a slash must be in
// the path.

struct NameQuery {
    std::vector<std::wstring> words, exts, notWords, globs, pathParts;
    int kind = 0;  // 1 folders, 2 files
    std::wstring text;
    bool Empty() const { return words.empty() && exts.empty() && globs.empty() && pathParts.empty(); }
};

NameQuery ParseNameQuery(const std::wstring& q) {
    NameQuery out;
    out.text = q;
    std::vector<std::wstring> tokens;
    std::wstring cur;
    bool quoted = false;
    for (wchar_t c : q) {
        if (c == L'"') {
            quoted = !quoted;
            continue;
        }
        if (iswspace(c) && !quoted) {
            if (!cur.empty()) tokens.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    for (std::wstring t : tokens) {
        const std::wstring l = Lower(t);
        if (l == L"type:file" || l == L"type:files") out.kind = 2;
        else if (l == L"type:folder" || l == L"type:folders" || l == L"type:dir") out.kind = 1;
        else if (l.rfind(L"ext:", 0) == 0) {
            size_t at = 4;
            while (at < l.size()) {
                size_t end = l.find(L',', at);
                if (end == std::wstring::npos) end = l.size();
                std::wstring e = l.substr(at, end - at);
                if (!e.empty()) out.exts.push_back(e[0] == L'.' ? e : L"." + e);
                at = end + 1;
            }
        } else if (l.size() > 1 && l[0] == L'!') out.notWords.push_back(l.substr(1));
        else if (l.size() > 1 && l[0] == L'.' && l.find_first_of(L".*?\\/", 1) == std::wstring::npos) out.exts.push_back(l);
        else if (l.find_first_of(L"*?") != std::wstring::npos) out.globs.push_back(l);
        else if (l.find_first_of(L"\\/") != std::wstring::npos) {
            std::replace(t.begin(), t.end(), L'/', L'\\');
            out.pathParts.push_back(Lower(t));
        } else out.words.push_back(l);
    }
    return out;
}

// * any run without a separator, ** any run, ? one character, [abc] [a-z] [!x]; case-insensitive, '\' separates.
bool GlobMatch(const wchar_t* p, const wchar_t* s) {
    while (*p) {
        if (*p == L'*') {
            const bool deep = p[1] == L'*';
            const wchar_t* rest = p + (deep ? 2 : 1);
            if (deep && *rest == L'\\' && GlobMatch(rest + 1, s)) return true;  // "**\" also matches no folder at all
            for (const wchar_t* t = s;; ++t) {
                if (GlobMatch(rest, t)) return true;
                if (!*t || (!deep && *t == L'\\')) return false;
            }
        }
        if (!*s) return false;
        if (*p == L'?') {
            if (*s == L'\\') return false;
        } else if (*p == L'[') {
            const wchar_t* q = p + 1;
            const bool neg = *q == L'!' || *q == L'^';
            if (neg) ++q;
            bool hit = false;
            const wchar_t c = static_cast<wchar_t>(towlower(*s));
            while (*q && *q != L']') {
                wchar_t a = static_cast<wchar_t>(towlower(*q)), b = a;
                if (q[1] == L'-' && q[2] && q[2] != L']') {
                    b = static_cast<wchar_t>(towlower(q[2]));
                    q += 2;
                }
                if (c >= a && c <= b) hit = true;
                ++q;
            }
            if (!*q || hit == neg) return false;
            p = q;
        } else if (towlower(*p) != towlower(*s)) {
            return false;
        }
        ++p;
        ++s;
    }
    return !*s;
}

// Does a name pass the query? score: how well (FuzzyMatch plus exact names and programs).
bool NameMatch(const NameQuery& q, const wchar_t* name, size_t n, bool dir, int& score, std::wstring& lowerBuf) {
    if ((q.kind == 1 && !dir) || (q.kind == 2 && dir)) return false;
    if (!q.exts.empty()) {
        if (dir) return false;
        bool ok = false;
        for (const std::wstring& e : q.exts)
            if (n > e.size() && _wcsnicmp(name + n - e.size(), e.c_str(), e.size()) == 0) ok = true;
        if (!ok) return false;
    }
    score = 0;
    for (const std::wstring& w : q.words) {
        int s;
        if (!FuzzyMatch(name, n, w, s, nullptr)) return false;
        score += s;
    }
    if (!q.notWords.empty() || !q.globs.empty()) {
        lowerBuf.assign(name, n);
        CharLowerBuffW(lowerBuf.data(), static_cast<DWORD>(n));
        for (const std::wstring& w : q.notWords)
            if (lowerBuf.find(w) != std::wstring::npos) return false;
        for (const std::wstring& g : q.globs)
            if (!GlobMatch(g.c_str(), lowerBuf.c_str())) return false;
    }
    if (q.words.size() == 1) score += NameRank(name, n, q.words[0]);
    return true;
}

// Something created after the index was taken (the watchers see it).
struct AddedItem {
    std::wstring path;
    bool dir = false;
    std::wstring key;  // lowercase path
};

// The best candidates from the index, by score.
struct Cand {
    int key;  // for keeping the best: score*8 - depth, or the close-name score
    uint32_t vol, node;  // vol == UINT32_MAX: from the paths created since (node indexes them)
    int16_t percent;  // close name: how close; 0 for real matches
    int rank;  // NameRank part
};

struct ScanOut {
    std::vector<Cand> best;
    uint64_t total = 0;
};

// Runs every name through match (on several threads), keeping the `keep` best. scope: only below that folder.
template <class F>
ScanOut ScanIndex(const std::vector<std::shared_ptr<const Volume>>& vols, const std::vector<AddedItem>& added, size_t keep,
                  const std::wstring& scopePath, F match) {
    struct Scope {
        bool any = false;
        uint32_t node = 0;
        uint8_t depth = 0;
    };
    std::vector<Scope> scopes(vols.size());
    std::vector<std::vector<uint32_t>> below(vols.size());  // scoped: the nodes below the scope, from the children lists
    size_t work = 0;
    const std::wstring scopeKey = Key(scopePath);
    for (size_t i = 0; i < vols.size(); ++i) {
        if (scopePath.empty()) {
            scopes[i].any = true;
            work += vols[i]->count;
            continue;
        }
        uint32_t node;
        if (FindNode(*vols[i], scopePath, node)) {
            scopes[i] = {false, node, vols[i]->node[node].depth};
            const Volume& v = *vols[i];
            std::vector<uint32_t> stack{node};
            while (!stack.empty()) {
                const uint32_t n = stack.back();
                stack.pop_back();
                for (uint32_t k = v.kidStart[n]; k < v.kidStart[n + 1]; ++k) {
                    below[i].push_back(v.kids[k]);
                    stack.push_back(v.kids[k]);
                }
            }
            work += below[i].size();
        } else {
            scopes[i] = {false, UINT32_MAX, 0};
        }
    }
    const unsigned nth = work < 100000 ? 1u : std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::vector<Cand>> part(nth);
    std::vector<uint64_t> totals(nth, 0);
    auto worse = [](const Cand& a, const Cand& b) { return a.key > b.key; };  // min-heap on key
    std::vector<std::thread> th;
    auto scan = [&](unsigned t) {
            auto& heap = part[t];
            std::wstring buf;
            for (uint32_t vi = 0; vi < vols.size(); ++vi) {
                const Volume& v = *vols[vi];
                const Scope& sc = scopes[vi];
                if (!sc.any && sc.node == UINT32_MAX) continue;
                const size_t n = sc.any ? v.count : below[vi].size();
                for (size_t j = (sc.any ? 1 : 0) + t; j < n; j += nth) {
                    const uint32_t i = sc.any ? static_cast<uint32_t>(j) : below[vi][j];
                    const IdxNode& nd = v.node[i];
                    Cand c{0, vi, i, 0, 0};
                    if (!match(v.name + nd.nameOff, nd.nameLen, nd.dir != 0, nd.depth, c, buf)) continue;
                    ++totals[t];
                    if (heap.size() < keep) {
                        heap.push_back(c);
                        std::push_heap(heap.begin(), heap.end(), worse);
                    } else if (c.key > heap.front().key) {
                        std::pop_heap(heap.begin(), heap.end(), worse);
                        heap.back() = c;
                        std::push_heap(heap.begin(), heap.end(), worse);
                    }
                }
            }
    };
    if (nth == 1) scan(0);  // a small folder: not worth starting threads
    else
        for (unsigned t = 0; t < nth; ++t) th.emplace_back(scan, t);
    for (auto& x : th) x.join();
    ScanOut out;
    for (unsigned t = 0; t < nth; ++t) {
        out.best.insert(out.best.end(), part[t].begin(), part[t].end());
        out.total += totals[t];
    }
    // created since the index was taken
    std::wstring buf;
    for (uint32_t i = 0; i < added.size(); ++i) {
        const std::wstring& p = added[i].path;
        if (!scopePath.empty() && !IsUnder(added[i].key.empty() ? Key(p) : added[i].key, scopeKey)) continue;
        const std::wstring leaf = LeafOf(p);
        const uint8_t depth = static_cast<uint8_t>(std::min<size_t>(250, std::count(p.begin(), p.end(), L'\\')));
        Cand c{0, UINT32_MAX, i, 0, 0};
        if (!match(leaf.c_str(), leaf.size(), added[i].dir, depth, c, buf)) continue;
        ++out.total;
        out.best.push_back(c);
    }
    std::sort(out.best.begin(), out.best.end(), [](const Cand& a, const Cand& b) { return a.key > b.key; });
    if (out.best.size() > keep) out.best.resize(keep);
    return out;
}
