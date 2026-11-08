// How it runs. "seek.exe" alone is what an agent starts: it speaks MCP (JSON-RPC, one message per line on stdin and
// stdout) and hands each tool call to "seek.exe --daemon", one process shared by every agent, which keeps the index,
// watches the drives and remembers. The first agent starts it; it keeps running. If it can't be reached, the call
// runs right here instead. The daemon only accepts its own user, on this machine.

const char* kInstructions =
    "Seek searches this Windows PC fast. find: files and folders by name anywhere (every drive, live index, typo-tolerant), "
    "instead of dir /s or Get-ChildItem -Recurse; ranks exact names, what the user changed lately and what is near a folder first. "
    "grep: text inside the files of a folder, also PDF and Office documents. recent: what the user changed or opened lately; "
    "use it when they mean 'the file I was working on'. Answers are short: use limit and offset to page, summary for counts.";

const char* kToolsJson = R"JSON([
{"name":"find","title":"Find files by name",
 "description":"Find files and folders by name anywhere on this PC (every fixed drive) in milliseconds, from a live index. Fuzzy and typo-tolerant. Exact names come first, then what the user changed lately, what is in folders they use, and what is inside `near`. Use it instead of dir /s, Get-ChildItem -Recurse or find. Query: words must appear in the name in order (fuzzy); .png or ext:png,jpg filter the extension; * and ? are wildcards on the name, {a,b} either; !word leaves out names with it; type:file or type:folder; a fragment with a slash, like src/report.pdf or docs/, must be in the path (what follows its last slash is the name to look for).",
 "inputSchema":{"type":"object","properties":{
  "query":{"type":"string","description":"What to look for, like \"report 2026\", \"jolt.exe\", \".blend\" or \"*.log !debug\"."},
  "path":{"type":"string","description":"Only below this folder. Omit to search every drive."},
  "near":{"type":"string","description":"A folder whose surroundings rank first, usually the current project."},
  "type":{"type":"string","enum":["any","file","folder"]},
  "limit":{"type":"integer","minimum":1,"maximum":200,"default":20},
  "offset":{"type":"integer","minimum":0,"default":0,"description":"Skip this many results (to page): the answer gives the offset of the next ones."},
  "summary":{"type":"boolean","default":false,"description":"How many and where (counts by folder, extension and drive) instead of a list."}},
  "required":["query"]},
 "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"grep","title":"Search text inside files",
 "description":"Search the text inside the files of a folder (or one file): code and text in any encoding, and PDF/Office documents when Windows has a text filter for them. Follows .gitignore (and .git/info/exclude, the user's global excludes and .ignore) and skips dependency and cache folders, binaries and files over 16 MB, unless ignored is true; a path inside an ignored folder is searched as asked, and a file given as the path is searched whatever its size. Literal by default and case-insensitive unless the pattern has capitals; regex for a pattern (a literal one that finds nothing but has regex signs, like a|b, is tried as a regex and the answer says so). Each matching line shows with its number, a long one cut around each match (with its column); context adds lines around, shared by matches close together. The header says how long the search took.",
 "inputSchema":{"type":"object","properties":{
  "pattern":{"type":"string","description":"The text to look for (or a regex with regex: true)."},
  "path":{"type":"string","description":"The folder or file to search in."},
  "regex":{"type":"boolean","default":false},
  "without":{"type":"string","description":"Lines that also have this are left out, like grep -v after the search: literal, or a regex when regex is true, with the pattern's case rule."},
  "case":{"type":"string","enum":["smart","sensitive","insensitive"],"default":"smart"},
  "glob":{"type":["string","array"],"items":{"type":"string"},"description":"Which files: \"*.cpp\", \"src/**/*.ts\", \"*.{ts,tsx}\"; a leading ! leaves them out: \"!*.min.js\"."},
  "context":{"type":"integer","minimum":0,"maximum":5,"default":1},
  "limit":{"type":"integer","minimum":1,"maximum":300,"default":30,"description":"Matching lines to show."},
  "offset":{"type":"integer","minimum":0,"default":0,"description":"Skip this many matching lines (to page)."},
  "per_file":{"type":"integer","minimum":1,"maximum":200,"default":5},
  "summary":{"type":"boolean","default":false,"description":"Match counts per file instead of the lines."},
  "documents":{"type":"boolean","default":true,"description":"Also read PDF and Office documents."},
  "hidden":{"type":"boolean","default":false},
  "ignored":{"type":"boolean","default":false,"description":"Also search what .gitignore leaves out and dependency and cache folders (node_modules, build outputs)."},
  "time_budget_ms":{"type":"integer","minimum":500,"maximum":60000,"default":6000}},
  "required":["pattern","path"]},
 "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"recent","title":"What the user worked on lately",
 "description":"Files the user created or changed lately and folders they opened in Jolt, newest first: what they are working on. Machine-wide from what Seek saw change while running (kept between runs); with path, also the dates on disk below that folder. System places, and what .gitignore leaves out, are left out unless include_system.",
 "inputSchema":{"type":"object","properties":{
  "path":{"type":"string","description":"Only below this folder; also reads the dates on disk there."},
  "query":{"type":"string","description":"Only names like this (same syntax as find), like \".docx\" or \"invoice\"."},
  "since":{"type":"string","default":"7d","description":"How far back: 30m, 2h, 3d, 2w."},
  "limit":{"type":"integer","minimum":1,"maximum":200,"default":20},
  "include_system":{"type":"boolean","default":false}}},
 "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}}
])JSON";

