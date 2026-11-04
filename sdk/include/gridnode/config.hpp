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
// The mod's file name without ".ini": letters (lower case), digits, '-', '_' and '.' kept, anything
// else '_'; never a reserved Windows device name (con, nul, com1...); at most 64 characters.
std::string fileName(const std::string& mod);
const std::string* find(const Entries& entries, const std::string& key);
// Sets (or adds) the key; true when that changed the entries.
bool set(Entries& entries, const std::string& key, const std::string& value);

// Comment lines written above a key (each prefixed "# "): a declared setting's label and description.
using Comments = std::vector<std::pair<std::string, std::string>>; // key -> text (lines split on '\n')
std::string serialize(const Entries& entries, const std::string& header, const Comments& comments);

// ---- declared settings (gn_api setting_add, API 17) ----------------------------------------------
// A mod declares a setting once (its kind, default, range, label, description); GridNode keeps the
// value in the mod's file as text, clamps it, and draws it on the mod's settings page.
enum class Kind : int32_t { Bool = 1, Int = 2, Float = 3, Choice = 4, Text = 5 }; // GN_SETTING_*
struct Decl {
    std::string id, label, description, group;
    Kind kind = Kind::Bool;
    double def = 0, min = 0, max = 0, step = 0; // min == max: no range (Int, Float)
    std::vector<std::string> choices;          // Choice: the labels; the value is the index
    std::string defText;                       // Text
    uint32_t flags = 0;                        // GN_SETTING_HIDDEN ...
};
// The declaration made usable: an unknown kind refused (false), Choice with no choices refused,
// min / max swapped when backwards, the default clamped.
bool normalize(Decl& d);
// A number as the setting allows it: Bool 0 / 1, Int and Choice whole numbers, inside the range
// (Choice: 0 to the last choice), NaN to the default.
double clamp(const Decl& d, double v);
// The file's text for a number ("1", "-3", "0.25"), and back: the default when it is not a number of
// that kind (a Bool also reads "true" / "false"; a Choice also its label).
std::string format(const Decl& d, double v);
double parseNumber(const Decl& d, const std::string& text);
// "Label: description. From 0 to 10, default 2." for the file.
std::string comment(const Decl& d);

} // namespace gn::config

// (Not gn::movement: that is mod.hpp's reading of her movement.)
namespace gn::moveevents {

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

} // namespace gn::moveevents
