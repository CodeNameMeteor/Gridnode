// Settings objects and the engine console.
//
// Settings: every object the engine's SettingsManager holds, read from its by-type hash map
// (db/annotations/console.yaml). Nothing is called; the map is only read. All 3,431 fields of the
// 135 settings classes are reachable this way, not only the 141 the console binds.
//
// Console: Console_executeCommand with trusted = 1, the way the engine runs its config files, so
// every registered command works. Commands run on the client thread.
//
// Online play: settings classes and command groups that reach EA's services or the network are
// flagged (onlineWarning). The menu asks the player for a confirmation. A mod's online console
// commands are refused until it calls online_allow (api.cpp consoleRunFor), then each is logged;
// settings objects are raw memory, so a mod that takes an online one without it is warned instead.

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;

// ---- online play -----------------------------------------------------------------------------
struct Online {
    const char* prefix; // settings class name without "Settings", or console group
    bool loose;         // also any longer name starting with it (OnlineDebug, BlazeClient...)
    const char* why;
};
const Online kOnline[] = {
    {"NetworkCore", false, "network transport used by online play"},
    {"Network", false, "network settings used by online play"},
    {"Online", true, "EA online services (Blaze)"},
    {"Origin", true, "EA app / Origin integration"},
    {"Server", false, "the game server's settings (the local server also talks to online services)"},
    {"SyncedGame", false, "settings synchronised with the server"},
    {"PamEntitlement", false, "entitlements (owned content) from EA's services"},
    {"PamSocialPlay", false, "social play: time trials, beat leaderboards, friends"},
    {"PamStats", false, "stats reported online"},
    {"PamProgression", false, "progression stored in your online profile"},
    {"PamReward", false, "rewards stored in your online profile"},
    {"Terminator", true, "EA online service connection"},
    {"WebBrowser", false, "the in-game web browser (EA pages)"},
    {"Blaze", true, "EA online services (Blaze)"},
    {"Matchmaking", true, "matchmaking"},
    {"Presence", true, "online presence"},
};

// name: a settings class ("NetworkSettings") or a console group / command ("Network", "Online.Login").
const char* onlineFor(const std::string& name) {
    for (const Online& o : kOnline) {
        const size_t n = strlen(o.prefix);
        if (name.compare(0, n, o.prefix) != 0) continue;
        const std::string rest = name.substr(n);
        if (rest.empty() || rest[0] == '.' || rest == "Settings" || o.loose) return o.why;
    }
    return nullptr;
}

// ---- settings -----------------------------------------------------------------------------------
std::mutex g_setM;
std::vector<SettingsEntry> g_settings; // guarded by g_setM
DWORD g_settingsAt = 0;
size_t g_settingsLogged = 0; // the count last logged

std::vector<SettingsEntry> scanSettings() {
    std::vector<SettingsEntry> out;
    if (!build::codeMatches("SettingsManager_load")) return out;
    using namespace e::st;
    const uintptr_t mgr = mem::readOr<uintptr_t>(mem::rva(e::global::SettingsManager));
    if (!mgr) return out;
    const uintptr_t buckets = mem::readOr<uintptr_t>(mgr + SettingsManager::off::byTypeBuckets);
    const uint32_t n = mem::readOr<uint32_t>(mgr + SettingsManager::off::byTypeBucketCount);
    if (!buckets || !n || n > 65536) return out;
    std::vector<uintptr_t> heads(n);
    if (!mem::read(buckets, heads.data(), n * sizeof(uintptr_t))) return out;
    for (uintptr_t node : heads) {
        for (int guard = 0; node && guard < 1024; ++guard) {
            uintptr_t rec[3] = {};
            if (!mem::read(node, rec, sizeof(rec))) break;
            const reflect::Type* t = registry().byTi(rec[0]);
            const uintptr_t obj = rec[1];
            if (t && obj) {
                const reflect::Type* own = registry().typeOf(mem::ProcessMemory(), obj);
                if (own && registry().isKindOf(own, t)) out.push_back({t, obj, onlineFor(t->name)});
            }
            node = rec[2];
        }
    }
    std::sort(out.begin(), out.end(), [](const SettingsEntry& a, const SettingsEntry& b) { return a.type->name < b.type->name; });
    static bool reportedEmpty = false;
    if (out.empty() && !reportedEmpty) {
        reportedEmpty = true;
        GN_LOG("settings: none found (manager %llx, buckets %llx, %u buckets)", static_cast<unsigned long long>(mgr),
               static_cast<unsigned long long>(buckets), n);
    }
    return out;
}

