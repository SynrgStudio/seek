// The tools agents call: find (names anywhere), grep (text under a folder) and recent (what the user has been working
// on). The answers are plain text, short, and say why each result is there, so an agent can decide without opening
// every file. Every call goes to the log.

struct ToolOut {
    std::wstring text;
    bool error = false;
    bool nothing = false;  // it found nothing
    bool cut = false;      // the time budget ended it: the answer is part of what there is
    std::wstring code;     // an error's kind, for programs (PROTOCOL.md): bad_args, not_found... ("failed" when empty)
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
        int braces = 0;  // a comma inside {a,b} is the glob's
        for (wchar_t c : s + L",") {
            if (c == L'{') ++braces;
            if (c == L'}' && braces > 0) --braces;
            if ((c == L',' || c == L';' || iswspace(c)) && braces == 0) {
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

// Right after the background process starts, it reads what was made while none watched: a search waits for that.
void WaitCatchUp(int ms) {
    for (int waited = 0; g_catchingUp && waited < ms; waited += 50) Sleep(50);
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
// What is wrong with a path given, or nothing; code: the error's kind, for programs.
std::wstring CheckFolder(const std::wstring& path, bool fileOk, std::wstring* code = nullptr) {
    auto bad = [&](const wchar_t* c, const std::wstring& why) {
        if (code) *code = c;
        return why;
    };
    if (path.size() < 3 || path[1] != L':') {
        if (path.rfind(L"\\\\", 0) != 0) return bad(L"bad_args", L"Give a full path, like C:\\Users\\me\\project.");
    }
    if (Private(Key(path))) return bad(L"private", L"That place is private: Seek doesn't look there.");
    const DWORD at = GetFileAttributesW(LongPath(path).c_str());
    if (at == INVALID_FILE_ATTRIBUTES) return bad(L"not_found", L"Not found: " + path);
    if (!fileOk && !(at & FILE_ATTRIBUTE_DIRECTORY)) return bad(L"not_a_folder", L"That is a file; give the folder it is in.");
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

// How many of the best names find ranks again by what the user is doing. The same for every call, so that the pages
// asked for with offset come from one order and none is skipped or repeated; paging goes that far.
constexpr size_t kRanked = 2000;

// fn(i) for every i under n, on several threads when there are enough to be worth starting them.
template <class F>
void ForEach(size_t n, F fn) {
    const unsigned nth = n < 400 ? 1u : std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t from; (from = next.fetch_add(64, std::memory_order_relaxed)) < n;)
            for (size_t i = from; i < std::min(n, from + 64); ++i) fn(i);
    };
    std::vector<std::thread> th;
    for (unsigned t = 1; t < nth; ++t) th.emplace_back(work);
    work();
    for (auto& x : th) x.join();
}

// What .gitignore leaves out below a folder, and dependency and cache folders, as grep leaves them out: worked out once
// per search from the index's folders. A folder's ignore files are read only where the index lists one.
struct IgnoreMap {
    uint32_t vol = UINT32_MAX;
    std::unordered_map<uint32_t, int> chainOf;  // the folders kept below it, and their rules (in chains); not the others
    std::vector<IgnChain> chains;
    bool Hides(const Volume& v, uint32_t i) const {  // a name in the index, below the folder
        const IdxNode& nd = v.node[i];
        if (nd.dir) return !chainOf.count(i);
        const auto it = chainOf.find(nd.parent);
        if (it == chainOf.end()) return true;  // in a folder left out
        const IgnChain& c = chains[static_cast<size_t>(it->second)];
        return !c.empty() && Ignored(c, Key(NodePath(v, i)), Lower(std::wstring(v.name + nd.nameOff, nd.nameLen)), false);
    }
};

IgnoreMap MakeIgnoreMap(const Volume& v, uint32_t vol, uint32_t root, const std::wstring& rootPath) {
    IgnoreMap m;
    m.vol = vol;
    v.Kids();
    IgnChain top = IgnoresAbove(rootPath);
    AddIgnores(rootPath, top);
    m.chains.push_back(std::move(top));
    m.chainOf[root] = 0;
    struct Item {
        uint32_t node;
        std::wstring path;
        int chain;
    };
    std::vector<Item> stack{{root, rootPath, 0}};
    const auto& junk = JunkDirs();
    auto own = [](const wchar_t* n, size_t len) {  // a name that brings rules of its own
        for (const wchar_t* r : {L".gitignore", L".ignore", L".git"})
            if (wcslen(r) == len && _wcsnicmp(n, r, len) == 0) return true;
        return false;
    };
    while (!stack.empty()) {
        const Item it = std::move(stack.back());
        stack.pop_back();
        for (uint32_t k = v.kidStart[it.node]; k < v.kidStart[it.node + 1]; ++k) {
            const uint32_t c = v.kids[k];
            const IdxNode& nd = v.node[c];
            if (!nd.dir) continue;
            const std::wstring name(v.name + nd.nameOff, nd.nameLen), lower = Lower(name);
            if (junk.count(lower)) continue;
            const std::wstring path = Join(it.path, name);
            const IgnChain& rules = m.chains[static_cast<size_t>(it.chain)];
            if (!rules.empty() && Ignored(rules, Key(path), lower, true)) continue;
            int chain = it.chain;
            bool more = false;
            for (uint32_t j = v.kidStart[c]; j < v.kidStart[c + 1] && !more; ++j) more = own(v.name + v.node[v.kids[j]].nameOff, v.node[v.kids[j]].nameLen);
            if (more) {
                IgnChain next = rules;
                AddIgnores(path, next);
                m.chains.push_back(std::move(next));
                chain = static_cast<int>(m.chains.size() - 1);
            }
            m.chainOf[c] = chain;
            stack.push_back({c, path, chain});
        }
    }
    return m;
}

// rows: for a program that lists the results itself (Seek's window): that many of the best, as "D" or "F", a tab and
// the path on each line, after a line "#rows", how many match, 1 when that count is exact, and "real", "near" (close
// names, a typo) or "scattered". Nothing is read from the drive for them: the program reads what it shows.
ToolOut ToolFind(const Json& a, size_t rows = 0) {
    const std::wstring query = Trim(ArgStr(a, "query"));
    if (query.empty()) return {L"find needs a query: a name or part of one, like \"report\", \"jolt.exe\", \".blend\" or \"*.log\".", true, false, false, L"bad_args"};
    std::wstring scope = ArgStr(a, "path"), nearDir = ArgStr(a, "near");
    if (!scope.empty()) scope = Normalize(Expand(scope));
    if (!nearDir.empty()) nearDir = Normalize(Expand(nearDir));
    const int limit = ArgInt(a, "limit", 20, 1, 200), offset = ArgInt(a, "offset", 0, 0, 1000000);
    const bool summary = ArgBool(a, "summary", false);
    NameQuery q = ParseNameQuery(query);
    const std::wstring type = Lower(ArgStr(a, "type"));
    if (type == L"file" || type == L"files") q.kind = 2;
    if (type == L"folder" || type == L"folders" || type == L"dir") q.kind = 1;
    if (q.Empty() && q.kind == 0) return {L"That query only leaves things out: add a name, an extension or a wildcard.", true, false, false, L"bad_args"};
    if (!q.Names() && !q.pathParts.empty())
        return {L"\"" + query + L"\" says where to look, not what: add a name, an extension or a wildcard after the slash, like \"" + query + L"report\" or \"" +
                    query + L"*.md\".",
                true, false, false, L"bad_args"};
    std::wstring code;
    if (!scope.empty())
        if (std::wstring bad = CheckFolder(scope, false, &code); !bad.empty()) return {bad, true, false, false, code};
    // a folder doesn't need the index: one on a drive without it yet is walked
    if (!WaitIndex(scope.empty() ? 30000 : 3000) && scope.empty())
        return {L"The drive index is still loading. Try again in a few seconds.", true, false, false, L"index_loading"};
    WaitCatchUp(10000);
    std::wstring building = scope.empty() ? IndexingNote() : L"";
    LoadUsage();
    // what was created since the index was taken: all of it, or what is below the folder
    std::shared_ptr<const std::vector<AddedItem>> everything;
    std::vector<AddedItem> below;
    if (scope.empty()) {
        everything = AddedList();
    } else {
        const std::wstring scopeKey = Key(scope);
        std::lock_guard lk(g_actM);
        for (const auto& [k, x] : g_added)
            if (IsUnder(k, scopeKey)) below.push_back(x);
    }
    const std::vector<AddedItem>& added = everything ? *everything : below;
    const auto vols = Volumes();
    const size_t keep = rows ? rows : summary ? 20000 : kRanked;
    std::wstring buf;
    auto toFound = [&](const ScanOut& s) {
        std::vector<Found> all(s.best.size());
        std::vector<char> stays(s.best.size());
        ForEach(s.best.size(), [&](size_t i) {
            const Cand& c = s.best[i];
            Found& f = all[i];
            if (c.vol == UINT32_MAX) {
                f.path = added[c.node].path;
                f.dir = added[c.node].dir;
            } else {
                f.path = NodePath(*vols[c.vol], c.node);
                f.dir = vols[c.vol]->node[c.node].dir != 0;
            }
            f.key = Key(f.path);
            bool ok = !Private(f.key) && !Removed(f.key);
            for (const std::wstring& p : q.pathParts) ok = ok && f.key.find(p) != std::wstring::npos;
            for (const std::wstring& p : q.notPaths) ok = ok && !HasPathPart(f.key, p);
            f.key0 = c.key;
            f.rank = c.rank;
            f.percent = c.percent;
            stays[i] = ok;
        });
        std::vector<Found> out;
        std::unordered_set<std::wstring_view> seen;  // the keys stay where they are until the last one was looked at
        std::vector<size_t> kept;
        for (size_t i = 0; i < all.size(); ++i)
            if (seen.insert(all[i].key).second && stays[i]) kept.push_back(i);
        out.reserve(kept.size());
        for (size_t i : kept) out.push_back(std::move(all[i]));
        return out;
    };
    // is the folder in the index? (one created after it was taken, or on a drive it doesn't cover, is walked instead)
    bool indexed = scope.empty();
    for (const auto& v : vols) {
        uint32_t n;
        if (!indexed && towupper(scope[0]) == v->letter && FindNode(*v, scope, n)) indexed = true;
    }
    // A part of the path asked for ("docs/"): looked at for every name that matches, not only for the best ones kept.
    // Those are the shallowest, and what is asked for is often deep: "docs/ .png" used to find nothing.
    auto inParts = [&](const std::wstring& key) {
        for (const std::wstring& p : q.pathParts)
            if (key.find(p) == std::wstring::npos) return false;
        for (const std::wstring& p : q.notPaths)
            if (HasPathPart(key, p)) return false;
        return true;
    };
    // "!target/", "!node_modules/": one folder's name each. Then the names on the way up are compared, without making
    // the path of every name that matches: on every drive that is many, and making them took most of a second.
    std::vector<std::wstring> notNames;
    bool byNames = q.pathParts.empty() && !q.notPaths.empty();
    for (const std::wstring& p : q.notPaths) {
        if (p.size() > 2 && p.front() == L'\\' && p.back() == L'\\' && p.find(L'\\', 1) == p.size() - 1) notNames.push_back(p.substr(1, p.size() - 2));
        else byNames = false;
    }
    auto partsOf = [&](const Cand& c) {  // of a name in the index or made since; a walk looks at its own
        if ((q.pathParts.empty() && q.notPaths.empty()) || !indexed) return true;
        if (c.vol != UINT32_MAX && byNames) {
            const Volume& v = *vols[c.vol];
            int steps = 0;
            for (uint32_t n = c.node; n < v.count && steps++ < 260; n = v.node[n].parent) {
                const IdxNode& nd = v.node[n];
                for (const std::wstring& w : notNames)
                    if (nd.nameLen == w.size() && _wcsnicmp(v.name + nd.nameOff, w.c_str(), w.size()) == 0) return false;
                if (v.node[n].parent == n) break;
            }
            return true;
        }
        if (c.vol != UINT32_MAX) return inParts(Key(NodePath(*vols[c.vol], c.node)));
        return inParts(added[c.node].key.empty() ? Key(added[c.node].path) : added[c.node].key);
    };
    // Below a folder, what .gitignore leaves out there and dependency and cache folders are left out, as grep leaves them
    // out, unless ignored is true or the folder is itself in one of them. Counted, for the answer to say they are there.
    const std::wstring scopeKey0 = Key(scope);
    const std::vector<std::wstring> junkList(JunkDirs().begin(), JunkDirs().end());
    const bool follow = !scope.empty() && !ArgBool(a, "ignored", false) && !AnySegment(scopeKey0, junkList) && !RepoIgnore().Check(scope, true);
    IgnoreMap ignoreMap;
    RepoIgnore oneByOne;  // for names made since the index and for walked folders: asked one at a time, never at once
    if (follow && indexed)
        for (uint32_t vi = 0; vi < vols.size(); ++vi) {
            uint32_t n;
            if (towupper(scope[0]) == vols[vi]->letter && FindNode(*vols[vi], scope, n)) ignoreMap = MakeIgnoreMap(*vols[vi], vi, n, scope);
        }
    std::atomic<uint64_t> hidden{0};
    auto hides = [&](const Cand& c, bool count) {
        if (!follow || !indexed) return false;
        bool h;
        if (c.vol == UINT32_MAX) {
            const AddedItem& x = added[c.node];
            const std::wstring k = x.key.empty() ? Key(x.path) : x.key;
            h = (k.size() > scopeKey0.size() && AnySegment(k.substr(scopeKey0.size()), junkList)) || oneByOne.Check(x.path, x.dir);
        } else {
            h = c.vol == ignoreMap.vol && ignoreMap.Hides(*vols[c.vol], c.node);
        }
        if (h && count) hidden.fetch_add(1, std::memory_order_relaxed);
        return h;
    };
    // letters scattered along a long name match by chance: counted apart, and shown only when nothing better is there
    std::atomic<uint64_t> scattered{0};
    auto realMatch = [&](const wchar_t* name, size_t n, bool dir, uint8_t depth, Cand& c, std::wstring& b) {
        int sc;
        if (!NameMatch(q, name, n, dir, sc, b, &c.rank) || !partsOf(c) || hides(c, true)) return false;
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
        if (!p || !partsOf(c) || hides(c, false)) return false;
        c.percent = static_cast<int16_t>(p);
        c.key = NearScore(p, n);
        return true;
    };
    // a folder outside the index: walked, with the same scoring
    auto walkScan = [&](auto match, bool count = true) {
        ScanOut s;
        std::vector<AddedItem> walked;
        WalkTree(scope, GetTickCount64() + 4000, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
            const std::wstring k = Key(p);
            if (Private(k)) return false;
            const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            Cand c{0, UINT32_MAX, static_cast<uint32_t>(walked.size()), 0, 0};
            const uint8_t depth = static_cast<uint8_t>(std::min<size_t>(250, std::count(p.begin(), p.end(), L'\\')));
            const bool left = follow && oneByOne.Check(p, dir);  // what .gitignore leaves out
            if (left) {
                if (count && match(fd.cFileName, wcslen(fd.cFileName), dir, depth, c, buf)) hidden.fetch_add(1, std::memory_order_relaxed);
            } else if (match(fd.cFileName, wcslen(fd.cFileName), dir, depth, c, buf) && inParts(k)) {
                ++s.total;
                walked.push_back({p, dir, k});
                s.best.push_back(c);
            }
            return !dir || (!left && !JunkDirs().count(Lower(fd.cFileName)));
        });
        std::sort(s.best.begin(), s.best.end(), [](const Cand& x, const Cand& y) { return x.key > y.key; });
        if (s.best.size() > keep) s.best.resize(keep);
        below = std::move(walked);  // toFound reads the paths from here
        return s;
    };
    ScanOut real = indexed ? ScanIndex(vols, added, keep, scope, realMatch) : walkScan(realMatch);
    // How many there are: what is left to show when every match was kept; otherwise the index's count, less what was
    // dropped among the kept ones (the same path twice, private, deleted, or outside the path asked), which is about
    // right: the others weren't looked at.
    const bool allKept = real.best.size() >= real.total;
    const auto isGood = [](int key) { return key > kNearFloor; };
    const auto goodKept = std::count_if(real.best.begin(), real.best.end(), [&](const Cand& c) { return isGood(c.key); });
    std::vector<Found> found = toFound(real);
    const uint64_t dropped = real.best.size() - found.size();
    const auto good = std::count_if(found.begin(), found.end(), [&](const Found& f) { return isGood(f.key0); });
    bool exact = allKept;
    uint64_t total;
    if (good > 0) {  // the scattered ones only say something when there is nothing else
        found.erase(std::remove_if(found.begin(), found.end(), [&](const Found& f) { return !isGood(f.key0); }), found.end());
        real.total -= std::min<uint64_t>(real.total, scattered.load());
        // they are kept last (best is sorted by key), so with one of them kept, every real match was
        exact = allKept || !isGood(real.best.back().key);
        total = exact ? found.size() : real.total - std::min<uint64_t>(real.total, goodKept - good);
    } else {
        total = exact ? found.size() : real.total - std::min(real.total, dropped);
    }
    // With no name that has the letters in order and together, what is left: names close to what was typed (a typo),
    // or else names with the letters scattered along them. Either way the answer says so.
    bool nearOnly = false;
    if (close.mistakes >= 0 && good == 0 && !summary) {
        ScanOut nearScan = indexed ? ScanIndex(vols, added, keep, scope, nearMatch) : walkScan(nearMatch, false);
        std::vector<Found> more = toFound(nearScan);
        if (!more.empty()) {  // they are what there is to page through; the scattered ones say less than they do
            nearOnly = true;
            found = std::move(more);
            total = found.size();
            exact = true;
        }
    }
    const bool scatteredOnly = good == 0 && !nearOnly && !found.empty();
    const std::wstring where = scope.empty() ? L"on all drives" : L"under " + scope;
    if (found.empty() && hidden && !rows) {  // only where .gitignore leaves things out: shown from there, saying so
        Json all = a;
        Json yes;
        yes.kind = Json::Bool;
        yes.b = true;
        all.o.insert(all.o.begin(), {"ignored", yes});
        ToolOut o = ToolFind(all);
        o.text = L"Nothing outside what .gitignore leaves out (and dependency and cache folders); in there:\n" + o.text;
        return o;
    }
    if (hidden)
        building += L"(" + Thousands(hidden) + (hidden == 1 ? L" more is" : L" more are") +
                    L" in what .gitignore leaves out, or in dependency and cache folders: ignored=true shows them.)\n";
    if (found.empty() && rows) return {L"#rows\t0\t1\treal\n", false, true};
    if (found.empty())
        return {L"Nothing named like \"" + query + L"\" " + where + L". (Index taken " + IndexAge() +
                    L"; what was created since is watched.) For text inside files, use grep.\n" + building,
                false, true};

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
        std::wstring out = L"find \"" + query + L"\" " + where + L": " + (exact ? L"" : L"about ") + Thousands(total) + (total == 1 ? L" match" : L" matches") +
                           (exact ? L"" : L"; the counts below are from the best " + Thousands(found.size()) + L" by name") + L".\n";
        auto counts = [&](const std::vector<std::pair<int, std::wstring>>& v) {
            std::wstring s;
            for (const auto& [c, k] : v)
                s += L"  " + std::wstring(std::max<int>(0, 6 - static_cast<int>(std::to_wstring(c).size())), L' ') + std::to_wstring(c) + L"  " +
                     Relative(folderName[k], scope) + L"\n";
            return s;
        };
        const auto areas = top(byArea, 10), folders = top(byFolder, 8);
        out += L"By area:\n" + counts(areas);
        if (byArea.size() > areas.size()) out += L"  … and " + Thousands(byArea.size() - areas.size()) + L" more areas\n";
        out += L"Most in one folder:\n" + counts(folders);
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
    const std::wstring typed = Lower(query);  // as Jolt keeps what was typed in Ctrl+P
    ForEach(found.size(), [&](size_t at) {
        Found& f = found[at];
        if (NoiseBelow(f.key, scopeKey)) f.boost -= 2000;  // caches, package stores, AppData: below the user's own files
        if (f.rank >= kExactName) f.why.push_back(f.rank == kExactProgram ? L"program, exact name" : L"exact name");
        else if (f.rank == kProgramName) f.why.push_back(L"program");
        if (f.percent) {
            f.why.push_back(L"close to what was typed: " + std::to_wstring(f.percent) + L"%");
            return;
        }
        uint8_t kind = 0;
        if (const int64_t t = LastActivity(f.key, &kind)) {
            const int64_t age = now - t;
            f.boost += age < kHour ? 3000 : age < kDay ? 1500 : age < 7 * kDay ? 600 : 0;
            if (age < 7 * kDay) f.why.push_back((kind == kOpened ? L"opened " : L"changed ") + Ago(t));
        }
        if (const double picked = JoltPicked(typed, f.key); picked > 0) {  // chosen in Jolt's Ctrl+P for this very text
            f.boost += static_cast<int>(std::min(5000.0, 3000.0 + 500.0 * picked));
            f.why.push_back(L"picked in Jolt for this");
        }
        if (const double h = JoltFileHeat(f.key); h > 0) {  // the file itself, opened from Jolt
            f.boost += static_cast<int>(std::min(2000.0, 600.0 + 300.0 * h));
            f.why.push_back(L"opened in Jolt");
        } else if (const double fh = JoltHeat(f.key); fh > 0) {
            f.boost += static_cast<int>(std::min(1200.0, 300.0 * fh));
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
    });
    std::stable_sort(found.begin(), found.end(), [](const Found& x, const Found& y) {
        if ((x.percent == 0) != (y.percent == 0)) return x.percent == 0;
        return x.key0 + x.boost > y.key0 + y.boost;
    });
    if (rows) {
        std::wstring out = L"#rows\t" + std::to_wstring(total) + (exact ? L"\t1\t" : L"\t0\t") + (nearOnly ? L"near" : scatteredOnly ? L"scattered" : L"real") + L"\n";
        for (const Found& f : found) out += (f.dir ? L"D\t" : L"F\t") + f.path + L"\n";
        return {out};
    }
    // gone since the index was taken: not shown, and not counted (one that can't be read is still there)
    auto exists = [](const Found& f, WIN32_FILE_ATTRIBUTE_DATA& d) { return FileInfo(f.path, d) || GetLastError() == ERROR_ACCESS_DENIED; };
    std::wstring list;
    int shown = 0, skipped = 0;  // skipped: the ones before offset
    uint64_t gone = 0;
    size_t next = 0;  // where this page ended in found
    for (; next < found.size() && shown < limit; ++next) {
        const Found& f = found[next];
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!exists(f, d)) {
            ++gone;
            continue;
        }
        if (skipped < offset) {
            ++skipped;
            continue;
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
    // Is there a next one in the list? Looked for only that far: checking a file the disk hasn't cached reads it, and
    // checking them all took most of a second on a hard drive. So the count can drop by one when a later page finds a
    // file deleted while Seek wasn't running (the ones deleted while it runs are left out already).
    bool hasNext = false;
    for (; next < found.size() && !hasNext; ++next) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (exists(found[next], d)) hasNext = true;
        else ++gone;
    }
    const uint64_t matches = total - std::min(total, gone);
    const uint64_t after = matches - std::min<uint64_t>(matches, static_cast<uint64_t>(skipped) + shown);  // after this page
    const std::wstring count = (exact ? L"" : L"about ") + Thousands(matches) + (matches == 1 ? L" match" : L" matches");
    std::wstring head = L"find \"" + query + L"\" " + where + (scope.empty() ? L"" : L" (paths relative to it, subfolders included)") + L": ";
    if (!shown) {
        if (!skipped && !offset) return {L"Nothing named like \"" + query + L"\" " + where + L" still exists (the index had some that are gone).", false, true};
        if (after) return {head + count + L"; paging goes through the best " + Thousands(kRanked) + L" by name, and offset " + std::to_wstring(offset) +
                           L" is past them. Narrow the query, add path, or use summary for counts.\n" + building};
        return {head + count + L", so offset " + std::to_wstring(offset) + L" is past the last one.\n" + building};
    }
    if (nearOnly) head += L"no name has those letters in order; these are close (typo-tolerant)";
    else if (scatteredOnly) head += L"no name has those letters together; these have them scattered along the name (" + count + L")";
    else head += count;
    if (skipped) head += L", " + std::to_wstring(skipped + 1) + L" to " + std::to_wstring(skipped + shown);
    else if (after && !nearOnly) head += L", the best " + std::to_wstring(shown);
    head += L":\n";
    std::wstring tail;
    if (after) {
        tail = L"(" + Thousands(after) + (nearOnly ? L" more close names" : L" more");
        if (hasNext) tail += L": offset=" + std::to_wstring(skipped + shown) + L" for the next ones" + (nearOnly ? L"" : L"; or narrow the query, add path, or use summary for counts");
        else tail += L" past the best " + Thousands(kRanked) + L" by name: narrow the query, add path, or use summary for counts";
        tail += L".)\n";
    }
    return {head + list + tail + building};
}

// ---- grep

// The first sign of a regex in a pattern, or empty: when the literal text matches nothing, it is tried as one, since
// an agent that wrote a|b most likely forgot regex: true. A backslash right after a letter or a digit is a path's
// (src\bin, C:\data, me\.ssh), not a sign; before a bracket or a quantifier it is one wherever it is, since no
// name starts with those in a path anyone types (var\("NAME", items\[0\]).
std::wstring RegexSign(const std::wstring& p) {
    if (p.find(L'|') != std::wstring::npos) return L"|";
    for (const wchar_t* s : {L".*", L".+"})
        if (p.find(s) != std::wstring::npos) return s;
    for (const wchar_t* s : {L"\\(", L"\\)", L"\\[", L"\\]", L"\\{", L"\\}", L"\\|", L"\\?", L"\\*", L"\\+"})
        if (p.find(s) != std::wstring::npos) return s;
    for (const wchar_t* s : {L"\\d", L"\\w", L"\\s", L"\\b", L"\\."})
        for (size_t at = p.find(s); at != std::wstring::npos; at = p.find(s, at + 1))
            if (at == 0 || !iswalnum(p[at - 1])) return s;
    if (p.size() > 1 && p[0] == L'^') return L"^";
    if (p.size() > 1 && p.back() == L'$') return L"$";
    if (const size_t o = p.find(L'['); o != std::wstring::npos && p.find(L']', o + 2) != std::wstring::npos) return L"[ ]";
    return L"";
}

// Is a drive a spinning disk? Its first reads are slow (seeks), which an answer can say. Asked once per drive.
bool HardDisk(wchar_t letter) {
    static std::mutex m;
    static int known[26] = {};  // 0 not asked, 1 a hard disk, 2 not one
    const int i = static_cast<int>(towupper(letter)) - L'A';
    if (i < 0 || i >= 26) return false;
    std::lock_guard lk(m);
    if (!known[i]) {
        known[i] = 2;
        wchar_t dev[] = L"\\\\.\\X:";
        dev[4] = static_cast<wchar_t>(L'A' + i);
        const HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY sq{StorageDeviceSeekPenaltyProperty, PropertyStandardQuery, {0}};
            DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
            DWORD got = 0;
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &sq, sizeof(sq), &d, sizeof(d), &got, nullptr) && got >= sizeof(d) && d.IncursSeekPenalty) known[i] = 1;
            CloseHandle(h);
        }
    }
    return known[i] == 1;
}

