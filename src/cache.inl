// Folders kept in memory, so searching one again doesn't read the drive. After a folder's first search, the background
// process reads its files into memory on a thread of its own, and the drive watchers keep them current: a file that
// changes is read again on the next search, one that appears is added, one that goes is dropped. Only in the background
// process, for the default rules (no hidden or ignored files: those searches walk the folder as always), up to a memory
// budget: "cache = 512" in seek.txt is the default, in MB; 0 turns it off. A folder nobody searched for 20 minutes is
// let go.

enum : uint8_t { kcText, kcBinary, kcBinaryExt, kcBig, kcFail, kcDoc, kcNoFilter };

struct CFile {
    std::wstring path, nameKey;
    uint64_t size = 0;
    uint8_t kind = kcText;
    Raw raw;           // text: its bytes
    std::wstring doc;  // a document: its text
    uint64_t Bytes() const { return raw.n + doc.size() * sizeof(wchar_t) + (path.size() + nameKey.size()) * 4 + 96; }
};

struct KeptFolder {
    std::wstring root, rootKey;
    bool inJunk = false;
    std::shared_mutex m;                               // the maps: shared to search them, exclusive to change them
    std::unordered_map<std::wstring, IgnChain> dirs;   // the folders walked, with the rules for their entries
    std::unordered_map<std::wstring, CFile> files;     // by key
    // with g_cacheM held
    bool ready = false, stale = false;
    uint64_t bytes = 0, used = 0;
    std::vector<std::wstring> dirty;  // paths the watchers saw change since
};

std::mutex g_cacheM;
std::condition_variable g_cacheCv;
std::vector<std::shared_ptr<KeptFolder>> g_folders;
std::unordered_map<std::wstring, uint64_t> g_tooBig;  // roots over the budget, and when that was seen
bool g_building = false;
constexpr uint64_t kCacheIdle = 20 * 60 * 1000, kTooBigWait = 30 * 60 * 1000;
constexpr size_t kDirtyMax = 20000;  // more changes than this before a search: read the folder again instead

uint64_t CacheBudget() { return g_rules.cacheMb << 20; }

void CacheDrop(const std::shared_ptr<KeptFolder>& f) {  // with g_cacheM held
    g_folders.erase(std::remove(g_folders.begin(), g_folders.end(), f), g_folders.end());
    g_cacheCv.notify_all();
}

// One file into memory: a text file's bytes, a document's text, or only what it is.
void LoadCFile(CFile& c, const WalkFile& w) {
    c.path = w.path;
    c.nameKey = w.nameKey;
    c.size = w.size;
    if (w.binaryExt) {
        c.kind = kcBinaryExt;
        return;
    }
    if (w.doc) {
        c.kind = DocText(w.path, c.doc, GetTickCount64() + 4000) ? kcDoc : kcNoFilter;
        return;
    }
    switch (ReadRaw(w.path, w.size, c.raw)) {
        case ReadOutcome::Ok: c.kind = kcText; break;
        case ReadOutcome::Binary: c.kind = kcBinary; break;
        case ReadOutcome::Big: c.kind = kcBig; break;
        default: c.kind = kcFail; break;
    }
}