// ---- console ----------------------------------------------------------------------------------
struct EngineString {
    char* begin;
    char* end;
    char* capacity;
    void* allocator;
};

bool consoleOk() {
    static const int ok = [] {
        const bool all = build::codeMatches("Console_executeCommand") && build::codeMatches("EngineAllocator_free") &&
                         build::codeMatches("ConsoleRegistry_getStaticCommands") &&
                         build::codeMatches("ConsoleRegistry_getBoundCommands");
        if (!all) GN_LOG("console: the console functions do not look as expected; the console is off");
        return all ? 1 : 0;
    }();
    return ok == 1;
}

std::mutex g_cmdM;
std::vector<ConsoleCommand> g_commands; // guarded by g_cmdM
std::set<std::string> g_strings;        // guarded by g_cmdM: interned, never freed (gn_command points here)
size_t g_seenStatic = 0, g_seenBound = 0;

const char* intern(const std::string& s) { return g_strings.insert(s).first->c_str(); }

std::string cstr(uintptr_t p) {
    std::string s;
    if (p) mem::readCString(p, s, 512);
    return s;
}

} // namespace

const char* onlineWarning(const std::string& name) { return onlineFor(name); }

std::vector<SettingsEntry> settingsList() {
    std::lock_guard<std::mutex> lock(g_setM);
    if (g_settings.empty() || GetTickCount() - g_settingsAt > 1000) {
        g_settings = scanSettings();
        g_settingsAt = GetTickCount();
        if (g_settings.size() != g_settingsLogged) {
            g_settingsLogged = g_settings.size();
            size_t online = 0;
            for (const SettingsEntry& s : g_settings) online += s.online ? 1 : 0;
            GN_LOG("settings: %llu settings objects (%llu marked as online)", static_cast<unsigned long long>(g_settings.size()),
                   static_cast<unsigned long long>(online));
        }
    }
    return g_settings;
}

uintptr_t settingsFind(const std::string& className) {
    for (const SettingsEntry& s : settingsList())
        if (s.type->name == className) return s.object;
    return 0;
}

bool consoleAvailable() { return consoleOk(); }

// Client thread. trusted = 1, as for the engine's own config files.
namespace {
bool ieq(const char* a, const std::string& b) { return _stricmp(a, b.c_str()) == 0; }

// The engine prints its own messages (the 'list' output, "Unknown console command", errors in the
// arguments) to an internal log that shipping builds do not show; only a command's result comes
// back. So GridNode answers 'list' itself and says when a command does not exist.
std::string listing(const std::string& filter) {
    std::string out;
    size_t n = 0;
    for (const ConsoleCommand& c : consoleCommands()) {
        std::string full = c.full;
        if (!filter.empty()) {
            std::string a = full, f = filter;
            for (char& ch : a) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            for (char& ch : f) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (a.find(f) == std::string::npos) continue;
        }
        out += full;
        if (c.setting) out += "  [setting]";
        if (c.online) out += "  [online]";
        if (*c.help) out += std::string("  ") + c.help;
        out += "\n";
        ++n;
    }
    out += std::to_string(n) + " command(s)" + (filter.empty() ? "" : " matching \"" + filter + "\"") +
           ". A setting with no value prints it; with a value sets it.";
    return out;
}

bool known(const std::string& word) {
    const size_t dot = word.find('.');
    for (const ConsoleCommand& c : consoleCommands()) {
        if (ieq(c.full, word)) return true;
        if (dot == std::string::npos && ieq(c.name, word)) return true; // the engine accepts a name without its group
    }
    return false;
}
} // namespace

