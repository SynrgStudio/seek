// The tools agents call: find (names anywhere), grep (text under a folder) and recent (what the user has been working
// on). The answers are plain text, short, and say why each result is there, so an agent can decide without opening
// every file. Every call goes to the log.

struct ToolOut {
    std::wstring text;
    bool error = false;
};

std::wstring ArgStr(const Json& a, const char* k) {
    const Json* v = a.at(k);
    if (!v) return L"";
    if (v->kind == Json::Str) return Wide(v->s);
    if (v->kind == Json::Num) return std::to_wstring(static_cast<long long>(v->n));
    return L"";
}
int ArgInt(const Json& a, const char* k, int def, int lo, int hi) {
    const Json* v = a.at(k);
    int x = def;
    if (v && v->kind == Json::Num) x = static_cast<int>(v->n);
    else if (v && v->kind == Json::Str && !v->s.empty()) x = atoi(v->s.c_str());
    return std::clamp(x, lo, hi);
}
bool ArgBool(const Json& a, const char* k, bool def) {
    const Json* v = a.at(k);
    if (!v) return def;
    if (v->kind == Json::Bool) return v->b;
    if (v->kind == Json::Str) return v->s == "true" || v->s == "yes" || v->s == "1";
    if (v->kind == Json::Num) return v->n != 0;
    return def;
}
// A list given as an array or as one string separated by commas or spaces.
std::vector<std::wstring> ArgList(const Json& a, const char* k) {
    std::vector<std::wstring> out;
    const Json* v = a.at(k);
    if (!v) return out;
    auto split = [&](const std::wstring& s) {
        std::wstring cur;
        for (wchar_t c : s + L",") {
            if (c == L',' || c == L';' || iswspace(c)) {
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    };
    if (v->kind == Json::Str) split(Wide(v->s));
    if (v->kind == Json::Arr)
        for (const Json& x : v->a)
            if (x.kind == Json::Str) split(Wide(x.s));
    return out;
}

bool WaitIndex(int ms) {
    for (int waited = 0; !g_indexReady && waited < ms; waited += 50) Sleep(50);
    return g_indexReady;
}

// While a drive is indexed for the first time (without Jolt), its names are missing from searches on all drives.
std::wstring IndexingNote() {
    const uint32_t mask = g_indexing;
    std::wstring s;
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) s += std::wstring(s.empty() ? L"" : L", ") + static_cast<wchar_t>(L'A' + i) + L": (" + Thousands(g_indexed[i]) + L" names so far)";
    if (s.empty()) return L"";
    return L"(Still indexing " + s + L" for the first time: until that ends, names there are missing. A path there is walked right away.)\n";
}

std::wstring IndexAge() {
    int64_t oldest = 0;
    for (const auto& v : Volumes())
        if (v->stamp && (!oldest || v->stamp < oldest)) oldest = v->stamp;
    return oldest ? Ago(oldest) : L"unknown";
}

// Checks a folder argument: empty when fine, or what is wrong.
std::wstring CheckFolder(const std::wstring& path, bool fileOk) {
    if (path.size() < 3 || path[1] != L':') {
        if (path.rfind(L"\\\\", 0) != 0) return L"Give a full path, like C:\\Users\\me\\project.";
    }
    if (Private(Key(path))) return L"That place is private: Seek doesn't look there.";
    const DWORD at = GetFileAttributesW(LongPath(path).c_str());
    if (at == INVALID_FILE_ATTRIBUTES) return L"Not found: " + path;
    if (!fileOk && !(at & FILE_ATTRIBUTE_DIRECTORY)) return L"That is a file; give the folder it is in.";
    return L"";
}

// ---- find

struct Found {
    std::wstring path, key;
    bool dir = false;
    int key0 = 0;  // name score
    int rank = 0;
    int percent = 0;
    int boost = 0;
    std::vector<std::wstring> why;
};

ToolOut ToolFind(const Json& a) {
    const std::wstring query = Trim(ArgStr(a, "query"));
    if (query.empty()) return {L"find needs a query: a name or part of one, like \"report\", \"jolt.exe\", \".blend\" or \"*.log\".", true};
    std::wstring scope = ArgStr(a, "path"), nearDir = ArgStr(a, "near");
    if (!scope.empty()) scope = Normalize(Expand(scope));
    if (!nearDir.empty()) nearDir = Normalize(Expand(nearDir));
    const int limit = ArgInt(a, "limit", 20, 1, 200);
    const bool summary = ArgBool(a, "summary", false);
    NameQuery q = ParseNameQuery(query);
    const std::wstring type = Lower(ArgStr(a, "type"));
    if (type == L"file" || type == L"files") q.kind = 2;
    if (type == L"folder" || type == L"folders" || type == L"dir") q.kind = 1;
    if (q.Empty() && q.kind == 0) return {L"That query only leaves things out: add a name, an extension or a wildcard.", true};
    if (!scope.empty())
        if (std::wstring bad = CheckFolder(scope, false); !bad.empty()) return {bad, true};
    // a folder doesn't need the index: one on a drive without it yet is walked
    if (!WaitIndex(scope.empty() ? 30000 : 3000) && scope.empty()) return {L"The drive index is still loading. Try again in a few seconds.", true};
    const std::wstring building = scope.empty() ? IndexingNote() : L"";
    LoadUsage();
    std::vector<AddedItem> added;  // what was created since the index was taken: below the folder, when there is one
    {
        const std::wstring scopeKey = Key(scope);
        std::lock_guard lk(g_actM);
        if (scope.empty()) added = g_added;
        else
            for (const AddedItem& x : g_added)
                if (IsUnder(x.key, scopeKey)) added.push_back(x);
    }
    const auto vols = Volumes();
    const size_t keep = summary ? 20000 : static_cast<size_t>(std::max(limit * 10, 300));
    std::wstring buf;
    auto toFound = [&](const ScanOut& s) {
        std::vector<Found> out;
        std::unordered_set<std::wstring> seen;
        for (const Cand& c : s.best) {
            Found f;
            if (c.vol == UINT32_MAX) {
                f.path = added[c.node].path;
                f.dir = added[c.node].dir;
            } else {
                f.path = NodePath(*vols[c.vol], c.node);
                f.dir = vols[c.vol]->node[c.node].dir != 0;
            }
            f.key = Key(f.path);
            if (!seen.insert(f.key).second || Private(f.key) || Removed(f.key)) continue;
            bool parts = true;
            for (const std::wstring& p : q.pathParts) parts = parts && f.key.find(p) != std::wstring::npos;
            if (!parts) continue;
            f.key0 = c.key;
            f.rank = c.rank;
            f.percent = c.percent;
            out.push_back(std::move(f));
        }
        return out;
    };
    // is the folder in the index? (one created after it was taken, or on a drive it doesn't cover, is walked instead)
    bool indexed = scope.empty();
    for (const auto& v : vols) {
        uint32_t n;
        if (!indexed && towupper(scope[0]) == v->letter && FindNode(*v, scope, n)) indexed = true;
    }
    // letters scattered along a long name match by chance: counted apart, and shown only when nothing better is there
    std::atomic<uint64_t> scattered{0};
    auto realMatch = [&](const wchar_t* name, size_t n, bool dir, uint8_t depth, Cand& c, std::wstring& b) {
        int sc;
        if (!NameMatch(q, name, n, dir, sc, b)) return false;
        c.rank = q.words.size() == 1 ? NameRank(name, n, q.words[0]) : 0;
        c.key = sc * 8 - std::min<int>(depth, 40);
        if (c.key <= kNearFloor) scattered.fetch_add(1, std::memory_order_relaxed);
        return true;
    };
    NameQuery shape = q;  // the same filters without the words, for close names
    shape.words.clear();
    const NearPattern close(q.words.size() == 1 ? q.words[0] : L"", 75);
    auto nearMatch = [&](const wchar_t* name, size_t n, bool dir, uint8_t, Cand& c, std::wstring& b) {
        int sc;
        if (!NameMatch(shape, name, n, dir, sc, b)) return false;
        const int p = NearMatch(name, n, close);
        if (!p) return false;
        c.percent = static_cast<int16_t>(p);
        c.key = NearScore(p, n);
        return true;
    };
    // a folder outside the index: walked, with the same scoring
    auto walkScan = [&](auto match) {
        ScanOut s;
        std::vector<AddedItem> walked;
        WalkTree(scope, GetTickCount64() + 4000, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
            const std::wstring k = Key(p);
            if (Private(k)) return false;
            const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            Cand c{0, UINT32_MAX, static_cast<uint32_t>(walked.size()), 0, 0};
            const uint8_t depth = static_cast<uint8_t>(std::min<size_t>(250, std::count(p.begin(), p.end(), L'\\')));
            if (match(fd.cFileName, wcslen(fd.cFileName), dir, depth, c, buf)) {
                ++s.total;
                walked.push_back({p, dir, k});
                s.best.push_back(c);
            }
            return !dir || !JunkDirs().count(Lower(fd.cFileName));
        });
        std::sort(s.best.begin(), s.best.end(), [](const Cand& x, const Cand& y) { return x.key > y.key; });
        if (s.best.size() > keep) s.best.resize(keep);
        added = std::move(walked);  // toFound reads the paths from here
        return s;
    };
    ScanOut real = indexed ? ScanIndex(vols, added, keep, scope, realMatch) : walkScan(realMatch);
    std::vector<Found> found = toFound(real);
    const auto good = std::count_if(found.begin(), found.end(), [](const Found& f) { return f.key0 > kNearFloor; });
    if (good > 0) {  // the scattered ones only say something when there is nothing else
        found.erase(std::remove_if(found.begin(), found.end(), [](const Found& f) { return f.key0 <= kNearFloor; }), found.end());
        real.total -= std::min<uint64_t>(real.total, scattered.load());
    }
    bool nearOnly = false;
    if (close.mistakes >= 0 && good == 0 && !summary) {  // close names, only when no name has the letters in order
        ScanOut nearScan = indexed ? ScanIndex(vols, added, keep, scope, nearMatch) : walkScan(nearMatch);
        std::vector<Found> more = toFound(nearScan);
        nearOnly = good == 0 && !more.empty();
        std::unordered_set<std::wstring> have;
        for (const Found& f : found) have.insert(f.key);
        for (Found& f : more)
            if (have.insert(f.key).second) found.push_back(std::move(f));
    }
    const std::wstring where = scope.empty() ? L"on all drives" : L"under " + scope;
    if (found.empty())
        return {L"Nothing named like \"" + query + L"\" " + where + L". (Index taken " + IndexAge() +
                L"; what was created since is watched.) For text inside files, use grep.\n" + building};

    if (summary) {
        std::unordered_map<std::wstring, int> byFolder, byArea, byExt, byDrive;
        std::unordered_map<std::wstring, std::wstring> folderName;
        // an area: the folder three levels below the search's root (or the drive), where a pile of matches lives
        const size_t rootDepth = scope.empty() ? 0 : static_cast<size_t>(std::count(scope.begin(), scope.end(), L'\\')) - (scope.back() == L'\\' ? 1 : 0);
        for (const Found& f : found) {
            if (f.percent) continue;
            const std::wstring parent = ParentOf(f.path), pk = Key(parent);
            ++byFolder[pk];
            folderName[pk] = parent;
            size_t cut = 0, seps = 0;
            for (size_t i = 0; i < parent.size(); ++i)
                if (parent[i] == L'\\' && ++seps == rootDepth + (scope.empty() ? 4 : 3)) {
                    cut = i;
                    break;
                }
            const std::wstring area = cut ? parent.substr(0, cut) : parent;
            ++byArea[Key(area)];
            folderName[Key(area)] = area;
            ++byExt[f.dir ? L"(folders)" : (ExtOf(LeafOf(f.path)).empty() ? L"(no extension)" : ExtOf(LeafOf(f.path)))];
            ++byDrive[f.path.substr(0, 2)];
        }
        auto top = [](const std::unordered_map<std::wstring, int>& m, size_t n) {
            std::vector<std::pair<int, std::wstring>> v;
            for (const auto& [k, c] : m) v.push_back({c, k});
            std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
            if (v.size() > n) v.resize(n);
            return v;
        };
        std::wstring out = L"find \"" + query + L"\" " + where + L": " + Thousands(real.total) + L" matches" +
                           (real.total > found.size() ? L" (grouped from the best " + Thousands(found.size()) + L" by name)" : L"") + L".\n";
        auto rows = [&](const std::vector<std::pair<int, std::wstring>>& v) {
            std::wstring s;
            for (const auto& [c, k] : v)
                s += L"  " + std::wstring(std::max<int>(0, 6 - static_cast<int>(std::to_wstring(c).size())), L' ') + std::to_wstring(c) + L"  " +
                     Relative(folderName[k], scope) + L"\n";
            return s;
        };
        const auto areas = top(byArea, 10), folders = top(byFolder, 8);
        out += L"By area:\n" + rows(areas);
        if (byArea.size() > areas.size()) out += L"  … and " + Thousands(byArea.size() - areas.size()) + L" more areas\n";
        out += L"Most in one folder:\n" + rows(folders);
        if (byFolder.size() > folders.size()) out += L"  … " + Thousands(byFolder.size()) + L" folders in all\n";
        out += L"By extension:";
        for (const auto& [c, k] : top(byExt, 12)) out += L"  " + k + L" " + Thousands(c);
        if (scope.empty()) {
            out += L"\nBy drive:";
            for (const auto& [c, k] : top(byDrive, 26)) out += L"  " + k + L" " + Thousands(c);
        }
        return {out + L"\n" + building};
    }

    // what the user is doing: things changed lately, folders used in Jolt, and the neighbourhood of `near`
    const int64_t now = NowFt();
    const std::wstring nearKey = Key(nearDir);
    const std::wstring scopeKey = Key(scope);
    for (Found& f : found) {
        if (NoiseBelow(f.key, scopeKey)) f.boost -= 2000;  // caches, package stores, AppData: below the user's own files
        if (f.rank >= kExactName) f.why.push_back(f.rank == kExactProgram ? L"program, exact name" : L"exact name");
        else if (f.rank == kProgramName) f.why.push_back(L"program");
        if (f.percent) {
            f.why.push_back(L"close to what was typed: " + std::to_wstring(f.percent) + L"%");
            continue;
        }
        if (const int64_t t = LastActivity(f.key)) {
            const int64_t age = now - t;
            f.boost += age < kHour ? 3000 : age < kDay ? 1500 : age < 7 * kDay ? 600 : 0;
            if (age < 7 * kDay) f.why.push_back(L"changed " + Ago(t));
        }
        std::wstring used;
        if (const double h = JoltHeat(f.key, &used); h > 0) {
            f.boost += static_cast<int>(std::min(1200.0, 300.0 * h));
            f.why.push_back(L"in a folder used in Jolt");
        }
        if (!nearKey.empty()) {
            if (IsUnder(f.key, nearKey)) {
                f.boost += 2500;
                f.why.push_back(L"inside " + LeafOf(nearDir));
            } else {
                size_t shared = 0;
                for (size_t i = 0; i < std::min(f.key.size(), nearKey.size()) && f.key[i] == nearKey[i]; ++i)
                    if (f.key[i] == L'\\') ++shared;
                if (shared > 1) f.boost += static_cast<int>(150 * (shared - 1));
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& x, const Found& y) {
        if ((x.percent == 0) != (y.percent == 0)) return x.percent == 0;
        return x.key0 + x.boost > y.key0 + y.boost;
    });
    std::wstring list;
    int shown = 0;
    for (const Found& f : found) {
        if (shown >= limit) break;
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!FileInfo(f.path, d)) {
            if (GetLastError() != ERROR_ACCESS_DENIED) continue;  // gone since the index was taken
        }
        std::wstring line = Relative(f.path, scope);
        if (f.dir || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) line += L"\\  ·  folder";
        else if (d.dwFileAttributes) line += L"  ·  " + SizeText((static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow);
        if (d.dwFileAttributes) line += L"  ·  modified " + Ago(Ft(d.ftLastWriteTime));
        if (!f.why.empty()) {
            line += L"  ·  ";
            for (size_t i = 0; i < f.why.size(); ++i) line += (i ? L", " : L"") + f.why[i];
        }
        list += line + L"\n";
        ++shown;
    }
    if (!shown) return {L"Nothing named like \"" + query + L"\" " + where + L" still exists (the index had some that are gone)."};
    std::wstring head = L"find \"" + query + L"\" " + where + (scope.empty() ? L"" : L" (paths below it)") + L": ";
    if (nearOnly) head += L"no name has those letters in order; these are close (typo-tolerant):\n";
    else head += Thousands(real.total) + (real.total == 1 ? L" match" : L" matches") + (static_cast<uint64_t>(shown) < real.total ? L", the best " + std::to_wstring(shown) : L"") + L":\n";
    std::wstring tail;
    if (static_cast<uint64_t>(shown) < real.total && !nearOnly)
        tail = L"(" + Thousands(real.total - shown) + L" more: narrow the query, add path, use summary for counts, or raise limit up to 200.)\n";
    return {head + list + tail + building};
}

// ---- grep

ToolOut ToolGrep(const Json& a) {
    GrepQuery q;
    q.pattern = ArgStr(a, "pattern");
    if (q.pattern.empty()) return {L"grep needs a pattern: the text to look for.", true};
    q.root = ArgStr(a, "path");
    if (q.root.empty()) return {L"grep needs a path: the folder (or file) to search in. To find files by name anywhere, use find.", true};
    q.root = Normalize(Expand(q.root));
    if (std::wstring bad = CheckFolder(q.root, true); !bad.empty()) return {bad, true};
    q.regex = ArgBool(a, "regex", false);
    const std::wstring cs = Lower(ArgStr(a, "case"));
    bool upper = false;
    for (wchar_t c : q.pattern) upper |= iswupper(c) != 0;
    q.ignoreCase = cs == L"insensitive" || (cs != L"sensitive" && !upper);
    for (std::wstring g : ArgList(a, "glob")) {
        std::replace(g.begin(), g.end(), L'/', L'\\');
        g = Lower(g);
        if (g.size() > 1 && g[0] == L'!') q.exclude.push_back(g.substr(1));
        else q.include.push_back(g);
    }
    q.context = ArgInt(a, "context", 1, 0, 5);
    q.perFile = ArgInt(a, "per_file", 5, 1, 200);
    q.documents = ArgBool(a, "documents", true);
    q.hidden = ArgBool(a, "hidden", false);
    q.ignored = ArgBool(a, "ignored", false);
    q.budgetMs = static_cast<uint64_t>(ArgInt(a, "time_budget_ms", 6000, 500, 60000));
    const int limit = ArgInt(a, "limit", 30, 1, 300), offset = ArgInt(a, "offset", 0, 0, 1000000);
    const bool summary = ArgBool(a, "summary", false);
    const bool isFile = !(GetFileAttributesW(LongPath(q.root).c_str()) & FILE_ATTRIBUTE_DIRECTORY);
    const std::wstring base = isFile ? ParentOf(q.root) : q.root;

    GrepResult r = RunGrep(q);
    if (!r.error.empty()) return {r.error, true};
    std::wstring note;
    if (r.files.empty() && !q.ignoreCase && !q.regex) {  // the exact case said no: what ignoring it finds
        GrepQuery q2 = q;
        q2.ignoreCase = true;
        GrepResult r2 = RunGrep(q2);
        if (!r2.files.empty()) {
            note = L"No match with that exact case; ignoring case:\n";
            r = std::move(r2);
        }
    }
    uint64_t lines = 0;
    for (const FileHits& f : r.files) lines += f.matches;
    std::wstring head = L"grep \"" + q.pattern + L"\" in " + q.root + L": " + Thousands(lines) + (lines == 1 ? L" matching line" : L" matching lines") +
                        L" in " + Thousands(r.files.size()) + (r.files.size() == 1 ? L" file" : L" files") + L" (searched " + Thousands(r.searched) +
                        L" files" + (r.docs ? L", " + Thousands(r.docs) + L" of them documents" : L"") + (r.fromMemory ? L", kept in memory" : L"") + L").\n";
    std::wstring tail;
    if (r.timedOut) tail += L"Stopped at the time budget (" + std::to_wstring(q.budgetMs / 1000) + L" s): not everything was searched. Narrow path or glob, or raise time_budget_ms.\n";
    if (!r.noFilter.empty()) {
        std::wstring kinds;
        int n = 0;
        for (const auto& [ext, c] : r.noFilter) {
            kinds += (kinds.empty() ? L"" : L", ") + ext;
            n += c;
        }
        tail += Thousands(n) + L" documents (" + kinds + L") couldn't be read: no text filter for them on this PC.\n";
    }
    if (r.files.empty()) {
        std::wstring out = head + tail;
        if (q.pattern.find_first_of(L" \\/") == std::wstring::npos && g_indexReady) {  // maybe it is a file's name
            Json fa;
            fa.kind = Json::Obj;
            Json qv, pv, lv;
            qv.kind = pv.kind = Json::Str;
            qv.s = Utf8(q.pattern);
            pv.s = Utf8(base);
            lv.kind = Json::Num;
            lv.n = 5;
            fa.o = {{"query", qv}, {"path", pv}, {"limit", lv}};
            ToolOut names = ToolFind(fa);
            if (!names.error && names.text.rfind(L"Nothing", 0) != 0) out += L"No text matches, but some names look like it:\n" + names.text;
        }
        return {out};
    }
    if (summary) {
        std::vector<const FileHits*> v;
        for (const FileHits& f : r.files) v.push_back(&f);
        std::stable_sort(v.begin(), v.end(), [](const FileHits* x, const FileHits* y) { return x->matches > y->matches; });
        std::wstring out = note + head + L"Most matches:\n";
        for (size_t i = 0; i < v.size() && i < 40; ++i)
            out += L"  " + std::to_wstring(v[i]->matches) + L"  " + Relative(v[i]->path, base) + (v[i]->doc ? L"  (document)" : L"") + L"\n";
        if (v.size() > 40) out += L"  … and " + Thousands(v.size() - 40) + L" more files\n";
        return {out + tail};
    }
    std::wstring body;
    int index = 0, shown = 0;
    for (const FileHits& f : r.files) {
        std::wstring block;
        for (const LineHit& h : f.lines) {
            if (index++ < offset) continue;
            if (shown >= limit) break;
            for (const auto& [n, t] : h.before) block += L"  " + std::to_wstring(n) + L"- " + t + L"\n";
            block += L"  " + std::to_wstring(h.line) + L": " + h.text + L"\n";
            for (const auto& [n, t] : h.after) block += L"  " + std::to_wstring(n) + L"- " + t + L"\n";
            ++shown;
        }
        if (!block.empty()) {
            std::wstring title = Relative(f.path, base);
            if (f.doc) title += L"  (text taken from the document)";
            if (f.matches > static_cast<int>(f.lines.size())) title += L"  (" + std::to_wstring(f.matches) + L" matching lines, " + std::to_wstring(f.lines.size()) + L" shown; per_file for more)";
            body += title + L"\n" + block;
        }
        if (shown >= limit) break;
    }
    int stored = 0;
    for (const FileHits& f : r.files) stored += static_cast<int>(f.lines.size());
    if (offset + shown < stored) tail += L"More: offset=" + std::to_wstring(offset + shown) + L".\n";
    if (!isFile) head += L"Paths below " + base + L".\n";
    return {note + head + body + tail};
}

// ---- recent

ToolOut ToolRecent(const Json& a) {
    std::wstring scope = ArgStr(a, "path");
    if (!scope.empty()) {
        scope = Normalize(Expand(scope));
        if (std::wstring bad = CheckFolder(scope, false); !bad.empty()) return {bad, true};
    }
    const std::wstring query = Trim(ArgStr(a, "query"));
    const NameQuery q = ParseNameQuery(query);
    const int limit = ArgInt(a, "limit", 20, 1, 200);
    const int64_t span = ParseSpan(ArgStr(a, "since"), 7 * kDay), from = NowFt() - span;
    const bool system = ArgBool(a, "include_system", false);
    const std::wstring scopeKey = Key(scope);
    LoadUsage();
    struct Item {
        std::wstring path;
        int64_t time;
        std::wstring what;
        bool folder = false;
    };
    std::unordered_map<std::wstring, Item> items;
    RepoIgnore ignore;  // build outputs and the like: what a repository's .gitignore leaves out is churn too
    auto add = [&](const std::wstring& path, int64_t t, const std::wstring& what, bool folder) {
        const std::wstring k = Key(path);
        if (t < from || Private(k) || (!scope.empty() && !IsUnder(k, scopeKey)) || (!system && NoiseBelow(k, scopeKey))) return;
        if (!system && !folder && ignore.Check(path, false)) return;  // a folder the user opened in Jolt always counts
        if (!query.empty()) {
            const std::wstring leaf = LeafOf(path);
            int sc;
            std::wstring b;
            if (!NameMatch(q, leaf.c_str(), leaf.size(), folder, sc, b)) return;
        }
        auto it = items.find(k);
        if (it == items.end() || it->second.time < t) items[k] = {path, t, what, folder};
    };
    int64_t since;
    {
        std::lock_guard lk(g_actM);
        since = g_actSince;
        for (const auto& [k, x] : g_act)
            if (x.kind != kDeleted) add(x.path, x.time, ActName(x.kind), false);
    }
    {
        std::lock_guard lk(g_useM);
        for (const Use& u : g_use) add(u.path, u.last, L"folder opened in Jolt", true);
    }
    bool walked = false, walkCut = false;
    if (!scope.empty()) {  // the dates on disk too: also what changed before Seek was watching
        walked = true;
        const uint64_t deadline = GetTickCount64() + 3000;
        WalkTree(scope, deadline, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
            const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (dir)
                return !JunkDirs().count(Lower(fd.cFileName)) && !Private(Key(p)) && Key(p) != Key(DataDir()) &&  // not Seek's own files
                       (system || !ignore.Check(p, true));
            add(p, Ft(fd.ftLastWriteTime), L"modified", false);
            return false;
        });
        walkCut = GetTickCount64() > deadline;
    }
    std::vector<Item> v;
    v.reserve(items.size());
    for (auto& [k, it] : items) v.push_back(std::move(it));
    std::sort(v.begin(), v.end(), [](const Item& x, const Item& y) { return x.time > y.time; });
    std::wstring list;
    int shown = 0;
    for (const Item& it : v) {
        if (shown >= limit) break;
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!FileInfo(it.path, d)) continue;  // gone
        const bool dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dir && !it.folder) continue;  // a folder's own date changes with what is inside: the files say more
        list += Relative(it.path, scope) + (dir ? L"\\" : L"") + L"  ·  " + it.what + L" " + Ago(it.time) +
                (dir ? L"" : L"  ·  " + SizeText((static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow)) + L"\n";
        ++shown;
    }
    std::wstring head = L"recent " + (scope.empty() ? std::wstring(L"on all drives") : L"under " + scope + L" (paths below it)") +
                        (query.empty() ? L"" : L", names like \"" + query + L"\"") + L", since " + Ago(from) + L", newest first";
    std::wstring notes = L"(Changes seen live since " + DateText(since) + L"; folders opened in Jolt";
    notes += walked ? L"; dates read from disk" + std::wstring(walkCut ? L" for part of the folder, it is big" : L"") + L")" : L")";
    if (!shown)
        return {head + L": nothing. " + notes + L"\n" +
                (scope.empty() ? L"Seek only knows what changed while it was running. Pass path to read dates from disk.\n" : L"")};
    std::wstring tail;
    if (v.size() > static_cast<size_t>(shown)) tail = L"(more: raise limit, narrow with query or path.)\n";
    if (!system) tail += L"System places (AppData, Windows, Program Files, temp, caches) and what .gitignore leaves out are left out; include_system shows them.\n";
    return {head + L":\n" + list + notes + L"\n" + tail};
}

// "0.4 ms", "12 ms", "1,234 ms"
std::wstring Ms(double ms) {
    if (ms < 10) {
        wchar_t b[32];
        swprintf_s(b, L"%.1f ms", ms);
        return b;
    }
    return Thousands(static_cast<uint64_t>(ms + 0.5)) + L" ms";
}

ToolOut RunTool(const std::string& name, const Json& args, const std::wstring& client) {
    const auto t0 = std::chrono::steady_clock::now();
    ToolOut out;
    try {
        if (name == "find") out = ToolFind(args);
        else if (name == "grep") out = ToolGrep(args);
        else if (name == "recent") out = ToolRecent(args);
        else out = {L"Unknown tool: " + Wide(name) + L". The tools are find, grep and recent.", true};
    } catch (const std::exception& e) {
        out = {L"Seek failed: " + Wide(e.what()), true};
    }
    std::string argText;
    JsonWrite(args, argText);
    Audit((client.empty() ? L"?" : client) + L"  " + Wide(name) + L" " + Wide(argText) + L"  ->  " + (out.error ? L"error" : L"ok") + L", " +
          Ms(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()));
    return out;
}