// Reads a folder into memory, on its own thread.
void CacheBuild(std::shared_ptr<KeptFolder> f) {
    std::unordered_map<std::wstring, IgnChain> dirs;
    std::unordered_map<std::wstring, CFile> files;
    std::mutex lm;
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool> over{false};
    const uint64_t budget = CacheBudget();
    GrepQuery wq;
    wq.root = f->root;
    WalkFolder(
        f->root, IgnoresAbove(f->root), wq, f->inJunk, UINT64_MAX, over,
        [&](const std::wstring& key, const IgnChain& chain) {
            std::lock_guard lk(lm);
            dirs[key] = chain;
        },
        [&](const WalkFile& w) {
            CFile c;
            LoadCFile(c, w);
            if ((bytes += c.Bytes()) > budget) over = true;
            std::lock_guard lk(lm);
            files.emplace(w.key, std::move(c));
        });
    if (!over) {
        std::unique_lock fl(f->m);
        f->dirs = std::move(dirs);
        f->files = std::move(files);
    }
    std::lock_guard lk(g_cacheM);
    g_building = false;
    if (over) {
        g_tooBig[f->rootKey] = GetTickCount64();
        CacheDrop(f);
        return;
    }
    f->bytes = bytes;
    f->ready = true;
    f->used = GetTickCount64();
    // what this folder holds needs no copy of its own, and the least used others go while over the budget
    for (size_t i = g_folders.size(); i-- > 0;)
        if (g_folders[i] != f && IsUnder(g_folders[i]->rootKey, f->rootKey)) g_folders.erase(g_folders.begin() + i);
    for (;;) {
        uint64_t total = 0;
        std::shared_ptr<KeptFolder> oldest;
        for (const auto& x : g_folders) {
            total += x->bytes;
            if (x != f && x->ready && (!oldest || x->used < oldest->used)) oldest = x;
        }
        if (total <= budget || !oldest) break;
        CacheDrop(oldest);
    }
    g_cacheCv.notify_all();
}

// After a search walked a folder: keep it in memory, if it fits and the watchers can keep it current.
void CacheAfterWalk(const GrepQuery& q, uint64_t textBytes) {
    const uint64_t budget = CacheBudget();
    if (!g_watching || !budget || q.hidden || q.ignored || q.root.size() < 3 || q.root[1] != L':') return;
    if (GetDriveTypeW(q.root.substr(0, 3).c_str()) != DRIVE_FIXED) return;  // only fixed drives are watched
    const std::wstring key = Key(q.root);
    std::lock_guard lk(g_cacheM);
    if (textBytes > budget) {
        g_tooBig[key] = GetTickCount64();
        return;
    }
    if (g_building) return;
    if (auto it = g_tooBig.find(key); it != g_tooBig.end() && GetTickCount64() - it->second < kTooBigWait) return;
    for (const auto& x : g_folders)
        if (key == x->rootKey || IsUnder(key, x->rootKey)) return;  // kept already, or left out there by its rules
    auto f = std::make_shared<KeptFolder>();
    f->root = q.root;
    f->rootKey = key;
    f->inJunk = AnySegment(key, std::vector<std::wstring>(JunkDirs().begin(), JunkDirs().end()));
    f->used = GetTickCount64();
    g_folders.push_back(f);  // before reading, so the changes made meanwhile are seen
    g_building = true;
    std::thread(CacheBuild, f).detach();
}

// ---- between runs: which folders were kept, so that a new background process (after an update, a restart) reads
// them again at its start, instead of leaving the first search of each to walk a drive that may not have them cached.
// Only their paths are saved; a folder nobody searched for 20 minutes was let go, and isn't read again.

std::vector<std::wstring> g_warmPending;  // still to read at this start; with g_cacheM held

std::wstring KeptFile() { return DataDir() + L"\\kept.txt"; }

void CacheSaveList() {
    if (!g_watching) return;
    std::string out = "# Seek: the folders kept in memory for grep, read again at the next start. Rewritten automatically.\n";
    {
        std::lock_guard lk(g_cacheM);
        for (const auto& f : g_folders) out += Utf8(f->root) + "\n";
        for (const std::wstring& r : g_warmPending) out += Utf8(r) + "\n";
    }
    const std::wstring f = KeptFile(), tmp = f + L".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << out;
    }
    MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// At the start of the background process, on a thread of its own: reads the folders the last one kept, one at a