std::wstring PipeName() {
    wchar_t user[256] = L"";
    DWORD n = 256;
    GetUserNameW(user, &n);
    return std::wstring(L"\\\\.\\pipe\\seek-") + user + InstanceTag();
}

std::wstring MutexName() { return L"Local\\SeekDaemon" + InstanceTag(); }

bool WriteFrame(HANDLE h, const std::string& s) {
    const uint32_t n = static_cast<uint32_t>(s.size());
    DWORD w;
    return WriteFile(h, &n, 4, &w, nullptr) && w == 4 && (n == 0 || (WriteFile(h, s.data(), n, &w, nullptr) && w == n));
}

bool ReadExact(HANDLE h, void* buf, DWORD n) {
    auto* p = static_cast<char*>(buf);
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, nullptr) || got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

bool ReadFrame(HANDLE h, std::string& s) {
    uint32_t n = 0;
    if (!ReadExact(h, &n, 4) || n > (64u << 20)) return false;
    s.resize(n);
    return n == 0 || ReadExact(h, s.data(), n);
}

// ---- the daemon

void LoadEverything(bool build) {
    LoadRules();  // once: the watchers read the rules from other threads
    LoadUsage();
    const std::vector<wchar_t> drives = FixedDrives();
    std::vector<wchar_t> missing;
    for (wchar_t c : drives)
        if (!LoadVolume(c, false)) missing.push_back(c);
    std::array<int64_t, 26> stamps{};  // when each drive's index was taken; 0 for a drive without one
    for (const auto& v : Volumes()) stamps[static_cast<size_t>(towupper(v->letter) - L'A')] = v->stamp;
    ActLoad(stamps);
    g_indexReady = true;  // drives without an index are searched as soon as theirs is built
    if (build)
        for (wchar_t c : missing) LoadVolume(c, true);
}

// "0.2.1" at the start of a build string ("0.2.1 Nov  8 2026 03:08:40"), as numbers.
bool ParseVersion(const std::string& s, int v[3]) { return sscanf_s(s.c_str(), "%d.%d.%d", &v[0], &v[1], &v[2]) == 3; }

