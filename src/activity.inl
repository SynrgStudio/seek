// What the user is working on. Every fixed drive is watched while the server runs: what is created, changed, renamed
// or deleted, with the time, kept between runs in activity.txt. Plus what Jolt knows: the folders used most and the
// ones opened last. Private places are never recorded.

enum : uint8_t { kCreated = 1, kModified = 2, kDeleted = 3, kRenamed = 4 };
const wchar_t* ActName(uint8_t k) {
    switch (k) {
        case kCreated: return L"created";
        case kModified: return L"changed";
        case kDeleted: return L"deleted";
        case kRenamed: return L"renamed to this";
        default: return L"touched";
    }
}

struct Act {
    std::wstring path;
    int64_t time = 0;
    uint8_t kind = 0;
};

std::mutex g_actM;
std::unordered_map<std::wstring, Act> g_act;           // by key
std::unordered_map<std::wstring, AddedItem> g_added;   // created after the indexes were taken, by key
std::unordered_map<std::wstring, int64_t> g_removed;   // deleted since: key, and when
int64_t g_actSince = 0;                                // since when changes have been watched
std::atomic<bool> g_watching{false};
constexpr size_t kActMax = 120000, kActKeep = 80000;
constexpr size_t kSinceMax = 100000, kSinceKeep = 80000;  // created or deleted since the index: over that, the oldest go

// Over max entries, the oldest go and keep stay. With g_actM held.
template <class Map, class TimeOf>
void PruneOldest(Map& m, size_t max, size_t keep, TimeOf timeOf) {
    if (m.size() <= max) return;
    std::vector<int64_t> times;
    times.reserve(m.size());
    for (const auto& e : m) times.push_back(timeOf(e.second));
    std::nth_element(times.begin(), times.begin() + (times.size() - keep), times.end());
    const int64_t cut = times[times.size() - keep];
    for (auto it = m.begin(); it != m.end();) it = timeOf(it->second) < cut ? m.erase(it) : std::next(it);
}

const std::unordered_set<std::wstring>& JunkDirs();  // grep.inl

// A folder renamed or moved here: the watchers see the folder, and nothing of what is inside. That is walked, with a
// budget, and counts as made since the index. Caches and private places are left out, as they rank last anyway.
void AddedWalk(const std::wstring& root) {
    std::vector<AddedItem> got;
    const int64_t t = NowFt();
    WalkTree(root, GetTickCount64() + 3000, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
        const std::wstring k = Key(p);
        if (Private(k)) return false;
        const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        got.push_back({p, dir, k, t});
        return dir && got.size() < 50000 && !JunkDirs().count(Lower(fd.cFileName));
    });
    std::lock_guard lk(g_actM);
    for (AddedItem& a : got) {
        g_removed.erase(a.key);
        g_added[a.key] = std::move(a);
    }
    PruneOldest(g_added, kSinceMax, kSinceKeep, [](const AddedItem& a) { return a.time; });
}

