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
std::unordered_map<std::wstring, Act> g_act;  // by key
std::vector<AddedItem> g_added;               // created after the indexes were taken
std::unordered_set<std::wstring> g_removed;   // deleted since (keys)
int64_t g_actSince = 0;                       // since when changes have been watched
std::atomic<bool> g_watching{false};
constexpr size_t kActMax = 120000, kActKeep = 80000;

void ActPrune() {  // with g_actM held
    if (g_act.size() <= kActMax) return;
    std::vector<int64_t> times;
    times.reserve(g_act.size());
    for (const auto& [k, a] : g_act) times.push_back(a.time);
    std::nth_element(times.begin(), times.begin() + (times.size() - kActKeep), times.end());
    const int64_t cut = times[times.size() - kActKeep];
    for (auto it = g_act.begin(); it != g_act.end();) it = it->second.time < cut ? g_act.erase(it) : std::next(it);
}

void ActRecord(const std::wstring& path, uint8_t kind, int64_t when = 0) {
    const std::wstring key = Key(path);
    if (Private(key) || IsUnder(key, Key(DataDir()))) return;
    bool dir = false;
    if (kind == kCreated || kind == kRenamed) {
        const DWORD a = GetFileAttributesW(path.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) return;  // already gone
        dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    std::lock_guard lk(g_actM);
    g_act[key] = {path, when ? when : NowFt(), kind};
    if (kind == kDeleted) g_removed.insert(key);
    else g_removed.erase(key);
    if ((kind == kCreated || kind == kRenamed) && g_added.size() < 50000) g_added.push_back({path, dir, key});
    ActPrune();
}

void CacheChanged(const std::wstring& path);  // cache.inl
void CacheLost(wchar_t letter);

void ActWatch(wchar_t letter) {
    const std::wstring root = std::wstring(1, letter) + L":\\";
    HANDLE d = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (d == INVALID_HANDLE_VALUE) return;
    std::vector<DWORD> buf(64 * 1024);  // 256 KB, DWORD-aligned as the API wants
    DWORD got = 0;
    while (ReadDirectoryChangesW(d, buf.data(), static_cast<DWORD>(buf.size() * sizeof(DWORD)), TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                     FILE_NOTIFY_CHANGE_SIZE,
                                 &got, nullptr, nullptr)) {
        if (!got) {  // too much at once: that batch is lost
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
}

void ActStartWatching() {
    if (g_watching.exchange(true)) return;
    for (wchar_t c : FixedDrives()) std::thread(ActWatch, c).detach();
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
    for (const Act& a : all) out += std::to_string(a.time) + "\t" + std::to_string(a.kind) + "\t" + Utf8(a.path) + "\n";
    const std::wstring f = ActFile(), tmp = f + L".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << out;
    }
    MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Reads what was seen before. Created and deleted items newer than the oldest index count as added or removed.
void ActLoad(int64_t indexStamp) {
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
        if (line.empty() || line[0] == '#') continue;
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        Act x{Wide(line.substr(b + 1)), _strtoi64(line.c_str(), nullptr, 10), static_cast<uint8_t>(atoi(line.c_str() + a + 1))};
        if (x.time >= old) loaded.push_back(std::move(x));
    }
    std::vector<AddedItem> added;
    for (const Act& x : loaded)
        if ((x.kind == kCreated || x.kind == kRenamed) && x.time > indexStamp && added.size() < 20000) {
            const DWORD at = GetFileAttributesW(x.path.c_str());
            if (at != INVALID_FILE_ATTRIBUTES) added.push_back({x.path, (at & FILE_ATTRIBUTE_DIRECTORY) != 0, Key(x.path)});
        }
    std::lock_guard lk(g_actM);
    for (Act& x : loaded) {
        const std::wstring k = Key(x.path);
        if (Private(k)) continue;
        auto it = g_act.find(k);
        if (it == g_act.end() || it->second.time < x.time) {
            if (x.kind == kDeleted && x.time > indexStamp) g_removed.insert(k);
            g_act[k] = std::move(x);
        }
    }
    g_added.insert(g_added.end(), added.begin(), added.end());
    g_actSince = since ? since : NowFt();
}

// When something was last created, changed or renamed here (0 if not seen, or deleted).
int64_t LastActivity(const std::wstring& key) {
    std::lock_guard lk(g_actM);
    auto it = g_act.find(key);
    return it != g_act.end() && it->second.kind != kDeleted ? it->second.time : 0;
}

bool Removed(const std::wstring& key) {
    std::lock_guard lk(g_actM);
    return g_removed.count(key) != 0;
}

// ---- what Jolt knows: usage.txt (heat, last use, folder) and recents.txt (opened last, first)

struct Use {
    std::wstring path, key;
    double heat = 0;
    int64_t last = 0;  // file time
};
std::mutex g_useM;
std::vector<Use> g_use;
std::vector<std::wstring> g_joltRecent;
int64_t g_useStamp = -1, g_recentStamp = -1;

void LoadUsage() {
    if (!g_rules.jolt) return;
    const std::wstring uf = RoamingAppData() + L"\\Jolt\\usage.txt", rf = RoamingAppData() + L"\\Jolt\\recents.txt";
    const int64_t ut = MTimeOf(uf), rt = MTimeOf(rf);
    std::lock_guard lk(g_useM);
    if (ut != g_useStamp) {
        g_useStamp = ut;
        g_use.clear();
        std::ifstream in(uf, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos) continue;
            Use u;
            u.heat = atof(line.c_str());
            u.last = (_strtoi64(line.c_str() + a + 1, nullptr, 10) + 11644473600LL) * kSecond;
            u.path = Normalize(Wide(line.substr(b + 1)));
            u.key = Key(u.path);
            if (!Private(u.key)) g_use.push_back(std::move(u));
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
        if (u.heat <= best || u.key.size() <= 3 || !IsUnder(key, u.key)) continue;
        if (std::count(key.begin() + std::min(key.size(), u.key.size()), key.end(), L'\\') > 2) continue;
        best = u.heat;
        if (folder) *folder = u.path;
    }
    return best;
}

int JoltRecentRank(const std::wstring& key) {  // 0 = opened last in Jolt; -1 = not among the recents
    std::lock_guard lk(g_useM);
    for (size_t i = 0; i < g_joltRecent.size() && i < 100; ++i)
        if (Key(g_joltRecent[i]) == key) return static_cast<int>(i);
    return -1;
}