// time. A search of one of them meanwhile waits for it, as it does for any folder being read.
void CacheWarm() {
    std::vector<std::wstring> roots;
    {
        std::ifstream in(KeptFile(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] != '#') roots.push_back(Normalize(Wide(line)));
        }
    }
    if (roots.empty() || !CacheBudget() || !g_watching) return;
    {
        std::lock_guard lk(g_cacheM);
        g_warmPending = roots;
    }
    for (const std::wstring& root : roots) {
        const std::wstring key = Key(root);
        const DWORD at = GetFileAttributesW(LongPath(root).c_str());
        const bool ok = root.size() >= 3 && root[1] == L':' && GetDriveTypeW(root.substr(0, 3).c_str()) == DRIVE_FIXED && at != INVALID_FILE_ATTRIBUTES &&
                        (at & FILE_ATTRIBUTE_DIRECTORY) && !Private(key);
        std::shared_ptr<KeptFolder> f;
        {
            std::unique_lock lk(g_cacheM);
            g_cacheCv.wait(lk, [] { return !g_building; });
            g_warmPending.erase(std::remove(g_warmPending.begin(), g_warmPending.end(), root), g_warmPending.end());
            bool skip = !ok;
            for (const auto& x : g_folders) skip = skip || key == x->rootKey || IsUnder(key, x->rootKey);  // a search got there first
            if (skip) continue;
            f = std::make_shared<KeptFolder>();
            f->root = root;
            f->rootKey = key;
            f->inJunk = AnySegment(key, std::vector<std::wstring>(JunkDirs().begin(), JunkDirs().end()));
            f->used = GetTickCount64();
            g_folders.push_back(f);  // before reading, so the changes made meanwhile are seen
            g_building = true;
        }
        CacheBuild(f);
    }
}

// From the drive watchers: something changed at path.
void CacheChanged(const std::wstring& path) {
    std::lock_guard lk(g_cacheM);
    if (g_folders.empty()) return;
    const std::wstring key = Key(path);
    for (const auto& f : g_folders) {
        if (f->stale || (key != f->rootKey && !IsUnder(key, f->rootKey))) continue;
        if (f->ready && !key.ends_with(L"\\.git\\info\\exclude")) {
            // Only what is in a folder that was walked, or is one, can change an answer. Without waiting: CacheApply
            // holds the folder for as long as it reads what changed, and this thread, a drive's watcher, must not fall
            // behind (what it doesn't look at goes to the list, and CacheApply sorts it out).
            std::shared_lock sl(f->m, std::try_to_lock);
            if (sl.owns_lock() && !f->dirs.count(key) && !f->dirs.count(Key(ParentOf(path)))) continue;
        }
        if (f->dirty.size() >= kDirtyMax) f->stale = true;
        else f->dirty.push_back(path);
    }
}

// From the drive watchers: changes were lost on that drive, so what is kept there may be out of date.
void CacheLost(wchar_t letter) {
    std::lock_guard lk(g_cacheM);
    for (const auto& f : g_folders)
        if (towupper(f->root[0]) == towupper(letter)) f->stale = true;
}

void CacheExpire() {
    std::lock_guard lk(g_cacheM);
    const uint64_t now = GetTickCount64();
    for (size_t i = g_folders.size(); i-- > 0;)
        if (g_folders[i]->ready && now - g_folders[i]->used > kCacheIdle) CacheDrop(g_folders[i]);
    for (auto it = g_tooBig.begin(); it != g_tooBig.end();) it = now - it->second > kTooBigWait ? g_tooBig.erase(it) : std::next(it);
}