void Serve(HANDLE pipe) {
    std::string req;
    while (ReadFrame(pipe, req)) {
        Json j;
        std::string reply;
        if (!JsonParse(req, j)) {
            reply = R"({"text":"Bad request","error":true})";
        } else if (const Json* tool = j.at("tool"); tool && tool->s == "__status") {  // seek --status, from any build
            reply = "{\"text\":" + JsonQuote(Utf8(StatusText())) + "}";
        } else {
            const Json* b = j.at("build");
            if (!b || b->s != kBuild) {
                // A program of another build is asking. A newer one, or another build of this version (development):
                // this process makes room for the one that asks. An older one, a session that was open when Seek was
                // updated, is served: what passes between them hasn't changed, and it gets this version's answers.
                int theirs[3], mine[3];
                const bool older = b && ParseVersion(b->s, theirs) && ParseVersion(SEEK_VERSION, mine) &&
                                   std::lexicographical_compare(theirs, theirs + 3, mine, mine + 3);
                if (!older) {
                    WriteFrame(pipe, R"({"restart":true})");
                    FlushFileBuffers(pipe);
                    ActSave();
                    ExitProcess(0);
                }
            }
            if (tool && tool->s == "__stop") {
                WriteFrame(pipe, R"({"text":"stopping"})");
                FlushFileBuffers(pipe);
                ActSave();
                ExitProcess(0);
            }
            static const Json empty = [] {
                Json e;
                e.kind = Json::Obj;
                return e;
            }();
            const Json* args = j.at("args");
            const Json* client = j.at("client");
            ToolOut out = RunTool(tool ? tool->s : "", args ? *args : empty, client ? Wide(client->s) : L"");
            reply = "{\"text\":" + JsonQuote(Utf8(out.text)) + ",\"error\":" + (out.error ? "true" : "false") + "}";
        }
        if (!WriteFrame(pipe, reply)) break;
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

// Before Seek had a repository of its own, its files lived in %LOCALAPPDATA%\Jolt\seek: what it saw change and the
// log move over, the copies of the index go.
void MoveOldData() {
    if (!SeekHome().empty()) return;
    const std::wstring old = LocalAppData() + L"\\Jolt\\seek";
    if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    for (const wchar_t* f : {L"\\activity.txt", L"\\log.txt"}) MoveFileExW((old + f).c_str(), (DataDir() + f).c_str(), 0);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((old + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((old + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(old.c_str());  // when something still holds a copy, the next start finishes it
}

int DaemonMain() {
    HANDLE mx = CreateMutexW(nullptr, TRUE, MutexName().c_str());  // held until it exits: --stop waits on it
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // one is running
    (void)mx;
    LoadRules();  // before the watchers: they must already know what is private
    MoveOldData();
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    // only this user, and SYSTEM, can connect
    std::wstring sddl = L"D:P(A;;GA;;;SY)";
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        BYTE buf[256];
        DWORD n = 0;
        if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &n)) {
            wchar_t* sid = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &sid)) {
                sddl += L"(A;;GA;;;" + std::wstring(sid) + L")";
                LocalFree(sid);
            }
        }
        CloseHandle(token);
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);
    std::thread([] { LoadEverything(true); }).detach();
    ActStartWatching();
    std::thread([] {  // saves what it saw, watches drives that couldn't be, picks up a newer index from Jolt, and walks its own indexes again daily
        for (int tick = 1;; ++tick) {
            Sleep(60 * 1000);
            CacheExpire();
            if (tick % 5 == 0) {
                ActSave();
                ActWatchMissing();
            }
            if (tick % 10 == 0)
                for (wchar_t c : FixedDrives()) LoadVolume(c, false);
            if (tick % 60 == 2)
                for (wchar_t c : FixedDrives()) RefreshOwnIndex(c);
        }
    }).detach();
    const std::wstring name = PipeName();
    for (;;) {
        HANDLE p = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, &sa);
        if (p == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }
        if (ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) std::thread(Serve, p).detach();
        else CloseHandle(p);
    }
}

// ---- reaching it

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

void StartDaemon() {
    std::wstring cmd = L"\"" + ExePath() + L"\" --daemon";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    // out of the agent's job when allowed: the daemon outlives the session that started it
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

HANDLE ConnectDaemon(int waitMs) {
    const std::wstring name = PipeName();
    for (int waited = 0, busy = 0;;) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        // every instance taken (another call connected a moment ago): the next one comes right away
        if (GetLastError() == ERROR_PIPE_BUSY && ++busy < 50 && WaitNamedPipeW(name.c_str(), 500)) continue;
        if (waited >= waitMs) return nullptr;
        Sleep(100);
        waited += 100;
    }
}

// Until the daemon's mutex is released (it held it until it exited).
void WaitDaemonGone() {
    if (HANDLE mx = OpenMutexW(SYNCHRONIZE, FALSE, MutexName().c_str())) {
        const DWORD r = WaitForSingleObject(mx, 5000);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(mx);
        CloseHandle(mx);
    }
}

std::atomic<bool> g_local{false};  // the daemon couldn't be reached: everything runs in this process

void LocalReady() {
    static std::once_flag once;
    std::call_once(once, [] { LoadEverything(false); });
}

// One tool call: through the daemon (starting it if needed), or here. On a connection of its own, since the calls of
// one session run at the same time, as the agent sends them.
ToolOut CallTool(const std::string& tool, const Json& args, const std::wstring& client) {
    std::string argText;
    JsonWrite(args, argText);
    const std::string req = "{\"build\":" + JsonQuote(kBuild) + ",\"tool\":" + JsonQuote(tool) + ",\"args\":" + argText + ",\"client\":" + JsonQuote(Utf8(client)) + "}";
    for (int attempt = 0; attempt < 3 && !g_local; ++attempt) {
        HANDLE pipe = ConnectDaemon(0);
        if (!pipe) {
            StartDaemon();
            pipe = ConnectDaemon(15000);
        }
        if (!pipe) break;
        std::string reply;
        const bool answered = WriteFrame(pipe, req) && ReadFrame(pipe, reply);
        CloseHandle(pipe);
        if (!answered) continue;
        Json j;
        if (!JsonParse(reply, j)) break;
        if (j.at("restart")) {  // it was an older one: it is closing, start the new one
            WaitDaemonGone();
            continue;
        }
        const Json* t = j.at("text");
        const Json* e = j.at("error");
        return {t ? Wide(t->s) : L"", e && e->b};
    }
    g_local = true;
    LocalReady();
    return RunTool(tool, args, client);
}

