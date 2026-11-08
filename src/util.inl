// Text, paths, time, the folders that stay private, and the log of what agents searched.

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::wstring Key(const std::wstring& p) { return Lower(p); }

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring Expand(const std::wstring& s) {
    wchar_t buf[2048];
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 2048);
    return n && n <= 2048 ? std::wstring(buf) : s;
}

// "c:/x/y/" -> "C:\x\y"; "C:" -> "C:\"
std::wstring Normalize(std::wstring p) {
    p = Trim(p);
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.size() >= 2 && p[1] == L':') p[0] = static_cast<wchar_t>(towupper(p[0]));
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    return p;
}

std::wstring ParentOf(const std::wstring& p) {
    if (p.size() <= 3) return L"";
    const size_t i = p.find_last_of(L'\\');
    if (i == std::wstring::npos) return L"";
    return i == 2 ? p.substr(0, 3) : p.substr(0, i);
}

std::wstring LeafOf(const std::wstring& p) {
    const size_t i = p.find_last_of(L'\\');
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring Join(const std::wstring& d, const std::wstring& n) {
    if (d.empty()) return n;
    return d.back() == L'\\' ? d + n : d + L"\\" + n;
}

// Is key k the root's, or inside it? (both lowercase)
bool IsUnder(const std::wstring& k, const std::wstring& root) {
    if (k == root) return true;
    const bool slash = !root.empty() && root.back() == L'\\';
    const size_t n = root.size() + (slash ? 0 : 1);  // root and its '\'
    return k.size() > n && k.compare(0, root.size(), root) == 0 && (slash || k[root.size()] == L'\\');
}

std::wstring ExtOf(const std::wstring& name) {
    const size_t i = name.find_last_of(L'.');
    if (i == std::wstring::npos || i == 0) return L"";
    return Lower(name.substr(i));
}

// The path below a folder, or the whole path when it isn't inside it.
std::wstring Relative(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || !IsUnder(Key(path), Key(root))) return path;
    if (path.size() == root.size()) return L".";
    return path.substr(root.size() + (root.back() == L'\\' ? 0 : 1));
}

// ---- time and numbers

constexpr int64_t kSecond = 10000000LL, kMinute = 60 * kSecond, kHour = 60 * kMinute, kDay = 24 * kHour;

int64_t NowFt() {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

int64_t Ft(const FILETIME& f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

std::wstring DateText(int64_t ft) {
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)}, local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&f, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t b[32];
    swprintf(b, 32, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

std::wstring Ago(int64_t ft) {
    const int64_t d = NowFt() - ft;
    if (d < kMinute) return L"just now";
    if (d < kHour) return std::to_wstring(d / kMinute) + L" min ago";
    if (d < kDay) return std::to_wstring(d / kHour) + L" h ago";
    if (d < 2 * kDay) return L"yesterday";
    if (d < 30 * kDay) return std::to_wstring(d / kDay) + L" days ago";
    return L"on " + DateText(ft).substr(0, 10);
}

std::wstring Thousands(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}

std::wstring SizeText(uint64_t b) {
    const wchar_t* unit[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        ++u;
    }
    wchar_t buf[32];
    if (u == 0) swprintf(buf, 32, L"%llu B", static_cast<unsigned long long>(b));
    else swprintf(buf, 32, v < 10 ? L"%.1f %s" : L"%.0f %s", v, unit[u]);
    return buf;
}

// "30m", "2h", "3d", "2w" -> a span of file time; def when it can't be read
int64_t ParseSpan(const std::wstring& s, int64_t def) {
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    const double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || v <= 0) return def;
    switch (towlower(*end)) {
        case L'm': return static_cast<int64_t>(v * kMinute);
        case L'h': return static_cast<int64_t>(v * kHour);
        case L'w': return static_cast<int64_t>(v * 7 * kDay);
        case L'd': case 0: return static_cast<int64_t>(v * kDay);
        default: return def;
    }
}

// ---- where things live

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    wchar_t* p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

const std::wstring& LocalAppData() {
    static const std::wstring p = KnownFolder(FOLDERID_LocalAppData);
    return p;
}
const std::wstring& RoamingAppData() {
    static const std::wstring p = KnownFolder(FOLDERID_RoamingAppData);
    return p;
}

// Where Seek keeps its files: %LOCALAPPDATA%\Seek, with its copy of the index, what it saw change, and the log. The
// variable SEEK_HOME names another folder, and that gives the instance a background process of its own (the tests
// use it, so they never touch the one the user's agents talk to).
const std::wstring& SeekHome() {
    static const std::wstring h = [] {
        const std::wstring v = Expand(L"%SEEK_HOME%");
        return v.empty() || v.find(L'%') != std::wstring::npos ? std::wstring() : Normalize(v);
    }();
    return h;
}

const std::wstring& DataDir() {
    static const std::wstring p = [] {
        const std::wstring d = SeekHome().empty() ? LocalAppData() + L"\\Seek" : SeekHome();
        SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
        return d;
    }();
    return p;
}

// The settings: %APPDATA%\Seek\seek.txt, or seek.txt in SEEK_HOME.
std::wstring SettingsFile() { return SeekHome().empty() ? RoamingAppData() + L"\\Seek\\seek.txt" : SeekHome() + L"\\seek.txt"; }

// Names the background process by: empty for the usual one, a hash of SEEK_HOME for another.
std::wstring InstanceTag() {
    if (SeekHome().empty()) return L"";
    uint32_t h = 2166136261u;
    for (const wchar_t c : Key(SeekHome())) h = (h ^ c) * 16777619u;
    wchar_t b[16];
    swprintf_s(b, L"-%08x", h);
    return b;
}

bool FileInfo(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& d) {
    std::wstring p = path.size() >= MAX_PATH ? L"\\\\?\\" + path : path;
    return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d) != 0;
}

