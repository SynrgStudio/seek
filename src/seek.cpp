// Seek: fast file search for AI agents, as an MCP server for Windows.
//  - find: names anywhere on the PC in milliseconds, from a live index of every fixed drive (the one Jolt keeps), with
//    the same fuzzy, typo-tolerant matching as Jolt; ranked by exact name, what the user changed lately, the folders
//    they use, and closeness to a folder.
//  - grep: text inside the files of a folder, also PDF/Office documents through Windows' text filters.
//  - recent: what the user is working on, from the drives' changes seen live.
// Read-only. Private places (keys, browser profiles, password stores) are never listed or searched, and every call
// is logged for the user to see. See README.md.

#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <filter.h>
#include <filterr.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

#define SEEK_VERSION "0.1.0"
const char* const kBuild = SEEK_VERSION " " __DATE__ " " __TIME__;  // a daemon from another build makes room

#include "../../src/fuzzy.inl"  // the same matching as Jolt: fuzzy, exact names and programs first, typos with a percentage
#include "json.inl"
#include "util.inl"
#include "index.inl"
#include "activity.inl"
#include "grep.inl"
#include "tools.inl"
#include "server.inl"

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2) {
        const std::wstring a = argv[1];
        if (a == L"--daemon") return DaemonMain();
        if (a == L"--cli") return CliMain(argc, argv, false);
        if (a == L"--call") return CliMain(argc, argv, true);
        if (a == L"--stop") return StopMain();
        if (a == L"--version") {
            Print(L"seek " + Wide(SEEK_VERSION) + L"\n");
            return 0;
        }
        if (a == L"--help" || a == L"-h" || a == L"/?") {
            Print(L"Seek " + Wide(SEEK_VERSION) +
                  L": fast file search for AI agents (MCP over stdio).\n"
                  L"  seek                         MCP server: what an agent runs\n"
                  L"  seek --cli <tool> [json]     run a tool here: find, grep or recent\n"
                  L"  seek --call <tool> [json]    the same, through the shared background process\n"
                  L"  seek --stop                  stop the background process\n"
                  L"Example: seek --cli find \"{\\\"query\\\":\\\"report .pdf\\\"}\"\n");
            return 0;
        }
    }
    return McpMain();
}