// Records a change. Returns whether it made a name known as made since the index that wasn't known before.
bool ActRecord(const std::wstring& path, uint8_t kind, int64_t when = 0) {
    const std::wstring key = Key(path);
    if (Private(key) || IsUnder(key, Key(DataDir()))) return false;
    bool dir = false;
    if (kind == kCreated || kind == kRenamed) {
        const DWORD a = GetFileAttributesW(path.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) return false;  // already gone
        dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    const int64_t t = when ? when : NowFt();
    bool fresh = false;
    {
        std::lock_guard lk(g_actM);
        if (kind == kDeleted) {
            // Made since the index and gone again: nothing to remember of it. Unless it was a folder: what was made inside
            // it since the index is still in g_added, and Removed() hides that through the folder.
            const auto it = g_added.find(key);
            const bool sinceIndex = it != g_added.end(), folder = sinceIndex && it->second.dir;
            if (sinceIndex) {
                g_added.erase(it);
                g_act.erase(key);
            } else {
                g_act[key] = {path, t, kind};
            }
            if (!sinceIndex || folder) {
                g_removed[key] = t;
                PruneOldest(g_removed, kSinceMax, kSinceKeep, [](int64_t x) { return x; });
            }
        } else {
            g_act[key] = {path, t, kind};
            g_removed.erase(key);
            if (kind == kCreated || kind == kRenamed) {
                fresh = !g_added.count(key);
                g_added[key] = {path, dir, key, t};
                PruneOldest(g_added, kSinceMax, kSinceKeep, [](const AddedItem& a) { return a.time; });
            }
        }
        PruneOldest(g_act, kActMax, kActKeep, [](const Act& a) { return a.time; });
    }
    if (kind == kRenamed && dir) std::thread(AddedWalk, path).detach();  // what is inside came along, unseen
    return fresh;
}

// A drive has a newer index: it holds what was created on the drive before it was taken, and no longer what was
// deleted, so those entries go. With a margin, since an index takes a while to make and what changed meanwhile may
// or may not be in it: a name kept here that the index also has shows once, one dropped that it lacks would be missing.
void ActIndexRenewed(wchar_t letter, int64_t stamp) {
    const int64_t cut = stamp - 15 * kMinute;
    const wchar_t l = static_cast<wchar_t>(towlower(letter));
    std::lock_guard lk(g_actM);
    for (auto it = g_added.begin(); it != g_added.end();) it = it->first[0] == l && it->second.time < cut ? g_added.erase(it) : std::next(it);
    for (auto it = g_removed.begin(); it != g_removed.end();) it = it->first[0] == l && it->second < cut ? g_removed.erase(it) : std::next(it);
}

void CacheChanged(const std::wstring& path);  // cache.inl
void CacheLost(wchar_t letter);

std::atomic<uint32_t> g_watchAlive{0};  // a bit per drive with a watcher thread
std::atomic<uint32_t> g_lostBatches[26];  // per drive: how often more changed at once than the buffer holds

void ActWatch(wchar_t letter) {
    const uint32_t bit = 1u << (towupper(letter) - L'A');
    if (g_watchAlive.fetch_or(bit) & bit) return;  // one is there already
    const std::wstring root = std::wstring(1, letter) + L":\\";
    HANDLE d = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (d == INVALID_HANDLE_VALUE) {  // locked, or gone: ActWatchMissing tries again later
        g_watchAlive &= ~bit;
        return;
    }
    std::vector<DWORD> buf(64 * 1024);  // 256 KB, DWORD-aligned as the API wants
    DWORD got = 0;
    while (ReadDirectoryChangesW(d, buf.data(), static_cast<DWORD>(buf.size() * sizeof(DWORD)), TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                     FILE_NOTIFY_CHANGE_SIZE,
                                 &got, nullptr, nullptr)) {
        if (!got) {  // too much at once: that batch is lost
            ++g_lostBatches[towupper(letter) - L'A'];
            CacheLost(letter);
            continue;
        }
        for (BYTE* p = reinterpret_cast<BYTE*>(buf.data());;) {
            const auto* f = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
            const std::wstring path = root + std::wstring(f->FileName, f->FileNameLength / sizeof(wchar_t));
            CacheChanged(path);
            switch (f->Action) {
                case FILE_ACTION_ADDED: ActRecord(path, kCreated); break;
                case FILE_ACTION_REMOVED: ActRecord(path, kDeleted); break;
                case FILE_ACTION_MODIFIED: ActRecord(path, kModified); break;
                case FILE_ACTION_RENAMED_OLD_NAME: ActRecord(path, kDeleted); break;
                case FILE_ACTION_RENAMED_NEW_NAME: ActRecord(path, kRenamed); break;
                default: break;
            }
            if (!f->NextEntryOffset) break;
            p += f->NextEntryOffset;
        }
    }
    CloseHandle(d);
    g_watchAlive &= ~bit;
    CacheLost(letter);  // from here on, what is kept of this drive isn't kept current
}

// A watcher for each fixed drive without one: at the start, and now and then after, since a drive can be locked
// (BitLocker) when the process starts, or appear later.
void ActWatchMissing() {
    for (wchar_t c : FixedDrives())
        if (!(g_watchAlive & (1u << (c - L'A')))) std::thread(ActWatch, c).detach();
}

void ActStartWatching() {
    if (g_watching.exchange(true)) return;
    ActWatchMissing();
}

// ---- catching up: what was made while no background process watched (an update, a restart, before the first agent
// called after logging in). NTFS keeps a journal of the changes on each drive, which a process without special rights
// can read from a handle to the drive's root: without names, but with the file's id, which gives its path now.

#ifndef FSCTL_READ_UNPRIVILEGED_USN_JOURNAL
#define FSCTL_READ_UNPRIVILEGED_USN_JOURNAL CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 234, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

std::atomic<uint64_t> g_caughtUp[26];   // per drive: names found made while Seek wasn't watching
std::atomic<int64_t> g_journalFrom[26];  // the oldest change read in its journal
std::atomic<bool> g_journalWhole[26];    // read from its start, with no mark of where the last run left it
std::atomic<bool> g_catchingUp{false};

// Where a drive's journal was: what came before was seen by the watchers. Saved with what they saw, so that the next
// start reads only what came after (the time no background process watched), not all of the journal.
struct JournalMark {
    DWORDLONG id = 0;
    USN next = 0;
};
std::array<JournalMark, 26> g_journalMark;  // from activity.txt

HANDLE OpenDriveRoot(wchar_t letter) {
    const std::wstring root = std::wstring(1, letter) + L":\\";
    return CreateFileW(root.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

bool JournalNow(wchar_t letter, JournalMark& out) {
    HANDLE vol = OpenDriveRoot(letter);
    if (vol == INVALID_HANDLE_VALUE) return false;
    USN_JOURNAL_DATA_V0 jd{};
    DWORD got = 0;
    const bool ok = DeviceIoControl(vol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof(jd), &got, nullptr) != 0;
    CloseHandle(vol);
    if (ok) out = {jd.UsnJournalID, jd.NextUsn};
    return ok;
}

// Reads the drive's journal for what was made or renamed since `since` and is still there, and records it as made
// since the index: from the mark the last run saved when it is still in the journal, else from the journal's start.
// Within a time budget.
void CatchUp(wchar_t letter, int64_t since) {
    HANDLE vol = OpenDriveRoot(letter);
    if (vol == INVALID_HANDLE_VALUE) return;
    USN_JOURNAL_DATA_V0 jd{};
    DWORD got = 0;
    if (!DeviceIoControl(vol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof(jd), &got, nullptr)) {  // not NTFS, or no journal
        CloseHandle(vol);
        return;
    }
    const JournalMark mark = g_journalMark[letter - L'A'];
    const bool fromMark = mark.id == jd.UsnJournalID && mark.next >= jd.FirstUsn && mark.next <= jd.NextUsn;
    g_journalWhole[letter - L'A'] = !fromMark;
    struct State {
        bool there = false, renamed = false;
        int64_t time = 0;
    };
    std::unordered_map<DWORDLONG, State> files;  // by file id: its last state in the journal
    READ_USN_JOURNAL_DATA_V1 rd{};
    rd.StartUsn = fromMark ? mark.next : jd.FirstUsn;
    rd.ReasonMask = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_RENAME_NEW_NAME;
    rd.UsnJournalID = jd.UsnJournalID;
    rd.MinMajorVersion = rd.MaxMajorVersion = 2;
    std::vector<DWORDLONG> buf(8192);  // 64 KB, aligned for the records
    const auto* bytes = reinterpret_cast<const BYTE*>(buf.data());
    const uint64_t deadline = GetTickCount64() + 30000;
    int64_t oldest = 0;
    while (GetTickCount64() < deadline && files.size() < 300000) {
        if (!DeviceIoControl(vol, FSCTL_READ_UNPRIVILEGED_USN_JOURNAL, &rd, sizeof(rd), buf.data(), static_cast<DWORD>(buf.size() * sizeof(DWORDLONG)), &got,
                             nullptr) ||
            got <= sizeof(USN))
            break;
        for (DWORD off = sizeof(USN); off + sizeof(USN_RECORD_V2) <= got;) {
            const auto* r = reinterpret_cast<const USN_RECORD_V2*>(bytes + off);
            if (r->RecordLength == 0) break;
            if (!oldest) oldest = r->TimeStamp.QuadPart;
            if (r->MajorVersion == 2 && r->TimeStamp.QuadPart > since) {
                State& s = files[r->FileReferenceNumber];
                if (r->Reason & USN_REASON_FILE_DELETE) {
                    s.there = false;
                } else {
                    s.there = true;
                    s.renamed = s.renamed || (r->Reason & USN_REASON_RENAME_NEW_NAME);
                    s.time = r->TimeStamp.QuadPart;
                }
            }
            off += r->RecordLength;
        }
        const USN next = *reinterpret_cast<const USN*>(bytes);
        if (next == rd.StartUsn) break;
        rd.StartUsn = next;
    }
    g_journalFrom[letter - L'A'] = oldest;
    uint64_t found = 0;
    std::vector<std::wstring> renamedDirs;
    for (const auto& [id, s] : files) {
        if (!s.there || GetTickCount64() > deadline) continue;
        FILE_ID_DESCRIPTOR fid{sizeof(FILE_ID_DESCRIPTOR)};
        fid.Type = FileIdType;
        fid.FileId.QuadPart = static_cast<LONGLONG>(id);
        HANDLE h = OpenFileById(vol, &fid, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_BACKUP_SEMANTICS);
        if (h == INVALID_HANDLE_VALUE) continue;  // gone since, or its id was taken again
        wchar_t p[2048];
        const DWORD n = GetFinalPathNameByHandleW(h, p, 2048, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(h);
        if (!n || n >= 2048) continue;
        std::wstring path(p, n);
        if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
        if (path.size() < 3 || path[1] != L':') continue;
        found += ActRecord(path, kCreated, s.time);
        const DWORD at = s.renamed ? GetFileAttributesW(path.c_str()) : INVALID_FILE_ATTRIBUTES;
        if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) renamedDirs.push_back(path);  // what is inside came along without records of its own
    }
    CloseHandle(vol);
    for (const std::wstring& d : renamedDirs) AddedWalk(d);
    g_caughtUp[letter - L'A'] = found;
}

// ---- kept between runs

std::wstring ActFile() { return DataDir() + L"\\activity.txt"; }

void ActSave() {
    std::vector<Act> all;
    int64_t since;
    {
        std::lock_guard lk(g_actM);
        all.reserve(g_act.size());
        for (const auto& [k, a] : g_act) all.push_back(a);
        since = g_actSince;
    }
    std::sort(all.begin(), all.end(), [](const Act& a, const Act& b) { return a.time > b.time; });
    if (all.size() > 40000) all.resize(40000);
    std::string out = "# Seek: changes seen on the drives. time, kind, path. Rewritten automatically.\n#since\t" + std::to_string(since) + "\n";
    // Where each watched drive's journal is now: the next start reads it from here. Not while catching up, nor for a
    // drive without a watcher: what comes before the mark wouldn't have been seen.
    if (!g_catchingUp)
        for (const wchar_t c : FixedDrives()) {
            JournalMark m;
            if ((g_watchAlive & (1u << (c - L'A'))) && JournalNow(c, m))
                out += "#usn\t" + std::string(1, static_cast<char>(c)) + "\t" + std::to_string(m.id) + "\t" + std::to_string(m.next) + "\n";
        }
    for (const Act& a : all) out += std::to_string(a.time) + "\t" + std::to_string(a.kind) + "\t" + Utf8(a.path) + "\n";
    const std::wstring f = ActFile(), tmp = f + L".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << out;
    }
    MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Reads what was seen before. Created and deleted items newer than their drive's index count as added or removed.
void ActLoad(const std::array<int64_t, 26>& indexStamp) {
    std::ifstream in(ActFile(), std::ios::binary);
    std::string line;
    std::vector<Act> loaded;
    int64_t since = 0;
    const int64_t old = NowFt() - 30 * kDay;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#since\t", 0) == 0) {
            since = _strtoi64(line.c_str() + 7, nullptr, 10);
            continue;
        }
        if (line.rfind("#usn\t", 0) == 0 && line.size() > 7 && line[5] >= 'A' && line[5] <= 'Z' && line[6] == '\t') {  // "#usn\tC\t<journal id>\t<next usn>"
            char* end = nullptr;
            const DWORDLONG id = _strtoui64(line.c_str() + 7, &end, 10);
            if (end && *end == '\t') g_journalMark[static_cast<size_t>(line[5] - 'A')] = {id, _strtoi64(end + 1, nullptr, 10)};
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        Act x{Wide(line.substr(b + 1)), _strtoi64(line.c_str(), nullptr, 10), static_cast<uint8_t>(atoi(line.c_str() + a + 1))};
        if (x.time >= old) loaded.push_back(std::move(x));
    }
    auto stampOf = [&](const std::wstring& key) {
        const int i = key.empty() ? -1 : static_cast<int>(towupper(key[0])) - L'A';
        return i >= 0 && i < 26 ? indexStamp[static_cast<size_t>(i)] : 0;
    };
    std::vector<AddedItem> added;  // the newest first, as saved: the ones still there
    for (const Act& x : loaded) {
        if (x.kind != kCreated && x.kind != kRenamed) continue;
        const std::wstring k = Key(x.path);
        if (Private(k) || x.time <= stampOf(k) || added.size() >= 20000) continue;
        const DWORD at = GetFileAttributesW(x.path.c_str());
        if (at != INVALID_FILE_ATTRIBUTES) added.push_back({x.path, (at & FILE_ATTRIBUTE_DIRECTORY) != 0, k, x.time});
    }
    std::lock_guard lk(g_actM);
    for (Act& x : loaded) {
        const std::wstring k = Key(x.path);
        if (Private(k)) continue;
        auto it = g_act.find(k);
        if (it == g_act.end() || it->second.time < x.time) {
            if (x.kind == kDeleted && x.time > stampOf(k)) g_removed[k] = x.time;
            g_act[k] = std::move(x);
        }
    }
    for (AddedItem& a : added)
        if (!g_removed.count(a.key)) g_added.emplace(a.key, std::move(a));
    g_actSince = since ? since : NowFt();
}

// When something was last created, changed or renamed here (0 if not seen, or deleted).
int64_t LastActivity(const std::wstring& key) {
    std::lock_guard lk(g_actM);
    auto it = g_act.find(key);
    return it != g_act.end() && it->second.kind != kDeleted ? it->second.time : 0;
}

// Gone since the index was taken: the path itself, or a folder above it (the index still lists what a deleted or
// renamed folder held; the watchers only saw the folder go).
bool Removed(const std::wstring& key) {
    std::lock_guard lk(g_actM);
    if (g_removed.empty()) return false;
    if (g_removed.count(key)) return true;
    for (size_t at = key.rfind(L'\\'); at != std::wstring::npos && at > 2; at = key.rfind(L'\\', at - 1))
        if (g_removed.count(key.substr(0, at))) return true;
    return false;
}

// ---- what Jolt knows: usage.txt (heat, last use, path, and "file" for a file: a folder otherwise), picks.txt (what
// was chosen in Ctrl+P for what was typed) and recents.txt (opened last, first)

struct Use {
    std::wstring path, key;
    double heat = 0;
    int64_t last = 0;  // file time
    bool file = false;  // a file opened from Jolt, not a folder
};
std::mutex g_useM;
std::vector<Use> g_use;
std::unordered_map<std::wstring, double> g_useFiles;  // the files' heat, by key
std::unordered_map<std::wstring, std::unordered_map<std::wstring, double>> g_picks;  // typed (lowercase) -> key -> times
std::vector<std::wstring> g_joltRecent;
int64_t g_useStamp = -1, g_recentStamp = -1, g_picksStamp = -1;

// Jolt's settings and what it learns: %APPDATA%\Jolt, or SEEK_JOLT_HOME (the tests).
std::wstring JoltDir() {
    const std::wstring v = Expand(L"%SEEK_JOLT_HOME%");
    return v.empty() || v.find(L'%') != std::wstring::npos ? RoamingAppData() + L"\\Jolt" : Normalize(v);
}

// The tab-separated fields of a line.
std::vector<std::string> Fields(const std::string& line) {
    std::vector<std::string> f;
    for (size_t at = 0;;) {
        const size_t tab = line.find('\t', at);
        f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
        if (tab == std::string::npos) return f;
        at = tab + 1;
    }
}

int64_t UnixToFt(const std::string& s) { return (_strtoi64(s.c_str(), nullptr, 10) + 11644473600LL) * kSecond; }

void LoadUsage() {
    if (!g_rules.jolt) return;
    const std::wstring dir = JoltDir(), uf = dir + L"\\usage.txt", rf = dir + L"\\recents.txt", pf = dir + L"\\picks.txt";
    const int64_t ut = MTimeOf(uf), rt = MTimeOf(rf), pt = MTimeOf(pf);
    std::lock_guard lk(g_useM);
    if (ut != g_useStamp) {
        g_useStamp = ut;
        g_use.clear();
        g_useFiles.clear();
        std::ifstream in(uf, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const std::vector<std::string> f = Fields(line);  // heat, last use, path[, "file"]
            if (f.size() < 3 || f[2].empty()) continue;
            Use u;
            u.heat = atof(f[0].c_str());
            u.last = UnixToFt(f[1]);
            u.path = Normalize(Wide(f[2]));
            u.key = Key(u.path);
            u.file = f.size() > 3 && f[3] == "file";
            if (Private(u.key)) continue;
            if (u.file) g_useFiles[u.key] = std::max(g_useFiles[u.key], u.heat);
            g_use.push_back(std::move(u));
        }
    }
    if (pt != g_picksStamp) {
        g_picksStamp = pt;
        g_picks.clear();
        std::ifstream in(pf, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const std::vector<std::string> f = Fields(line);  // times, last, typed, path
            if (f.size() < 4 || f[2].empty() || f[3].empty()) continue;
            const std::wstring key = Key(Normalize(Wide(f[3])));
            if (Private(key)) continue;
            double& times = g_picks[Lower(Trim(Wide(f[2])))][key];
            times = std::max(times, atof(f[0].c_str()));
        }
    }
    if (rt != g_recentStamp) {
        g_recentStamp = rt;
        g_joltRecent.clear();
        std::ifstream in(rf, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const std::wstring p = Normalize(Wide(line));
            if (!Private(Key(p))) g_joltRecent.push_back(p);
        }
    }
}

// The heat of the most used Jolt folder that holds this path closely (it, or up to two levels inside), and that
// folder. Only close: a drive or a whole profile is used a lot, and that says nothing about what is deep inside.
double JoltHeat(const std::wstring& key, std::wstring* folder = nullptr) {
    std::lock_guard lk(g_useM);
    double best = 0;
    for (const Use& u : g_use) {
        if (u.file || u.heat <= best || u.key.size() <= 3 || !IsUnder(key, u.key)) continue;
        if (std::count(key.begin() + std::min(key.size(), u.key.size()), key.end(), L'\\') > 2) continue;
        best = u.heat;
        if (folder) *folder = u.path;
    }
    return best;
}

// The heat of this file, opened from Jolt; 0 when it wasn't.
double JoltFileHeat(const std::wstring& key) {
    std::lock_guard lk(g_useM);
    const auto it = g_useFiles.find(key);
    return it == g_useFiles.end() ? 0 : it->second;
}

// How often this path was chosen in Jolt's Ctrl+P for this very text (lowercase); 0 when it wasn't.
double JoltPicked(const std::wstring& typed, const std::wstring& key) {
    std::lock_guard lk(g_useM);
    const auto t = g_picks.find(typed);
    if (t == g_picks.end()) return 0;
    const auto k = t->second.find(key);
    return k == t->second.end() ? 0 : k->second;
}

int JoltRecentRank(const std::wstring& key) {  // 0 = opened last in Jolt; -1 = not among the recents
    std::lock_guard lk(g_useM);
    for (size_t i = 0; i < g_joltRecent.size() && i < 100; ++i)
        if (Key(g_joltRecent[i]) == key) return static_cast<int>(i);
    return -1;
}