int64_t MTimeOf(const std::wstring& f) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    return FileInfo(f, d) ? Ft(d.ftLastWriteTime) : 0;
}

// ---- private places: never listed, searched or remembered

struct Rules {
    std::vector<std::wstring> prefixes, names, exts;  // private
    std::vector<std::wstring> noisePrefixes, noiseNames, noiseExts;  // system churn, left out of "recent" unless asked
    std::wstring profileDot;  // "c:\users\me\.": the dot folders there are programs' data (.cargo, .vscode, .t3…)
    bool log = true;
    bool jolt = true;        // use Jolt's index and what Jolt knows about the folders used, when Jolt is installed
    uint64_t cacheMb = 512;  // folders kept in memory for grep, at most
} g_rules;

void LoadRulesNow();
void LoadRules() {  // once: other threads read them from then on
    static std::once_flag once;
    std::call_once(once, LoadRulesNow);
}

void LoadRulesNow() {
    Rules r;
    auto prefix = [](std::vector<std::wstring>& to, const std::wstring& p) {
        const std::wstring k = Key(Normalize(Expand(p)));
        if (k.size() > 3 && k.find(L'%') == std::wstring::npos) to.push_back(k);
    };
    for (const wchar_t* p : {
             L"%USERPROFILE%\\.ssh", L"%USERPROFILE%\\.gnupg", L"%USERPROFILE%\\.aws", L"%USERPROFILE%\\.azure", L"%USERPROFILE%\\.kube",
             L"%USERPROFILE%\\.docker", L"%USERPROFILE%\\.git-credentials", L"%APPDATA%\\Microsoft\\Credentials",
             L"%LOCALAPPDATA%\\Microsoft\\Credentials", L"%APPDATA%\\Microsoft\\Protect", L"%APPDATA%\\Microsoft\\Crypto",
             L"%APPDATA%\\Microsoft\\SystemCertificates", L"%LOCALAPPDATA%\\Google\\Chrome\\User Data", L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data",
             L"%LOCALAPPDATA%\\BraveSoftware", L"%LOCALAPPDATA%\\Thorium\\User Data", L"%LOCALAPPDATA%\\Vivaldi\\User Data",
             L"%LOCALAPPDATA%\\Chromium\\User Data", L"%APPDATA%\\Opera Software", L"%APPDATA%\\Mozilla\\Firefox\\Profiles", L"%APPDATA%\\Bitwarden",
             L"%LOCALAPPDATA%\\1Password", L"%APPDATA%\\KeePass", L"%APPDATA%\\KeePassXC", L"%APPDATA%\\Telegram Desktop\\tdata", L"%APPDATA%\\Signal",
         })
        prefix(r.prefixes, p);
    r.names = {L"$recycle.bin", L"system volume information"};
    r.exts = {L".kdbx", L".kdb", L".pfx", L".p12", L".ppk"};
    for (const wchar_t* p : {L"%LOCALAPPDATA%", L"%USERPROFILE%\\AppData\\LocalLow", L"%APPDATA%", L"%WINDIR%", L"%ProgramData%",
                             L"%ProgramFiles%", L"%ProgramFiles(x86)%", L"%TEMP%"})
        prefix(r.noisePrefixes, p);
    // caches, package stores and generated folders: everywhere, rarely what is meant
    r.noiseNames = {L".git", L"node_modules", L"__pycache__", L".vs", L".cache", L"$recycle.bin", L"system volume information", L".cargo",
                    L".rustup", L".npm", L".nuget", L".gradle", L"packagecache", L".godot", L"site-packages", L"bower_components", L".m2",
                    L".pnpm-store", L".yarn", L"logs", L"cache", L"caches", L".fingerprint"};
    // what programs and compilers write on their own all the time
    r.noiseExts = {L".log", L".log1", L".log2", L".blf", L".regtrans-ms", L".ndjson", L".tmp", L".temp", L".lock", L".lck", L".etl", L".dmp",
                   L".pf", L".sqlite", L".sqlite-wal", L".sqlite-shm", L".sqlite-journal", L".db", L".db-wal", L".db-shm", L".db-journal",
                   L".ldb", L".tlog", L".obj", L".o", L".pdb", L".ilk", L".idb", L".pch", L".ipch", L".rlib", L".rmeta", L".d",
                   L".lastbuildstate"};
    r.profileDot = Key(Normalize(Expand(L"%USERPROFILE%"))) + L"\\.";
    // seek.txt: "exclude = C:\folder", "exclude = *.ext", "exclude = name", "log = no", "jolt = no", "cache = 256"
    r.jolt = Expand(L"%SEEK_NO_JOLT%") != L"1";
    std::ifstream in(SettingsFile(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const std::wstring l = Trim(Wide(line));
        if (l.empty() || l[0] == L'#') continue;
        const size_t eq = l.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring k = Lower(Trim(l.substr(0, eq))), v = Trim(l.substr(eq + 1));
        if (k == L"log") r.log = Lower(v) != L"no";
        if (k == L"jolt") r.jolt = Lower(v) != L"no";
        if (k == L"cache") r.cacheMb = static_cast<uint64_t>(_wtoi64(v.c_str()));
        if (k != L"exclude" || v.empty()) continue;
        if (v.rfind(L"*.", 0) == 0) r.exts.push_back(Lower(v.substr(1)));
        else if (v.find_first_of(L"\\/:%") == std::wstring::npos) r.names.push_back(Lower(v));
        else prefix(r.prefixes, v);
    }
    g_rules = std::move(r);
}

bool AnySegment(const std::wstring& key, const std::vector<std::wstring>& names) {
    size_t at = 0;
    while (at < key.size()) {
        size_t end = key.find(L'\\', at);
        if (end == std::wstring::npos) end = key.size();
        const std::wstring_view seg(key.data() + at, end - at);
        for (const std::wstring& n : names)
            if (seg == n) return true;
        at = end + 1;
    }
    return false;
}

// key: the whole path, lowercase
bool Private(const std::wstring& key) {
    for (const std::wstring& p : g_rules.prefixes)
        if (IsUnder(key, p)) return true;
    if (AnySegment(key, g_rules.names)) return true;
    const std::wstring ext = ExtOf(key);
    if (!ext.empty())
        for (const std::wstring& e : g_rules.exts)
            if (ext == e) return true;
    return false;
}

// Private, for an entry in a folder that is known not to be: then only the entry's own path, name and extension can
// make it so. The same answer as Private(key) without walking the whole path, for walks that check every entry.
bool PrivateLeaf(const std::wstring& key, const std::wstring& nameKey) {
    for (const std::wstring& p : g_rules.prefixes)
        if (key == p) return true;
    for (const std::wstring& n : g_rules.names)
        if (nameKey == n) return true;
    const size_t dot = nameKey.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        const std::wstring_view ext(nameKey.data() + dot, nameKey.size() - dot);
        for (const std::wstring& e : g_rules.exts)
            if (ext == e) return true;
    }
    return false;
}

