#pragma once
// The portable parts of mod settings files and movement events (tested on the host): a mod's
// settings are key=value lines in mods/<mod>.ini; movement events are what changed between two
// readings of Faith's movement.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gn::config {

using Entries = std::vector<std::pair<std::string, std::string>>; // in file order

// key=value lines. Blank lines and lines starting with # or ; are skipped, spaces around the key
// and the value are trimmed, a later line for the same key wins.
Entries parse(const std::string& text);
// One key=value line per entry, after `header` (each of its lines prefixed with "# ").
std::string serialize(const Entries& entries, const std::string& header);
// A key the file can hold: not empty, no '=', no line break, no spaces at either end, not a comment.
bool validKey(const std::string& key);
// A value the file can hold: one line, spaces at either end are lost.
std::string cleanValue(const std::string& value);
// The mod's file name without ".ini": letters, digits, '-', '_' and '.' kept, anything else '_'.
std::string fileName(const std::string& mod);
const std::string* find(const Entries& entries, const std::string& key);
// Sets (or adds) the key; true when that changed the entries.
bool set(Entries& entries, const std::string& key, const std::string& value);

} // namespace gn::config

namespace gn::movement {

// GN_MOVE_EVENT_* in gridnode/api.h.
constexpr int32_t kEventState = 1, kEventLanded = 2, kEventAirborne = 3;

struct Sample {
    int32_t state = 0;
    bool inAir = false;
};

struct Event {
    int32_t kind = 0;
    int32_t state = 0;    // the state now
    int32_t oldState = 0; // the state at the last reading
};

// What changed from `was` to `now`, in order: the landing or take-off first, then the state.
std::vector<Event> diff(const Sample& was, const Sample& now);

} // namespace gn::movement
