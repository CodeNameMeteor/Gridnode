// Mod load order from manifests (gridnode/mod_order.hpp).

#include "gridnode/mod_order.hpp"

#include <cctype>
#include <map>

namespace gn::modorder {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<long> parts(const std::string& v) {
    std::vector<long> out;
    size_t i = 0;
    while (i <= v.size()) {
        const size_t dot = v.find('.', i);
        const std::string p = v.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        long n = 0;
        for (char c : p) {
            if (!std::isdigit(static_cast<unsigned char>(c))) break;
            n = n * 10 + (c - '0');
        }
        out.push_back(n);
        if (dot == std::string::npos) break;
        i = dot + 1;
    }
    return out;
}

} // namespace

Dep parseDep(const std::string& text) {
    Dep d;
    std::string t = trim(text);
    if (!t.empty() && t[0] == '?') d.soft = true, t = trim(t.substr(1));
    const size_t ge = t.find(">=");
    if (ge != std::string::npos) d.minVersion = trim(t.substr(ge + 2)), t = trim(t.substr(0, ge));
    d.id = t;
    return d;
}

int compareVersions(const std::string& a, const std::string& b) {
    const std::vector<long> x = parts(a), y = parts(b);
    for (size_t i = 0; i < x.size() || i < y.size(); ++i) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

Plan plan(const std::vector<Entry>& mods) {
    const size_t n = mods.size();
    Plan out;
    out.problem.assign(n, "");
    std::map<std::string, size_t> byId;
    for (size_t i = 0; i < n; ++i) {
        if (byId.count(mods[i].id)) out.problem[i] = "the id \"" + mods[i].id + "\" is already taken by another mod";
        else byId[mods[i].id] = i;
    }
    // Missing or too old hard dependencies.
    for (size_t i = 0; i < n; ++i) {
        if (!out.problem[i].empty()) continue;
        for (const Dep& d : mods[i].deps) {
            if (d.soft || d.id.empty()) continue;
            auto it = byId.find(d.id);
            if (it == byId.end()) out.problem[i] = "needs " + d.id + (d.minVersion.empty() ? "" : " " + d.minVersion + " or newer") + ", which is not there";
            else if (!d.minVersion.empty() && compareVersions(mods[it->second].version, d.minVersion) < 0)
                out.problem[i] = "needs " + d.id + " " + d.minVersion + " or newer (it is " + (mods[it->second].version.empty() ? "unversioned" : mods[it->second].version) + ")";
            if (!out.problem[i].empty()) break;
        }
    }
    // Kahn's algorithm, picking the lowest index ready each time (file name order otherwise).
    // Optional dependencies only order: when the mods left wait on each other, their optional links
    // are dropped and loading goes on (two mods that optionally need each other both loaded none,
    // review 2026-11-03). Only required ones can make a cycle that stops a mod.
    std::vector<int> waiting(n, 0);
    std::vector<bool> done(n, false), softDropped(n, false);
    const auto count = [&](size_t i) {
        int w = 0;
        for (const Dep& d : mods[i].deps) {
            auto it = byId.find(d.id);
            if (it == byId.end() || it->second == i || done[it->second] || (d.soft && softDropped[i])) continue;
            ++w;
        }
        return w;
    };
    for (size_t i = 0; i < n; ++i) waiting[i] = count(i);
    for (size_t step = 0; step < n; ++step) {
        size_t pick = n;
        for (size_t i = 0; i < n && pick == n; ++i)
            if (!done[i] && waiting[i] == 0) pick = i;
        if (pick == n) { // the rest wait on each other: their optional links go, once
            bool dropped = false;
            for (size_t i = 0; i < n; ++i)
                if (!done[i] && !softDropped[i]) softDropped[i] = dropped = true, waiting[i] = count(i);
            if (!dropped) break; // a cycle of required dependencies
            --step;
            continue;
        }
        done[pick] = true;
        for (size_t i = 0; i < n; ++i)
            if (!done[i]) waiting[i] = count(i);
        if (!out.problem[pick].empty()) continue;
        // A hard dependency left out takes this one with it.
        for (const Dep& d : mods[pick].deps) {
            auto it = byId.find(d.id);
            if (d.soft || it == byId.end() || out.problem[it->second].empty()) continue;
            out.problem[pick] = "needs " + d.id + ", which did not load";
            break;
        }
        if (out.problem[pick].empty()) out.order.push_back(pick);
    }
    for (size_t i = 0; i < n; ++i)
        if (!done[i] && out.problem[i].empty()) out.problem[i] = "in a dependency cycle";
    return out;
}

} // namespace gn::modorder