// ---- MCP over stdio

void Send(const std::string& msg) {
    static std::mutex m;
    std::lock_guard lk(m);
    const std::string line = msg + "\n";
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), static_cast<DWORD>(line.size()), &w, nullptr);
}

std::string IdText(const Json* id) {
    if (!id) return "null";
    std::string s;
    JsonWrite(*id, s);
    return s;
}

// The protocol versions Seek speaks: every one since the first, as nothing tools rely on changed between them. A
// client asking for one of those, or a later one, gets it back; anything else gets the latest Seek knows.
constexpr const char* kFirstProtocol = "2024-11-05";
constexpr const char* kLatestProtocol = "2025-11-25";

bool IsDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// One JSON-RPC message from the client: the reply to send, or nothing for a notification.
std::string Handle(const Json& msg, std::wstring& client) {
    const Json* id = msg.at("id");
    const Json* method = msg.at("method");
    const Json* params = msg.at("params");
    if (!method || method->kind != Json::Str) return "";  // a reply to us: nothing is asked of the client
    const std::string& m = method->s;
    if (!id || id->kind == Json::Null) return "";  // notifications (initialized, cancelled): nothing to answer
    const std::string head = "{\"jsonrpc\":\"2.0\",\"id\":" + IdText(id) + ",";
    auto error = [&](int code, const std::string& text) {
        return head + "\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + JsonQuote(text) + "}}";
    };
    if (m == "initialize") {
        std::string version = kLatestProtocol;
        if (params)
            if (const Json* v = params->at("protocolVersion"); v && v->kind == Json::Str && IsDate(v->s) && v->s >= kFirstProtocol) version = v->s;
        if (params)
            if (const Json* ci = params->at("clientInfo"))
                if (const Json* n = ci->at("name"); n && n->kind == Json::Str) client = Wide(n->s);
        return head + "\"result\":{\"protocolVersion\":" + JsonQuote(version) +
               ",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"seek\",\"title\":\"Seek\",\"version\":\"" SEEK_VERSION
               "\",\"websiteUrl\":\"https://seek.synrgs.tech\"},\"instructions\":" + JsonQuote(kInstructions) + "}}";
    }
    if (m == "tools/list") {
        std::string tools(kToolsJson);
        std::replace(tools.begin(), tools.end(), '\n', ' ');  // one message per line
        return head + "\"result\":{\"tools\":" + tools + "}}";
    }
    if (m == "tools/call") {
        const Json* name = params ? params->at("name") : nullptr;
        const Json* args = params ? params->at("arguments") : nullptr;
        if (!name || name->kind != Json::Str || (name->s != "find" && name->s != "grep" && name->s != "recent"))
            return error(-32602, "Unknown tool: " + (name && name->kind == Json::Str ? name->s : std::string("(none)")) + ". The tools are find, grep and recent.");
        if (args && args->kind != Json::Obj && args->kind != Json::Null) return error(-32602, "The arguments must be an object.");
        static const Json empty = [] {
            Json e;
            e.kind = Json::Obj;
            return e;
        }();
        const ToolOut out = CallTool(name->s, args && args->kind == Json::Obj ? *args : empty, client);
        return head + "\"result\":{\"content\":[{\"type\":\"text\",\"text\":" + JsonQuote(Utf8(out.text)) + "}],\"isError\":" +
               (out.error ? "true" : "false") + "}}";
    }
    if (m == "ping") return head + "\"result\":{}}";
    return error(-32601, "Method not found");
}

// Is there a tools/call in it? Those run on a thread of their own, so that the calls an agent sends at once run at once.
bool IsToolCall(const Json& msg) {
    if (msg.kind == Json::Arr) {
        for (const Json& one : msg.a)
            if (IsToolCall(one)) return true;
        return false;
    }
    const Json* m = msg.at("method");
    return m && m->kind == Json::Str && m->s == "tools/call";
}