// Brings a kept folder up to date with what the watchers saw. false: it can't be, and was let go.
bool CacheApply(const std::shared_ptr<KeptFolder>& f) {
    std::vector<std::wstring> dirty;
    {
        std::lock_guard lk(g_cacheM);
        if (f->stale) {
            CacheDrop(f);
            return false;
        }
        dirty.swap(f->dirty);
    }
    if (dirty.empty()) return true;
    bool stale = false;
    int64_t delta = 0;
    std::thread([&] {  // with COM ready, for documents' filters
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_lock fl(f->m);
        auto dropTree = [&](const std::wstring& dirKey) {
            for (auto it = f->files.begin(); it != f->files.end();)
                if (IsUnder(it->first, dirKey)) {
                    delta -= static_cast<int64_t>(it->second.Bytes());
                    it = f->files.erase(it);
                } else {
                    ++it;
                }
            for (auto it = f->dirs.begin(); it != f->dirs.end();) it = (it->first == dirKey || IsUnder(it->first, dirKey)) ? f->dirs.erase(it) : std::next(it);
        };
        auto dropFile = [&](const std::wstring& key) {
            if (auto it = f->files.find(key); it != f->files.end()) {
                delta -= static_cast<int64_t>(it->second.Bytes());
                f->files.erase(it);
            }
        };
        std::unordered_set<std::wstring> seen;
        for (const std::wstring& path : dirty) {
            const std::wstring key = Key(path), nameKey = Lower(LeafOf(path));
            if (!seen.insert(key).second) continue;
            if (nameKey == L".gitignore" || nameKey == L".ignore" || key.ends_with(L"\\.git\\info\\exclude") || key == f->rootKey) {
                // the rules changed, or the folder itself: read it again
                if (key != f->rootKey || GetFileAttributesW(LongPath(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
                    stale = true;
                    break;
                }
                continue;
            }
            const auto parent = f->dirs.find(Key(ParentOf(path)));
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (!FileInfo(path, d)) {  // gone
                if (f->dirs.count(key)) dropTree(key);
                else dropFile(key);
                continue;
            }
            if (parent == f->dirs.end()) continue;  // in a folder that isn't kept
            const DWORD at = d.dwFileAttributes;
            // the checks a walk makes
            bool keep = !PrivateLeaf(key, nameKey) && !(at & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
            if (at & FILE_ATTRIBUTE_DIRECTORY) {
                keep = keep && !(at & FILE_ATTRIBUTE_REPARSE_POINT) && (f->inJunk || !JunkDirs().count(nameKey)) &&
                       !Ignored(parent->second, key, nameKey, true);
                if (f->dirs.count(key)) {  // known: its entries come as changes of their own
                    if (!keep) dropTree(key);
                    continue;
                }
                if (!keep) continue;
                // a folder made or moved here: walk it
                std::unordered_map<std::wstring, IgnChain> dirs;
                std::unordered_map<std::wstring, CFile> files;
                std::mutex lm;
                GrepQuery wq;
                wq.root = path;
                const std::atomic<bool> never{false};
                WalkFolder(
                    path, parent->second, wq, f->inJunk, UINT64_MAX, never,
                    [&](const std::wstring& k, const IgnChain& chain) {
                        std::lock_guard lk(lm);
                        dirs[k] = chain;
                    },
                    [&](const WalkFile& w) {
                        CFile c;
                        LoadCFile(c, w);
                        std::lock_guard lk(lm);
                        files.emplace(w.key, std::move(c));
                    });
                for (auto& [k, chain] : dirs) f->dirs[k] = std::move(chain);
                for (auto& [k, c] : files) {
                    dropFile(k);
                    delta += static_cast<int64_t>(c.Bytes());
                    f->files.emplace(k, std::move(c));
                }
                continue;
            }
            dropFile(key);
            keep = keep && !Ignored(parent->second, key, nameKey, false) && !(at & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
            const std::wstring ext = ExtOf(nameKey);
            const uint64_t size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
            const bool doc = DocExts().count(ext) != 0;
            if (!keep || (doc && size > kDocMax)) continue;
            CFile c;
            LoadCFile(c, {path, key, nameKey, size, doc, BinaryExts().count(ext) != 0});
            delta += static_cast<int64_t>(c.Bytes());
            f->files.emplace(key, std::move(c));
        }
        fl.unlock();
        if (SUCCEEDED(co)) CoUninitialize();
    }).join();
    std::lock_guard lk(g_cacheM);
    f->bytes = static_cast<uint64_t>(std::max<int64_t>(0, static_cast<int64_t>(f->bytes) + delta));
    if (stale || f->stale) {
        CacheDrop(f);
        return false;
    }
    return true;
}

// A search answered from memory, when the folder is kept and the search follows the default rules.
std::optional<GrepResult> CachedGrep(const GrepQuery& q, const Matcher& m, const BytePre& pre) {
    if (q.hidden || q.ignored || !g_watching) return std::nullopt;
    const std::wstring key = Key(q.root);
    std::shared_ptr<KeptFolder> f;
    {
        std::unique_lock lk(g_cacheM);
        auto pick = [&]() -> std::shared_ptr<KeptFolder> {
            for (const auto& x : g_folders)
                if (key == x->rootKey || IsUnder(key, x->rootKey)) return x;
            return nullptr;
        };
        f = pick();
        if (!f) return std::nullopt;
        if (!f->ready) {  // being read in: that ends sooner than a walk of its own would
            g_cacheCv.wait_for(lk, std::chrono::seconds(10), [&] {
                f = pick();
                return !f || f->ready;
            });
            if (!f || !f->ready) return std::nullopt;
        }
        f->used = GetTickCount64();
    }
    if (!CacheApply(f)) return std::nullopt;

    GrepResult res;
    res.fromMemory = true;
    std::shared_lock sl(f->m);
    if (key != f->rootKey && !f->dirs.count(key)) return std::nullopt;  // a file, or left out there by the rules: walk it
    const bool all = key == f->rootKey, globs = !q.include.empty() || !q.exclude.empty();
    const size_t relAt = key.size() + (key.back() == L'\\' ? 0 : 1);
    std::vector<const std::pair<const std::wstring, CFile>*> picked;
    picked.reserve(f->files.size());
    uint64_t total = 0;
    for (const auto& e : f->files) {
        if (!all && !IsUnder(e.first, key)) continue;
        if (globs && !GlobsPass(q, e.second.nameKey, e.first.substr(std::min(e.first.size(), relAt)))) continue;
        picked.push_back(&e);
        total += e.second.raw.n;
    }
    std::mutex fm;
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> searched{0}, docs{0}, big{0}, binaries{0}, unreadable{0};
    auto work = [&] {
        std::wstring text;
        for (size_t i; (i = next++) < picked.size();) {
            const CFile& c = picked[i]->second;
            switch (c.kind) {
                case kcBinary:
                case kcBinaryExt: ++binaries; continue;
                case kcBig:
                case kcFail: {  // passed over: the answer says so
                    if (c.kind == kcBig) ++big;
                    else ++unreadable;
                    std::lock_guard lk(fm);
                    if (c.kind == kcBig && res.bigFiles.size() < kNamedSkips) res.bigFiles.push_back({c.path, c.size});
                    if (c.kind == kcFail && res.unreadableFiles.size() < kNamedSkips) res.unreadableFiles.push_back(c.path);
                    continue;
                }
                case kcNoFilter:
                    if (q.documents) {
                        std::lock_guard lk(fm);
                        ++res.noFilter[ExtOf(c.nameKey)];
                    }
                    continue;
                default: break;
            }
            if (c.kind == kcDoc && !q.documents) continue;
            FileHits fh;
            fh.path = c.path;
            fh.doc = c.kind == kcDoc;
            ++searched;
            if (fh.doc) {
                ++docs;
                FindInText(m, c.doc, fh, q.perFile, q.context);
            } else {
                if (DecodeBytes(c.raw.p.get(), c.raw.n, text, &pre) == ReadOutcome::NoMatch) continue;
                FindInText(m, text, fh, q.perFile, q.context);
            }
            if (fh.matches) {
                std::lock_guard lk(fm);
                res.files.push_back(std::move(fh));
            }
        }
    };
    if (total < (4u << 20)) {
        work();  // small: not worth starting threads
    } else {
        std::vector<std::thread> threads;
        const unsigned nth = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));
        for (unsigned i = 0; i < nth; ++i) threads.emplace_back(work);
        for (auto& t : threads) t.join();
    }
    sl.unlock();
    std::sort(res.files.begin(), res.files.end(), [](const FileHits& a, const FileHits& b) { return WalkBefore(a.path, b.path); });
    res.searched = searched;
    res.docs = docs;
    res.big = big;
    res.binaries = binaries;
    res.unreadable = unreadable;
    return res;
}