bool NoiseExt(const std::wstring& key) {
    const std::wstring ext = ExtOf(key);
    if (ext.empty()) return false;
    for (const std::wstring& e : g_rules.noiseExts)
        if (ext == e) return true;
    return false;
}

bool Noise(const std::wstring& key) {
    for (const std::wstring& p : g_rules.noisePrefixes)
        if (IsUnder(key, p)) return true;
    if (!g_rules.profileDot.empty() && key.rfind(g_rules.profileDot, 0) == 0) return true;
    return AnySegment(key, g_rules.noiseNames) || NoiseExt(key);
}

// Noise below a folder the agent asked about: when that folder is itself in a system place (a project in AppData,
// a test in temp), only the caches and logs inside it count as noise.
bool NoiseBelow(const std::wstring& key, const std::wstring& scopeKey) {
    if (scopeKey.empty() || !Noise(scopeKey)) return Noise(key);
    if (key.size() <= scopeKey.size()) return false;
    return AnySegment(key.substr(scopeKey.size()), g_rules.noiseNames) || NoiseExt(key);
}

// ---- the log: what each agent searched, for the user to see

std::mutex g_logM;
void Audit(const std::wstring& line) {
    if (!g_rules.log) return;
    std::lock_guard lk(g_logM);
    static HANDLE h = INVALID_HANDLE_VALUE;  // kept open: a call shouldn't wait for the file to open
    static uint64_t size = 0;
    const std::wstring f = DataDir() + L"\\log.txt";
    auto open = [&] {
        h = CreateFileW(f.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER s{};
        size = h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &s) ? static_cast<uint64_t>(s.QuadPart) : 0;
    };
    if (h == INVALID_HANDLE_VALUE) open();
    if (size > 2 * 1024 * 1024) {
        CloseHandle(h);
        MoveFileExW(f.c_str(), (DataDir() + L"\\log.old.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
        open();
    }
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string s = Utf8(DateText(NowFt()) + L"  " + line) + "\r\n";
    DWORD w = 0;
    WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &w, nullptr);
    size += w;
}