// One message: a request's reply is sent, a batch's replies together.
void Answer(const Json& msg, std::wstring& client) {
    if (msg.kind == Json::Arr) {  // a batch, from clients of the 2025-03-26 protocol
        std::string replies;
        for (const Json& one : msg.a)
            if (std::string r = Handle(one, client); !r.empty()) replies += (replies.empty() ? "" : ",") + r;
        if (!replies.empty()) Send("[" + replies + "]");
        return;
    }
    if (std::string r = Handle(msg, client); !r.empty()) Send(r);
}

int McpMain() {
    std::wstring client;
    std::string buf, line;
    char chunk[65536];
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
        buf.append(chunk, got);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            Json msg;
            if (!JsonParse(line, msg)) {
                Send(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}})");
                continue;
            }
            if (IsToolCall(msg)) {  // on a thread of its own: the next message is read meanwhile
                std::thread([msg = std::move(msg), c = client]() mutable { Answer(msg, c); }).detach();
                continue;
            }
            Answer(msg, client);
        }
    }
    return 0;
}

// ---- command line: for trying it and for scripts

void Print(const std::wstring& s) {
    const std::string u = Utf8(s);
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u.data(), static_cast<DWORD>(u.size()), &w, nullptr);
}

int CliMain(int argc, wchar_t** argv, bool viaDaemon) {
    if (argc < 3) {
        Print(L"seek --cli|--call <find|grep|recent> [json arguments]\n");
        return 2;
    }
    Json args;
    args.kind = Json::Obj;
    if (argc >= 4 && !JsonParse(Utf8(argv[3]), args)) {
        Print(L"The arguments must be a JSON object, like {\"query\":\"report\"}\n");
        return 2;
    }
    ToolOut out;
    if (viaDaemon) {
        out = CallTool(Utf8(argv[2]), args, L"command line");
    } else {
        LocalReady();
        out = RunTool(Utf8(argv[2]), args, L"command line");
    }
    Print(out.text);
    if (!out.text.empty() && out.text.back() != L'\n') Print(L"\n");
    return out.error ? 1 : 0;
}

// seek --reindex [C D ...]: walks the drives again now, where Seek keeps its own index (Jolt keeps its own current).
int ReindexMain(int argc, wchar_t** argv) {
    LoadRules();
    std::vector<wchar_t> drives;
    for (int i = 2; i < argc; ++i)
        if (iswalpha(argv[i][0])) drives.push_back(static_cast<wchar_t>(towupper(argv[i][0])));
    if (drives.empty()) drives = FixedDrives();
    for (const wchar_t c : drives) {
        if (JoltHas(c)) {
            Print(std::wstring(1, c) + L": uses Jolt's index, which Jolt keeps current.\n");
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto v = BuildIndex(c);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const bool saved = SaveIndex(*v, CopyName(c, v->stamp));
        wchar_t took[32];
        swprintf_s(took, L"%.1f s", s);
        Print(std::wstring(1, c) + L": " + Thousands(v->count) + L" names in " + took + (saved ? L"" : L", but it couldn't be saved") + L".\n");
    }
    Print(L"A running background process picks the new index up within 10 minutes.\n");
    return 0;
}

// seek --status: how the background process is, for the user and for issues to come with the numbers.
int StatusMain() {
    LoadRules();
    HANDLE h = ConnectDaemon(0);
    if (!h) {
        Print(L"Seek " SEEK_VERSION L": the background process isn't running. It starts with an agent's first call, or with  seek --call find {\\\"query\\\":\\\"x\\\"}\n"
              L"Its files: " + DataDir() + L"\nSettings: " + SettingsFile() + L"\n");
        return 1;
    }
    std::string reply;
    WriteFrame(h, "{\"build\":" + JsonQuote(kBuild) + ",\"tool\":\"__status\"}");
    const bool got = ReadFrame(h, reply);
    CloseHandle(h);
    Json j;
    const Json* t = got && JsonParse(reply, j) ? j.at("text") : nullptr;
    if (!t) {
        Print(L"The background process didn't answer.\n");
        return 1;
    }
    Print(Wide(t->s));
    return 0;
}

int StopMain() {
    HANDLE h = ConnectDaemon(0);
    if (!h) return 0;
    std::string reply;
    WriteFrame(h, "{\"build\":" + JsonQuote(kBuild) + ",\"tool\":\"__stop\"}");
    ReadFrame(h, reply);
    CloseHandle(h);
    WaitDaemonGone();  // then the executable can be replaced
    Sleep(200);
    return 0;
}
