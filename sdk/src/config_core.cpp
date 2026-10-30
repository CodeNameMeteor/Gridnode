// Mod settings files and movement events: the portable parts (gridnode/config.hpp).

#include "gridnode/config.hpp"

#include <cctype>

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