// rows: for a program that lists the matches itself (the pipe's __grep_rows): a line "#rows", the files with matches,
// the matching lines, 1 when not all of them are listed, and how it searched (asked, nocase, regex); then one line per
// match: the path, the line, the column and the line's text (a long one around the match), tab-separated.
ToolOut ToolGrep(const Json& a, bool rows = false) {
    const auto started = std::chrono::steady_clock::now();
    GrepQuery q;
    q.pattern = ArgStr(a, "pattern");
    if (q.pattern.empty()) return {L"grep needs a pattern: the text to look for.", true, false, false, L"bad_args"};
    q.root = ArgStr(a, "path");
    if (q.root.empty()) return {L"grep needs a path: the folder (or file) to search in. To find files by name anywhere, use find.", true, false, false, L"bad_args"};
    q.root = Normalize(Expand(q.root));
    if (std::wstring code, bad = CheckFolder(q.root, true, &code); !bad.empty()) return {bad, true, false, false, code};
    q.regex = ArgBool(a, "regex", false);
    q.without = ArgStr(a, "without");
    const std::wstring cs = Lower(ArgStr(a, "case"));
    bool upper = false;
    for (wchar_t c : q.pattern) upper |= iswupper(c) != 0;
    q.ignoreCase = cs == L"insensitive" || (cs != L"sensitive" && !upper);
    for (const std::wstring& given : ArgList(a, "glob"))
        for (std::wstring g : ExpandBraces(given)) {
            std::replace(g.begin(), g.end(), L'/', L'\\');
            g = Lower(g);
            if (g.size() > 1 && g[0] == L'!') q.exclude.push_back(g.substr(1));
            else q.include.push_back(g);
        }
    const int around = ArgInt(a, "context", 1, 0, 5);  // both sides; before and after set one each, further
    q.context.before = rows ? 0 : ArgInt(a, "before", around, 0, 200);
    q.context.after = rows ? 0 : ArgInt(a, "after", around, 0, 200);
    q.context.block = !rows && ArgBool(a, "block", false);
    q.perFile = rows ? ArgInt(a, "per_file", 100, 1, 10000) : ArgInt(a, "per_file", 5, 1, 200);
    q.documents = ArgBool(a, "documents", true);
    q.hidden = ArgBool(a, "hidden", false);
    q.ignored = ArgBool(a, "ignored", false);
    q.budgetMs = static_cast<uint64_t>(ArgInt(a, "time_budget_ms", 6000, 500, 60000));
    const int limit = rows ? ArgInt(a, "limit", 2000, 1, 100000) : ArgInt(a, "limit", 30, 1, 300), offset = ArgInt(a, "offset", 0, 0, 1000000);
    const bool summary = ArgBool(a, "summary", false);
    const bool isFile = !(GetFileAttributesW(LongPath(q.root).c_str()) & FILE_ATTRIBUTE_DIRECTORY);
    const std::wstring base = isFile ? ParentOf(q.root) : q.root;

    GrepResult r = RunGrep(q);
    if (!r.error.empty()) return {r.error, true, false, false, L"bad_args"};  // a regex that doesn't parse, mostly
    std::wstring note, how = L"asked";
    if (r.files.empty() && !q.ignoreCase && !q.regex) {  // the exact case said no: what ignoring it finds
        GrepQuery q2 = q;
        q2.ignoreCase = true;
        GrepResult r2 = RunGrep(q2);
        if (!r2.files.empty()) {
            note = L"No match with that exact case; ignoring case:\n";
            how = L"nocase";
            r = std::move(r2);
        }
    }
    const std::wstring sign = q.regex ? L"" : RegexSign(q.pattern);
    std::wstring regexMiss;  // what trying it as a regex said, when that found nothing either
    if (r.files.empty() && !sign.empty() && !r.timedOut) {  // the literal text said no: what it finds as a regex
        GrepQuery q2 = q;
        q2.regex = true;
        GrepResult r2 = RunGrep(q2);
        if (!r2.error.empty()) regexMiss = L"Tried as a regex too (" + sign + L" is a regex sign): " + r2.error + L"\n";
        else if (r2.files.empty()) regexMiss = L"Nothing as a regex either (" + sign + L" is a regex sign).\n";
        else {
            note = L"No match for the literal text; " + sign + L" is a regex sign, so as a regex (regex: true):\n";
            how = L"regex";
            r = std::move(r2);
        }
    }
    uint64_t lines = 0;
    for (const FileHits& f : r.files) lines += f.matches;
    if (rows) {
        std::wstring out;
        bool cut = r.timedOut;
        int n = 0;
        for (const FileHits& f : r.files) {
            cut = cut || f.matches > static_cast<int>(f.lines.size());
            for (const LineHit& h : f.lines) {
                if (n >= limit) {
                    cut = true;
                    break;
                }
                std::wstring t = h.text;
                std::replace(t.begin(), t.end(), L'\t', L' ');
                out += f.path + L"\t" + std::to_wstring(h.line) + L"\t" + std::to_wstring(h.col) + L"\t" + t + L"\n";
                ++n;
            }
        }
        return {L"#rows\t" + std::to_wstring(r.files.size()) + L"\t" + std::to_wstring(lines) + (cut ? L"\t1\t" : L"\t0\t") + how + L"\n" + out, false,
                r.files.empty(), r.timedOut};
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::wstring head = L"grep \"" + q.pattern + L"\"" + (q.without.empty() ? L"" : L" without \"" + q.without + L"\"") + L" in " + q.root + L": " + Thousands(lines) +
                        (lines == 1 ? L" matching line" : L" matching lines") +
                        L" in " + Thousands(r.files.size()) + (r.files.size() == 1 ? L" file" : L" files") + L" (searched " + Thousands(r.searched) +
                        (r.searched == 1 ? L" file" : L" files") + (r.docs ? L", " + Thousands(r.docs) + (r.docs == 1 ? L" of them a document" : L" of them documents") : L"") +
                        L" in " + Thousands(static_cast<uint64_t>(ms)) + L" ms" +
                        (r.fromMemory ? L", read from Seek's memory, not the drive"
                         : ms > 1000 && HardDisk(q.root[0]) ? L", the first read from a hard disk: the next searches here come from memory"
                                                            : L"") +
                        L").\n";
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
    if (r.big) {  // passed over for their size: named, so that a search of a log doesn't look complete when it isn't
        std::wstring names;
        for (const auto& [p, size] : r.bigFiles) names += (names.empty() ? L"" : L", ") + Relative(p, base) + L" (" + SizeText(size) + L")";
        tail += Thousands(r.big) + (r.big == 1 ? L" file over 16 MB wasn't read: " : L" files over 16 MB weren't read: ") + names +
                (r.big > r.bigFiles.size() ? L", …" : L"") + L".\n";
    }
    if (r.unreadable) {
        std::wstring names;
        for (const std::wstring& p : r.unreadableFiles) names += (names.empty() ? L"" : L", ") + Relative(p, base);
        tail += Thousands(r.unreadable) + (r.unreadable == 1 ? L" file couldn't be read" : L" files couldn't be read") +
                L" (in use by another program, or no permission): " + names + (r.unreadable > r.unreadableFiles.size() ? L", …" : L"") + L".\n";
    }
    if (r.files.empty()) {
        std::wstring out = head + tail;
        if (!r.searched && !r.timedOut && !r.big && !r.unreadable && r.noFilter.empty()) {  // nothing was even looked at: why
            const bool globs = !q.include.empty() || !q.exclude.empty();
            out += std::wstring(L"Nothing was searched: ") +
                   (isFile ? L"that file is binary" : r.binaries ? (globs ? L"every file the glob kept is binary" : L"every file there is binary")
                                                                   : (globs ? L"the glob left every file out" : L"no file there")) +
                   L".\n";
        }
        if (!sign.empty()) {  // not a file's name
            out += regexMiss.empty() ? sign + L" is a regex sign: pass regex: true to use it as one.\n" : regexMiss;
            return {out, false, true, r.timedOut};
        }
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
        return {out, false, true, r.timedOut};
    }
    if (summary) {
        std::vector<const FileHits*> v;
        for (const FileHits& f : r.files) v.push_back(&f);
        std::stable_sort(v.begin(), v.end(), [](const FileHits* x, const FileHits* y) { return x->matches > y->matches; });
        std::wstring out = note + head + L"Most matches:\n";
        for (size_t i = 0; i < v.size() && i < 40; ++i)
            out += L"  " + std::to_wstring(v[i]->matches) + L"  " + Relative(v[i]->path, base) + (v[i]->doc ? L"  (document)" : L"") + L"\n";
        if (v.size() > 40) out += L"  … and " + Thousands(v.size() - 40) + L" more files\n";
        return {out + tail, false, false, r.timedOut};
    }
    std::wstring body;
    int index = 0, shown = 0;
    for (const FileHits& f : r.files) {
        // Matches close together share their context: each line once, a matching one with ':' (and its windows) even
        // as another's context, and -- between groups that don't touch, the way ripgrep does.
        std::unordered_map<int, const LineHit*> hits;
        for (const LineHit& h : f.lines) hits[h.line] = &h;
        std::wstring block;
        int last = 0;  // the last line written for this file
        auto put = [&](int n, const std::wstring& t) {
            if (n <= last) return;
            if (last && n > last + 1 && (q.context.before || q.context.after || q.context.block)) block += L"  --\n";
            last = n;
            const auto it = hits.find(n);
            if (it == hits.end()) {
                block += L"  " + std::to_wstring(n) + L"- " + t + L"\n";
                return;
            }
            const LineHit& h = *it->second;
            block += L"  " + std::to_wstring(n) + L": " + h.text + L"\n";
            const std::wstring pad(std::to_wstring(n).size() + 4, L' ');
            for (const auto& [col, w] : h.more) block += pad + L"col " + std::to_wstring(col) + L": " + w + L"\n";
            if (h.beyond) block += pad + L"(" + std::to_wstring(h.beyond) + (h.beyond == 1 ? L" more match" : L" more matches") + L" in this line)\n";
        };
        for (const LineHit& h : f.lines) {
            if (index++ < offset) continue;
            if (shown >= limit) break;
            for (const auto& [n, t] : h.before) put(n, t);
            put(h.line, h.text);
            for (const auto& [n, t] : h.after) put(n, t);
            if (h.blockCut) block += L"  (the block goes on past " + std::to_wstring(kBlockMax) + L" lines)\n";
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
    return {note + head + body + tail, false, false, r.timedOut};
}

// ---- recent

// rows: for a program that lists them itself (the pipe's __recent_rows): a line "#rows", how many, 1 when that is all;
// then one per line: D or F, the full path, the time in Unix seconds, and created, changed, renamed or opened.
ToolOut ToolRecent(const Json& a, bool rows = false) {
    std::wstring scope = ArgStr(a, "path");
    if (!scope.empty()) {
        scope = Normalize(Expand(scope));
        if (std::wstring code, bad = CheckFolder(scope, false, &code); !bad.empty()) return {bad, true, false, false, code};
    }
    const std::wstring query = Trim(ArgStr(a, "query"));
    const NameQuery q = ParseNameQuery(query);
    const int limit = rows ? ArgInt(a, "limit", 1000, 1, 100000) : ArgInt(a, "limit", 20, 1, 200);
    const bool walk = ArgBool(a, "walk", true);  // with path, the dates on disk too
    const int64_t span = ParseSpan(ArgStr(a, "since"), 7 * kDay), from = NowFt() - span;
    const bool system = ArgBool(a, "include_system", false);
    const std::wstring scopeKey = Key(scope);
    LoadUsage();
    // Right after the background process starts: what the last run saw is read with the index, and catching up with
    // the drives' journals only starts after that. Waiting for the second alone answered from nothing.
    WaitIndex(30000);
    WaitCatchUp(10000);
    struct Item {
        std::wstring path;
        int64_t time;
        std::wstring what;
        bool folder = false;
        const wchar_t* code = L"";  // for rows
    };
    std::unordered_map<std::wstring, Item> items;
    // Build outputs and the like: what a repository's .gitignore leaves out is churn too. Finding that out reads the
    // disk (a .git, the ignore files), so the answers are kept for a minute, for every call: a program asks again
    // and again (Slate, each time a note opens), and an edited .gitignore is seen a minute later at most.
    static std::mutex ignoreM;
    static std::unique_ptr<RepoIgnore> kept;
    static uint64_t keptAt = 0;
    std::lock_guard ignoreLock(ignoreM);
    if (!kept || GetTickCount64() - keptAt > 60000) {
        kept = std::make_unique<RepoIgnore>();
        keptAt = GetTickCount64();
    }
    RepoIgnore& ignore = *kept;
    // opened: the user opened it in Jolt, so it counts even where .gitignore leaves things out
    auto add = [&](const std::wstring& path, int64_t t, const std::wstring& what, bool folder, const wchar_t* code, bool opened = false) {
        // the cheap questions first: most paths go at one of them, and only what is left reads the disk
        if (t < from) return;
        const std::wstring k = Key(path);
        if (!scope.empty() && !IsUnder(k, scopeKey)) return;
        for (const std::wstring& p : q.pathParts)
            if (k.find(p) == std::wstring::npos) return;
        for (const std::wstring& p : q.notPaths)
            if (HasPathPart(k, p)) return;
        if (!query.empty()) {
            const std::wstring leaf = LeafOf(path);
            int sc;
            std::wstring b;
            if (!NameMatch(q, leaf.c_str(), leaf.size(), folder, sc, b)) return;
        }
        if (Private(k) || (!system && NoiseBelow(k, scopeKey))) return;
        if (!system && !opened && ignore.Check(path, false)) return;
        auto it = items.find(k);
        if (it == items.end() || it->second.time < t) items[k] = {path, t, what, folder, code};
    };
    // copied out first: the filtering reads from disk (.gitignore files), and the drive watchers need the lock
    int64_t since;
    std::vector<Act> acts;
    {
        std::lock_guard lk(g_actM);
        since = g_actSince;
        for (const auto& [k, x] : g_act)
            if (x.kind != kDeleted && x.time >= from && (scope.empty() || IsUnder(k, scopeKey))) acts.push_back(x);
    }
    for (const Act& x : acts)
        add(x.path, x.time, ActName(x.kind), false,
            x.kind == kCreated ? L"created" : x.kind == kRenamed ? L"renamed" : x.kind == kOpened ? L"opened" : L"changed", x.kind == kOpened);
    std::vector<Use> uses;
    {
        std::lock_guard lk(g_useM);
        uses = g_use;
    }
    for (const Use& u : uses) add(u.path, u.last, u.file ? L"opened in Jolt" : L"folder opened in Jolt", !u.file, L"opened", true);
    bool walked = false, walkCut = false;
    if (!scope.empty() && walk) {  // the dates on disk too: also what changed before Seek was watching
        walked = true;
        const uint64_t deadline = GetTickCount64() + 3000;
        WalkTree(scope, deadline, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
            const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (dir)
                return !JunkDirs().count(Lower(fd.cFileName)) && !Private(Key(p)) && Key(p) != Key(DataDir()) &&  // not Seek's own files
                       (system || !ignore.Check(p, true));
            add(p, Ft(fd.ftLastWriteTime), L"modified", false, L"changed");
            return false;
        });
        walkCut = GetTickCount64() > deadline;
    }
    std::vector<Item> v;
    v.reserve(items.size());
    for (auto& [k, it] : items) v.push_back(std::move(it));
    std::sort(v.begin(), v.end(), [](const Item& x, const Item& y) { return x.time > y.time; });
    if (rows) {
        std::wstring out;
        size_t i = 0;
        int n = 0;
        for (; i < v.size() && n < limit; ++i) {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (!FileInfo(v[i].path, d)) continue;  // gone
            const bool dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (dir && !v[i].folder) continue;
            out += (dir ? L"D\t" : L"F\t") + v[i].path + L"\t" + std::to_wstring((v[i].time - 116444736000000000LL) / 10000000) + L"\t" + v[i].code + L"\n";
            ++n;
        }
        const bool all = i == v.size();  // every one was looked at: the count is what is listed
        return {L"#rows\t" + std::to_wstring(all ? static_cast<size_t>(n) : v.size()) + (all && !walkCut ? L"\t1\n" : L"\t0\n") + out, false, n == 0, walkCut};
    }
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
                (scope.empty() ? L"Seek knows what changed while it was running, and what the drives' change journals held when it started. Pass path to read dates from disk.\n" : L""),
                false, true, walkCut};
    std::wstring tail;
    if (v.size() > static_cast<size_t>(shown)) tail = L"(more: raise limit, narrow with query or path.)\n";
    if (!system) tail += L"System places (AppData, Windows, Program Files, temp, caches) and what .gitignore leaves out are left out; include_system shows them.\n";
    return {head + L":\n" + list + notes + L"\n" + tail, false, false, walkCut};
}

// ---- status: what the background process knows, for seek --status

std::atomic<uint64_t> g_calls{0}, g_lastCall{0};  // tool calls served, and the tick of the last one
const uint64_t g_started = GetTickCount64();

std::wstring AgoTick(uint64_t tick) { return Ago(NowFt() - static_cast<int64_t>(GetTickCount64() - tick) * 10000); }

// The index of each drive, for a program to show while the first one is made (the pipe's __status_rows): a line
// "#rows", this version and how many drives; then one per drive: its letter, loading (the background process has just
// started and is reading the indexes), indexing (the first index is being made: the names so far), ready or none,
// how many names, jolt or own (whose index it is), and when it was taken in Unix seconds (0 when there is none yet).
std::wstring StatusRows() {
    const std::vector<wchar_t> drives = FixedDrives();
    const auto vols = Volumes();
    std::wstring out = L"#rows\t" SEEK_VERSION L"\t" + std::to_wstring(drives.size()) + L"\n";
    for (const wchar_t c : drives) {
        std::shared_ptr<const Volume> v;
        for (const auto& x : vols)
            if (x->letter == c) v = x;
        std::wstring state = L"none", source = L"own";
        uint64_t names = 0;
        int64_t taken = 0;
        if (!g_indexReady) {
            state = L"loading";
        } else if (g_indexing & (1u << (c - L'A'))) {
            state = L"indexing";
            names = g_indexed[c - L'A'];
        } else if (v) {
            state = L"ready";
            names = v->count;
            taken = (v->stamp - 116444736000000000LL) / 10000000;
        }
        if (JoltHas(c)) source = L"jolt";
        out += std::wstring(1, c) + L"\t" + state + L"\t" + std::to_wstring(names) + L"\t" + source + L"\t" + std::to_wstring(std::max<int64_t>(0, taken)) + L"\n";
    }
    return out;
}

std::wstring StatusText() {
    std::wstring out = L"Seek " SEEK_VERSION L", background process " + std::to_wstring(GetCurrentProcessId()) + L", started " + AgoTick(g_started) + L", " +
                       Thousands(g_calls) + (g_calls == 1 ? L" call served" : L" calls served") + (g_calls ? L", the last " + AgoTick(g_lastCall) : L"") + L".\n";
    out += L"Its files: " + DataDir() + L"  (" + (g_rules.log ? L"log.txt has every call" : L"log off in seek.txt") + L")\nSettings: " + SettingsFile() +
           (GetFileAttributesW(SettingsFile().c_str()) == INVALID_FILE_ATTRIBUTES ? L" (none: the defaults)" : L"") + L"\n";
    out += L"Index:\n";
    const auto vols = Volumes();
    const std::vector<wchar_t> drives = FixedDrives();
    for (const wchar_t c : drives) {
        std::wstring line = L"  " + std::wstring(1, c) + L":  ";
        std::shared_ptr<const Volume> v;
        for (const auto& x : vols)
            if (x->letter == c) v = x;
        if (g_indexing & (1u << (c - L'A'))) line += L"being indexed for the first time, " + Thousands(g_indexed[c - L'A']) + L" names so far";
        else if (!v) line += L"no index";
        else line += Thousands(v->count) + L" names, " + (JoltHas(c) ? L"from Jolt's index" : L"Seek's own (walked again daily)") + L", taken " + Ago(v->stamp);
        out += line + L"\n";
    }
    {
        std::lock_guard lk(g_actM);
        out += L"Since the indexes were taken: " + Thousands(g_added.size()) + L" names created (the ones deleted again left out), " + Thousands(g_removed.size()) +
               L" deleted; past " + Thousands(kSinceMax) + L" of either, the oldest go. Changes remembered: " + Thousands(g_act.size()) + L", watched since " +
               DateText(g_actSince) + L", saved to activity.txt every 5 min.\n";
    }
    std::wstring caught;
    for (const wchar_t c : drives)
        if (const int64_t from = g_journalFrom[c - L'A'])
            caught += std::wstring(caught.empty() ? L"" : L", ") + c + L": " + Thousands(g_caughtUp[c - L'A']) +
                      (g_journalWhole[c - L'A'] ? L" (all of its journal, back to " + DateText(from) + L")" : L" (from where the last run left it, " + DateText(from) + L")");
    out += g_catchingUp ? L"Catching up with what was made while no background process watched, from the drives' change journals.\n"
           : caught.empty() ? L""
                            : L"Made while no background process watched, found at its start in the drives' change journals: " + caught + L".\n";
    std::wstring watching, missing, lost;
    for (const wchar_t c : drives) {
        ((g_watchAlive & (1u << (c - L'A'))) ? watching : missing) += std::wstring(1, c) + L": ";
        if (const uint32_t n = g_lostBatches[c - L'A']) lost += std::wstring(1, c) + L": " + std::to_wstring(n) + L"  ";
    }
    out += L"Drive watchers: " + (watching.empty() ? L"none" : L"on " + watching) + (missing.empty() ? L"" : L"; not on " + missing + L"(tried again every 5 min)") +
           (lost.empty() ? L"" : L"; batches of changes lost (too many at once): " + lost) + L"\n";
    {
        std::lock_guard lk(g_cacheM);
        uint64_t total = 0;
        for (const auto& f : g_folders) total += f->bytes;
        out += L"Folders in memory for grep: " + std::to_wstring(g_folders.size()) + L", " + SizeText(total) + L" of " + SizeText(CacheBudget()) + L".\n";
        for (const auto& f : g_folders) {
            std::wstring line = L"  " + f->root + L"  ";
            if (!f->ready) line += L"being read";
            else {
                std::shared_lock sl(f->m, std::try_to_lock);
                line += (sl.owns_lock() ? Thousands(f->files.size()) + L" files, " : L"") + SizeText(f->bytes) + L", used " + AgoTick(f->used) +
                        (f->stale ? L", out of date: read again on the next search" : f->dirty.empty() ? L"" : L", " + Thousands(f->dirty.size()) + L" changes to apply");
            }
            out += line + L"\n";
        }
    }
    std::wstring jolt = L"not used (jolt = no in seek.txt, or SEEK_NO_JOLT)";
    if (g_rules.jolt) {
        std::lock_guard lk(g_useM);
        jolt = g_use.empty() ? L"not found; Seek keeps its own indexes"
                             : L"what it saw used ranks the results: " +
                                   Thousands(static_cast<uint64_t>(std::count_if(g_use.begin(), g_use.end(), [](const Use& u) { return !u.file; }))) + L" folders, " +
                                   Thousands(g_useFiles.size()) +
                                   L" files, and what was picked in Ctrl+P for " + Thousands(g_picks.size()) + L" texts";
    }
    return out + L"Jolt: " + jolt + L"\n";
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

// ---- stats: what agents asked, by day and tool, kept past the log's rotation (stats.txt), for seek --stats. The
// times are counted in steps, which is enough to tell a median of 15 ms from one of 30.

constexpr double kStatBounds[] = {1, 2, 3, 5, 7, 10, 15, 20, 30, 50, 70, 100, 150, 200, 300, 500, 700, 1000, 1500, 2000, 3000, 5000, 10000};  // ms: each step's upper edge
constexpr int kStatBuckets = static_cast<int>(std::size(kStatBounds)) + 1;  // and one for what is past them
struct Stat {
    uint64_t calls = 0, errors = 0;
    int64_t nothing = 0, cut = 0;  // -1: not known (a day taken from the log, which doesn't say)
    uint64_t bucket[kStatBuckets] = {};
};
std::mutex g_statM;
std::map<std::pair<std::string, std::string>, Stat> g_stats;  // by day ("2026-11-09") and tool
bool g_statsDirty = false;

// The command line and Seek's own tests and benchmark aren't what agents asked.
bool StatCounts(const std::wstring& client) { return client != L"command line" && client != L"bench" && client.rfind(L"seek-", 0) != 0; }

void StatAdd(const std::string& day, const std::string& tool, double ms, bool error, int nothing, int cut) {
    if (tool != "find" && tool != "grep" && tool != "recent") return;
    std::lock_guard lk(g_statM);
    Stat& s = g_stats[{day, tool}];
    ++s.calls;
    s.errors += error;
    if (nothing < 0 || s.nothing < 0) {
        s.nothing = s.cut = -1;
    } else {
        s.nothing += nothing;
        s.cut += cut;
    }
    int b = 0;
    while (b < kStatBuckets - 1 && ms > kStatBounds[b]) ++b;
    ++s.bucket[b];
    g_statsDirty = true;
}

std::wstring StatsFile() { return DataDir() + L"\\stats.txt"; }

void StatsSave() {
    std::string out = "# Seek: what agents asked, by day and tool: calls, errors, found nothing, cut by the time budget (-1: not known), and how\n"
                      "# many took up to 1, 2, 3, 5, 7, 10, 15, 20, 30, 50, 70, 100, 150, 200, 300, 500, 700, 1000, 1500, 2000, 3000, 5000, 10000 ms,\n"
                      "# and more. Rewritten automatically; seek --stats shows it.\n";
    {
        std::lock_guard lk(g_statM);
        if (!g_statsDirty) return;
        g_statsDirty = false;
        for (const auto& [k, s] : g_stats) {
            out += k.first + "\t" + k.second + "\t" + std::to_string(s.calls) + "\t" + std::to_string(s.errors) + "\t" + std::to_string(s.nothing) + "\t" +
                   std::to_string(s.cut) + "\t";
            for (int b = 0; b < kStatBuckets; ++b) out += (b ? " " : "") + std::to_string(s.bucket[b]);
            out += "\n";
        }
    }
    const std::wstring f = StatsFile(), tmp = f + L".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << out;
    }
    MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// From stats.txt. The first time there is none: then from what the log still has, which doesn't say what found
// nothing or was cut.
void StatsLoad() {
    std::ifstream in(StatsFile(), std::ios::binary);
    std::string line;
    if (in) {
        std::lock_guard lk(g_statM);
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const std::vector<std::string> f = Fields(line);
            if (f.size() < 7) continue;
            Stat s;
            s.calls = _strtoui64(f[2].c_str(), nullptr, 10);
            s.errors = _strtoui64(f[3].c_str(), nullptr, 10);
            s.nothing = _strtoi64(f[4].c_str(), nullptr, 10);
            s.cut = _strtoi64(f[5].c_str(), nullptr, 10);
            const char* p = f[6].c_str();
            for (int b = 0; b < kStatBuckets && *p; ++b) {
                char* end = nullptr;
                s.bucket[b] = _strtoui64(p, &end, 10);
                p = end;
            }
            g_stats[{f[0], f[1]}] = s;
        }
        return;
    }
    for (const wchar_t* name : {L"\\log.old.txt", L"\\log.txt"}) {
        std::ifstream log(DataDir() + name, std::ios::binary);
        while (std::getline(log, line)) {  // "2026-11-08 18:32  client  find {...}  ->  ok, 12 ms"
            const size_t arrow = line.rfind("  ->  ");
            if (line.size() < 20 || arrow == std::string::npos) continue;
            size_t at = std::string::npos;
            std::string tool;
            for (const char* t : {"find", "grep", "recent"})
                if (const size_t p = line.find(std::string("  ") + t + " {", 16); p < at) {
                    at = p;
                    tool = t;
                }
            if (at == std::string::npos || at < 18 || !StatCounts(Wide(line.substr(18, at - 18)))) continue;
            const std::string res = line.substr(arrow + 6);  // "ok, 1,234 ms"
            const size_t comma = res.find(", ");
            if (comma == std::string::npos) continue;
            std::string num;
            for (const char c : res.substr(comma + 2)) {
                if (isdigit(static_cast<unsigned char>(c)) || c == '.') num += c;
                else if (c != ',') break;
            }
            StatAdd(line.substr(0, 10), tool, atof(num.c_str()), res.rfind("error", 0) == 0, -1, -1);
        }
    }
}

// The last `days` days with calls, a row per tool, and their total.
std::wstring StatsText(int days) {
    std::lock_guard lk(g_statM);
    if (g_stats.empty()) return L"Seek " SEEK_VERSION L": no calls from agents counted yet. (" + StatsFile() + L")\n";
    std::vector<std::string> shown;  // the days, the newest first
    for (auto it = g_stats.rbegin(); it != g_stats.rend(); ++it)
        if (shown.empty() || shown.back() != it->first.first) {
            if (static_cast<int>(shown.size()) == days) break;
            shown.push_back(it->first.first);
        }
    auto cell = [](const std::wstring& s, size_t w) { return std::wstring(w > s.size() ? w - s.size() : 0, L' ') + s; };
    auto within = [](const Stat& s, double share) -> std::wstring {  // the step that share of the calls fit in
        const uint64_t need = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(static_cast<double>(s.calls) * share)));
        uint64_t seen = 0;
        for (int b = 0; b < kStatBuckets; ++b)
            if ((seen += s.bucket[b]) >= need) return b < kStatBuckets - 1 ? Thousands(static_cast<uint64_t>(kStatBounds[b])) : L">10,000";
        return L"?";
    };
    auto row = [&](const std::wstring& day, const std::string& tool, const Stat& s) {
        return day + std::wstring(12 - day.size(), L' ') + Wide(tool) + std::wstring(8 - tool.size(), L' ') + cell(Thousands(s.calls), 6) + cell(within(s, 0.5), 8) +
               cell(within(s, 0.95), 8) + cell(s.nothing < 0 ? L"-" : Thousands(static_cast<uint64_t>(s.nothing)), 9) +
               cell(s.cut < 0 ? L"-" : Thousands(static_cast<uint64_t>(s.cut)), 6) + cell(Thousands(s.errors), 8) + L"\n";
    };
    std::wstring out = L"Seek " SEEK_VERSION L": what agents asked, the last " + std::to_wstring(shown.size()) + (shown.size() == 1 ? L" day" : L" days") +
                       L" with calls. Times are the tool's own, in ms: half of the calls took at most `median`, 95 of 100 at most `p95`.\n" +
                       L"day         tool     calls  median     p95  nothing   cut  errors\n";
    std::map<std::string, Stat> total;
    for (const std::string& day : shown) {
        bool first = true;
        for (const char* tool : {"find", "grep", "recent"}) {
            const auto it = g_stats.find({day, tool});
            if (it == g_stats.end()) continue;
            out += row(first ? Wide(day) : L"", tool, it->second);
            first = false;
            Stat& t = total[tool];
            t.calls += it->second.calls;
            t.errors += it->second.errors;
            if (it->second.nothing < 0 || t.nothing < 0) t.nothing = t.cut = -1;
            else t.nothing += it->second.nothing, t.cut += it->second.cut;
            for (int b = 0; b < kStatBuckets; ++b) t.bucket[b] += it->second.bucket[b];
        }
    }
    if (shown.size() > 1) {
        bool first = true;
        for (const char* tool : {"find", "grep", "recent"})
            if (const auto it = total.find(tool); it != total.end()) {
                out += row(first ? L"all of them" : L"", tool, it->second);
                first = false;
            }
    }
    return out + L"nothing: found nothing. cut: stopped at the time budget, with part of the answer. -: not known, for days taken from the log.\n"
                 L"The command line and Seek's own tests and benchmark aren't counted. Kept in " + StatsFile() + L"\n";
}