std::string consoleRun(const std::string& line) {
    if (!consoleOk()) return "(the console is not available)";
    const size_t start = line.find_first_not_of(' ');
    if (start == std::string::npos) return {};
    const size_t sp = line.find(' ', start);
    const std::string word = line.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
    std::string rest = sp == std::string::npos ? std::string() : line.substr(line.find_first_not_of(' ', sp) == std::string::npos ? line.size() : line.find_first_not_of(' ', sp));
    if (_stricmp(word.c_str(), "list") == 0 || _stricmp(word.c_str(), "help") == 0) return listing(rest);
    if (_stricmp(word.c_str(), "progress") == 0) return progressCommand(rest); // GridNode's own (gamestate.cpp)
    if (_stricmp(word.c_str(), "ant") == 0) return antCommand(rest);           // GridNode's own (ant.cpp)
    if (_stricmp(word.c_str(), "camera") == 0) return layouts::cameraCommand(rest); // GridNode's own (layouts.cpp)
    if (_stricmp(word.c_str(), "replay") == 0) return replayCommand();          // GridNode's own (replay.cpp)
    if (_stricmp(word.c_str(), "sample") == 0) return sampleCommand(rest);      // GridNode's own (sampler.cpp)
    if (!known(word)) return "Unknown console command \"" + word + "\" (type list to see them all)";
    EngineString s{};
    reinterpret_cast<void (*)(EngineString*, const char*, uint8_t)>(mem::rva(e::function::Console_executeCommand))(&s, line.c_str(), 1);
    std::string out;
    if (s.begin && s.end >= s.begin && s.end - s.begin < (1 << 20)) out.assign(s.begin, s.end);
    if (s.begin && s.capacity - s.begin > 1)
        reinterpret_cast<void (*)(void*, void*, int)>(mem::rva(e::function::EngineAllocator_free))(&s.allocator, s.begin,
                                                                                                  static_cast<int>(s.capacity - s.begin));
    return out.empty() ? "(done; the command printed nothing)" : out;
}

std::vector<ConsoleCommand> consoleCommands() {
    std::lock_guard<std::mutex> lock(g_cmdM);
    if (!consoleOk()) return g_commands;
    uintptr_t sv[2] = {}, bv[2] = {};
    const auto staticVec = reinterpret_cast<uintptr_t (*)()>(mem::rva(e::function::ConsoleRegistry_getStaticCommands))();
    const auto boundVec = reinterpret_cast<uintptr_t (*)()>(mem::rva(e::function::ConsoleRegistry_getBoundCommands))();
    if (!mem::read(staticVec, sv, sizeof(sv)) || !mem::read(boundVec, bv, sizeof(bv))) return g_commands;
    const size_t ns = sv[1] >= sv[0] ? (sv[1] - sv[0]) / 8 : 0;
    const size_t nb = bv[1] >= bv[0] ? (bv[1] - bv[0]) / 0x28 : 0;
    if (ns == g_seenStatic && nb == g_seenBound) return g_commands;
    if (ns > 20000 || nb > 20000) return g_commands;
    std::vector<ConsoleCommand> cmds;
    std::vector<uintptr_t> ptrs(ns);
    if (ns && mem::read(sv[0], ptrs.data(), ns * 8)) {
        for (uintptr_t p : ptrs) {
            uintptr_t rec[4] = {};
            if (!p || !mem::read(p, rec, sizeof(rec))) continue;
            ConsoleCommand c;
            c.name = intern(cstr(rec[1]));
            c.group = intern(cstr(rec[2]));
            c.help = intern(cstr(rec[3]));
            // Settings fields are added to this list too (ConsoleRegistry_addCommands), with the
            // field binding as their function.
            c.setting = rec[0] == mem::rva(e::function::ConsoleFieldBinding_getOrSet);
            cmds.push_back(c);
        }
    }
    std::vector<uintptr_t> bound(nb * 5);
    if (nb && mem::read(bv[0], bound.data(), nb * 0x28)) {
        for (size_t i = 0; i < nb; ++i) {
            ConsoleCommand c;
            c.name = intern(cstr(bound[i * 5 + 0]));
            c.group = intern(cstr(bound[i * 5 + 1]));
            c.help = intern(cstr(bound[i * 5 + 2]));
            c.setting = true;
            cmds.push_back(c);
        }
    }
    for (ConsoleCommand& c : cmds) {
        const std::string full = *c.group ? std::string(c.group) + "." + c.name : std::string(c.name);
        c.full = intern(full);
        c.online = onlineFor(*c.group ? c.group : c.name);
    }
    std::sort(cmds.begin(), cmds.end(), [](const ConsoleCommand& a, const ConsoleCommand& b) { return strcmp(a.full, b.full) < 0; });
    size_t settingCmds = 0;
    for (const ConsoleCommand& c : cmds) settingCmds += c.setting ? 1 : 0;
    GN_LOG("console: %llu commands (%llu of them settings fields; %llu in the bound list)",
           static_cast<unsigned long long>(cmds.size()), static_cast<unsigned long long>(settingCmds), static_cast<unsigned long long>(nb));
    g_commands = std::move(cmds);
    g_seenStatic = ns;
    g_seenBound = nb;
    return g_commands;
}

const char* commandOnline(const std::string& line) {
    std::string word = line.substr(0, line.find(' '));
    const size_t dot = word.find('.');
    return onlineFor(dot == std::string::npos ? word : word.substr(0, dot));
}

} // namespace gn::host
