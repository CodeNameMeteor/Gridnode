// Mod settings files and movement events: the portable parts (gridnode/config.hpp).

#include "gridnode/config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gn::config {
namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

} // namespace

Entries parse(const std::string& text) {
    Entries out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        if (!validKey(key)) continue;
        set(out, key, trim(line.substr(eq + 1)));
    }
    return out;
}

std::string serialize(const Entries& entries, const std::string& header) {
    std::string o;
    size_t pos = 0;
    while (pos < header.size()) {
        size_t end = header.find('\n', pos);
        if (end == std::string::npos) end = header.size();
        o += "# " + header.substr(pos, end - pos) + "\n";
        pos = end + 1;
    }
    for (const auto& [k, v] : entries) o += k + "=" + v + "\n";
    return o;
}

std::string serialize(const Entries& entries, const std::string& header, const Comments& comments) {
    std::string o = serialize(Entries{}, header);
    for (const auto& [k, v] : entries) {
        for (const auto& [ck, text] : comments) {
            if (ck != k) continue;
            o += "\n";
            size_t pos = 0;
            while (pos < text.size()) {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                o += "# " + text.substr(pos, end - pos) + "\n";
                pos = end + 1;
            }
            break;
        }
        o += k + "=" + v + "\n";
    }
    return o;
}

bool normalize(Decl& d) {
    switch (d.kind) {
    case Kind::Bool: case Kind::Int: case Kind::Float: case Kind::Text: break;
    case Kind::Choice:
        if (d.choices.empty()) return false;
        break;
    default:
        return false;
    }
    if (d.min > d.max) std::swap(d.min, d.max);
    if (!std::isfinite(d.def)) d.def = 0;
    const double def = d.def;
    d.def = 0; // clamp() falls back on the default
    d.def = clamp(d, def);
    return true;
}

double clamp(const Decl& d, double v) {
    if (!std::isfinite(v)) return d.def;
    switch (d.kind) {
    case Kind::Bool: return v != 0 ? 1 : 0;
    case Kind::Choice: return std::min(std::max(std::round(v), 0.0), double(d.choices.size() - 1));
    case Kind::Int: v = std::round(v); break;
    case Kind::Float: break;
    case Kind::Text: return 0;
    }
    if (d.min < d.max) v = std::min(std::max(v, d.min), d.max);
    return v;
}

std::string format(const Decl& d, double v) {
    v = clamp(d, v);
    char b[48];
    if (d.kind == Kind::Float) snprintf(b, sizeof(b), "%.7g", v);
    else snprintf(b, sizeof(b), "%.0f", v);
    return b;
}

double parseNumber(const Decl& d, const std::string& text) {
    const std::string t = trim(text);
    if (d.kind == Kind::Bool) {
        if (t == "true" || t == "on" || t == "yes") return 1;
        if (t == "false" || t == "off" || t == "no") return 0;
    }
    if (d.kind == Kind::Choice)
        for (size_t i = 0; i < d.choices.size(); ++i)
            if (t == d.choices[i]) return double(i);
    if (t.empty()) return d.def;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (!end || *end) return d.def;
    return clamp(d, v);
}

std::string comment(const Decl& d) {
    std::string c = d.label.empty() ? d.id : d.label;
    if (!d.description.empty()) c += ": " + d.description;
    if (!c.empty() && c.back() != '.') c += ".";
    switch (d.kind) {
    case Kind::Bool: c += std::string(" 1 or 0, default ") + (d.def ? "1." : "0."); break;
    case Kind::Choice: {
        c += " One of:";
        for (size_t i = 0; i < d.choices.size(); ++i) c += " " + std::to_string(i) + " " + d.choices[i] + (i + 1 < d.choices.size() ? "," : ".");
        c += " Default " + format(d, d.def) + ".";
        break;
    }
    case Kind::Int: case Kind::Float:
        if (d.min < d.max) c += " From " + format(d, d.min) + " to " + format(d, d.max) + ",";
        c += " default " + format(d, d.def) + ".";
        break;
    case Kind::Text:
        if (!d.defText.empty()) c += " Default: " + d.defText + ".";
        break;
    }
    return c;
}

bool validKey(const std::string& key) {
    if (key.empty() || key != trim(key) || key[0] == '#' || key[0] == ';') return false;
    for (char c : key)
        if (c == '=' || c == '\n' || c == '\r') return false;
    return true;
}

std::string cleanValue(const std::string& value) {
    std::string v;
    for (char c : value) v += (c == '\n' || c == '\r') ? ' ' : c;
    return trim(v);
}

std::string fileName(const std::string& mod) {
    std::string f;
    for (char c : mod) f += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') ? c : '_';
    if (f.empty() || f.find_first_not_of('.') == std::string::npos) f = "_" + f; // never "", "." or ".."
    return f;
}

const std::string* find(const Entries& entries, const std::string& key) {
    for (const auto& [k, v] : entries)
        if (k == key) return &v;
    return nullptr;
}

bool set(Entries& entries, const std::string& key, const std::string& value) {
    const std::string v = cleanValue(value);
    for (auto& [k, old] : entries)
        if (k == key) {
            if (old == v) return false;
            old = v;
            return true;
        }
    entries.emplace_back(key, v);
    return true;
}

} // namespace gn::config

namespace gn::moveevents {

std::vector<Event> diff(const Sample& was, const Sample& now) {
    std::vector<Event> out;
    if (was.inAir != now.inAir) out.push_back({now.inAir ? kEventAirborne : kEventLanded, now.state, was.state});
    if (was.state != now.state) out.push_back({kEventState, now.state, was.state});
    return out;
}

} // namespace gn::moveevents