// ---- sizes

// How much space the folders and files below a folder take: the biggest folders a few levels down, and the biggest
// files. The index keeps names, not sizes, so this reads the disk, on several threads, as Seek walks a drive for an
// index of its own. Links and junctions aren't followed (what they point to is counted where it is), private places
// aren't looked into, and files only in the cloud aren't counted: they take no room on this disk.
ToolOut ToolSizes(const Json& a) {
    std::wstring root = ArgStr(a, "path");
    if (root.empty()) return {L"sizes needs a path: a folder, or a drive like C:\\.", true, false, false, L"bad_args"};
    root = Normalize(Expand(root));
    std::wstring code;
    if (std::wstring bad = CheckFolder(root, false, &code); !bad.empty()) return {bad, true, false, false, code};
    const int depthMax = ArgInt(a, "depth", 3, 1, 12);
    const int limit = ArgInt(a, "limit", 15, 1, 200);
    const size_t filesMax = static_cast<size_t>(ArgInt(a, "files", 10, 0, 200));
    const uint64_t budgetMs = static_cast<uint64_t>(ArgInt(a, "time_budget_ms", 60000, 1000, 600000));
    const auto started = std::chrono::steady_clock::now();
    const uint64_t deadline = GetTickCount64() + budgetMs;

    struct Dir {
        uint32_t parent;
        int depth;
        std::wstring path;
        uint64_t bytes = 0, files = 0;  // its own files; then, below, everything under it
    };
    struct Big {
        uint64_t size;
        std::wstring path;
    };
    std::vector<Dir> dirs{{UINT32_MAX, 0, root}};
    std::vector<Big> big;  // the biggest files, a min-heap on size
    auto bigger = [](const Big& x, const Big& y) { return x.size > y.size; };
    uint64_t privateDirs = 0, unreadable = 0, links = 0, cloudFiles = 0, cloudBytes = 0;
    bool cut = false;
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint32_t> todo{0};
    unsigned busy = 0;  // threads listing a folder; none, with nothing left, is the end
    auto work = [&] {
        struct Entry {
            std::wstring name;
            DWORD attr;
            uint64_t size;
        };
        std::vector<Entry> got;
        WIN32_FIND_DATAW fd;
        for (;;) {
            uint32_t di;
            std::wstring path;
            {
                std::unique_lock lk(m);
                cv.wait(lk, [&] { return !todo.empty() || busy == 0; });
                if (todo.empty()) break;
                if (GetTickCount64() > deadline) {  // what was walked is answered; the rest isn't
                    cut = true;
                    todo.clear();
                    cv.notify_all();
                    if (busy == 0) break;
                    continue;
                }
                di = todo.back();
                todo.pop_back();
                path = dirs[di].path;
                ++busy;
            }
            got.clear();
            const std::wstring pattern = LongPath(path) + (path.back() == L'\\' ? L"*" : L"\\*");
            HANDLE f = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            const bool failed = f == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND;
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
                    got.push_back({fd.cFileName, fd.dwFileAttributes, (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow});
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            {
                std::lock_guard lk(m);
                if (failed) ++unreadable;
                for (const Entry& e : got) {
                    if (e.attr & FILE_ATTRIBUTE_DIRECTORY) {
                        if (e.attr & FILE_ATTRIBUTE_REPARSE_POINT) {
                            ++links;
                            continue;
                        }
                        std::wstring p = Join(path, e.name);
                        if (Private(Key(p))) {
                            ++privateDirs;
                            continue;
                        }
                        todo.push_back(static_cast<uint32_t>(dirs.size()));
                        dirs.push_back({di, dirs[di].depth + 1, std::move(p)});
                        continue;
                    }
                    if (e.attr & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) {
                        ++cloudFiles;  // a placeholder: its size is the file's in the cloud, not room taken here
                        cloudBytes += e.size;
                        continue;
                    }
                    dirs[di].bytes += e.size;
                    ++dirs[di].files;
                    if (filesMax && (big.size() < filesMax || e.size > big.front().size)) {
                        std::wstring p = Join(path, e.name);
                        if (Private(Key(p))) continue;  // counted, not named
                        if (big.size() == filesMax) {
                            std::pop_heap(big.begin(), big.end(), bigger);
                            big.pop_back();
                        }
                        big.push_back({e.size, std::move(p)});
                        std::push_heap(big.begin(), big.end(), bigger);
                    }
                }
                --busy;
            }
            cv.notify_all();
        }
    };
    {
        std::vector<std::thread> threads;
        for (unsigned i = 0, n = std::max(2u, std::min(8u, std::thread::hardware_concurrency())); i < n; ++i) threads.emplace_back(work);
        for (auto& t : threads) t.join();
    }
    // A folder's size is its files' and its subfolders': a subfolder is always listed after its parent, so from the
    // end every folder is whole before it is added to its parent.
    for (size_t i = dirs.size(); i-- > 1;) {
        dirs[dirs[i].parent].bytes += dirs[i].bytes;
        dirs[dirs[i].parent].files += dirs[i].files;
    }
    const uint64_t total = dirs[0].bytes;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    auto share = [&](uint64_t b) {
        if (!total) return std::wstring(L"0%");
        const double p = 100.0 * static_cast<double>(b) / static_cast<double>(total);
        wchar_t buf[16];
        swprintf_s(buf, p < 10 ? L"%.1f%%" : L"%.0f%%", p);
        return std::wstring(buf);
    };
    auto pad = [](std::wstring s, size_t w) { return s.size() < w ? std::wstring(w - s.size(), L' ') + s : s; };

    wchar_t took[32];
    swprintf_s(took, secs < 10 ? L"%.1f s" : L"%.0f s", secs);
    std::wstring out = L"sizes under " + root + L": " + SizeText(total) + L" in " + Thousands(dirs[0].files) + (dirs[0].files == 1 ? L" file" : L" files") + L" and " +
                       Thousands(dirs.size() - 1) + (dirs.size() == 2 ? L" folder" : L" folders") + L" (read from the disk in " + took + L"; paths below it).\n";
    if (root.size() == 3) {  // a drive: how much of what it has in use was accounted for
        ULARGE_INTEGER free{}, size{};
        if (GetDiskFreeSpaceExW(root.c_str(), nullptr, &size, &free) && size.QuadPart >= free.QuadPart) {
            const uint64_t used = size.QuadPart - free.QuadPart;
            out += root.substr(0, 2) + L" has " + SizeText(used) + L" in use";
            if (used > total) out += L"; the other " + SizeText(used - total) + L" is in what couldn't be read or wasn't looked into (below), and in the file system's own files";
            out += L".\n";
        }
    }
    // A folder that is all but one of its subfolders (Users, and Users\me) says nothing that subfolder doesn't: it isn't
    // listed, and it isn't a level either, so depth 3 reaches as far below Users\me as below Windows.
    std::vector<uint64_t> biggestChild(dirs.size(), 0);
    for (size_t i = 1; i < dirs.size(); ++i) biggestChild[dirs[i].parent] = std::max(biggestChild[dirs[i].parent], dirs[i].bytes);
    auto passThrough = [&](size_t i) { return dirs[i].bytes && biggestChild[i] * 100 >= dirs[i].bytes * 99; };
    std::vector<int> level(dirs.size(), 0);  // parents come before their subfolders
    for (size_t i = 1; i < dirs.size(); ++i) level[i] = level[dirs[i].parent] + (dirs[i].parent != 0 && passThrough(dirs[i].parent) ? 0 : 1);
    std::vector<uint32_t> order;
    for (uint32_t i = 1; i < dirs.size(); ++i)
        if (level[i] <= depthMax && !passThrough(i)) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return dirs[x].bytes > dirs[y].bytes; });
    if (order.size() > static_cast<size_t>(limit)) order.resize(static_cast<size_t>(limit));
    if (!order.empty()) {
        out += L"Biggest folders, up to " + std::to_wstring(depthMax) + (depthMax == 1 ? L" level" : L" levels") +
               L" down (a folder that is all one subfolder isn't listed, nor counted as a level):\n";
        for (uint32_t i : order)
            out += L"  " + pad(SizeText(dirs[i].bytes), 9) + L"  " + pad(share(dirs[i].bytes), 5) + L"  " + Relative(dirs[i].path, root) + L"\\  (" +
                   Thousands(dirs[i].files) + (dirs[i].files == 1 ? L" file)\n" : L" files)\n");
    }
    if (!big.empty()) {
        std::sort(big.begin(), big.end(), [](const Big& x, const Big& y) { return x.size > y.size; });
        out += L"Biggest files:\n";
        for (const Big& b : big) out += L"  " + pad(SizeText(b.size), 9) + L"  " + Relative(b.path, root) + L"\n";
    }
    if (cut) out += L"(Stopped at the time budget (" + std::to_wstring(budgetMs / 1000) + L" s): the sizes are of what was read by then, so they are low. Raise time_budget_ms, or give a folder further down.)\n";
    if (unreadable) out += L"(" + Thousands(unreadable) + (unreadable == 1 ? L" folder" : L" folders") + L" couldn't be read (no permission, as system folders): not counted.)\n";
    if (privateDirs) out += L"(" + Thousands(privateDirs) + (privateDirs == 1 ? L" private folder" : L" private folders") + L" (keys, browser profiles, password managers…) weren't looked into: not counted.)\n";
    if (links) out += L"(" + Thousands(links) + (links == 1 ? L" link or junction wasn't" : L" links and junctions weren't") + L" followed: what they point to is counted where it is.)\n";
    if (cloudFiles) out += L"(" + Thousands(cloudFiles) + (cloudFiles == 1 ? L" file is" : L" files are") + L" only in the cloud: their " + SizeText(cloudBytes) + L" take no room here, and aren't counted.)\n";
    return {out, false, total == 0, cut};
}

ToolOut RunTool(const std::string& name, const Json& args, const std::wstring& client) {
    const auto t0 = std::chrono::steady_clock::now();
    ++g_calls;
    g_lastCall = GetTickCount64();
    ToolOut out;
    try {
        if (name == "find") out = ToolFind(args);
        else if (name == "grep") out = ToolGrep(args);
        else if (name == "recent") out = ToolRecent(args);
        else if (name == "sizes") out = ToolSizes(args);
        else out = {L"Unknown tool: " + Wide(name) + L". The tools are find, grep, recent and sizes.", true, false, false, L"unknown_command"};
    } catch (const std::exception& e) {
        out = {L"Seek failed: " + Wide(e.what()), true};
    }
    std::string argText;
    JsonWrite(args, argText);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Audit((client.empty() ? L"?" : client) + L"  " + Wide(name) + L" " + Wide(argText) + L"  ->  " + (out.error ? L"error" : L"ok") + L", " + Ms(ms));
    if (g_watching && StatCounts(client)) StatAdd(Utf8(DateText(NowFt()).substr(0, 10)), name, ms, out.error, out.nothing, out.cut);  // in the background process
    return out;
}
