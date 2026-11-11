#pragma once
// The offsets mods put on the game's mixer groups (gn_api bus_*): what each mod set, the ducks and their
// ramps, and the sum the mixer's property step adds. Portable (no game access), so the ramp and the
// stacking rules are tested on the host (tests/runtime_core_test.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace gn::audio {

constexpr int kGroups = 161;       // the mixer's groups (108 seen) and the master; the game's table is checked against it
constexpr float kFloorDb = -96.f;  // gain 0 (silence) as an offset
constexpr float kDuckAttack = 0.1f;  // seconds to reach a duck's gain (linear in gain, as the game fades)
constexpr float kDuckRelease = 0.5f; // seconds back to where the duck found it

inline float gainToDb(float gain) {
    if (!(gain > 1e-5f)) return kFloorDb; // zero, negative and NaN
    return std::min(0.f, std::max(kFloorDb, 20.f * std::log10(gain)));
}
inline float dbToGain(float db) { return std::pow(10.f, db / 20.f); }

class MixOffsets {
public:
    // A mod's steady gain on a group (1 = no change). Another call by the same mod replaces it.
    void set(const std::string& mod, uintptr_t tag, int group, float gain) {
        if (group < 0 || group >= kGroups) return;
        std::lock_guard<std::mutex> lock(m_);
        for (auto it = sets_.begin(); it != sets_.end(); ++it)
            if (it->group == group && it->mod == mod) {
                sets_.erase(it);
                break;
            }
        const float db = gainToDb(gain);
        if (db != 0.f) sets_.push_back({mod, tag, group, db});
    }

    // Pulls a group down to `gain` for `seconds` (held), then back. Ducks on one group do not add: the deepest wins.
    void duck(const std::string& mod, uintptr_t tag, int group, float gain, float seconds) {
        if (group < 0 || group >= kGroups) return;
        std::lock_guard<std::mutex> lock(m_);
        ducks_.push_back({mod, tag, group, std::min(1.f, std::max(0.f, gain)), 0.f, std::max(0.f, seconds)});
    }

    // A mod went (its code is `gone`'s): its offsets and ducks are dropped. Returns the groups that changed.
    std::vector<int> forget(const std::function<bool(uintptr_t tag)>& gone) {
        std::lock_guard<std::mutex> lock(m_);
        std::vector<int> groups;
        for (auto it = sets_.begin(); it != sets_.end();)
            if (gone(it->tag)) {
                groups.push_back(it->group);
                it = sets_.erase(it);
            } else {
                ++it;
            }
        for (auto it = ducks_.begin(); it != ducks_.end();)
            if (gone(it->tag)) {
                groups.push_back(it->group);
                it = ducks_.erase(it);
            } else {
                ++it;
            }
        std::sort(groups.begin(), groups.end());
        groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
        return groups;
    }

    // Moves the ducks on by dt and writes every group's offset (dB) to out[kGroups]. Returns the groups whose
    // offset differs from the last call (so the caller makes the game recompute them).
    std::vector<int> step(float dt, float* out) {
        std::lock_guard<std::mutex> lock(m_);
        for (Duck& d : ducks_) d.t += dt;
        ducks_.erase(std::remove_if(ducks_.begin(), ducks_.end(), [](const Duck& d) { return d.t >= kDuckAttack + d.hold + kDuckRelease; }),
                     ducks_.end());
        float now[kGroups];
        float deepest[kGroups];
        for (int g = 0; g < kGroups; ++g) {
            now[g] = 0.f;
            deepest[g] = 0.f;
        }
        for (const Set& s : sets_) now[s.group] += s.db;
        for (const Duck& d : ducks_) deepest[d.group] = std::min(deepest[d.group], duckDb(d));
        std::vector<int> changed;
        for (int g = 0; g < kGroups; ++g) {
            const float total = std::max(kFloorDb, now[g] + deepest[g]);
            if (total != last_[g]) {
                last_[g] = total;
                changed.push_back(g);
            }
            out[g] = total;
        }
        return changed;
    }

    bool any() const {
        std::lock_guard<std::mutex> lock(m_);
        return !sets_.empty() || !ducks_.empty();
    }

private:
    struct Set {
        std::string mod;
        uintptr_t tag;
        int group;
        float db;
    };
    struct Duck {
        std::string mod;
        uintptr_t tag;
        int group;
        float gain;
        float t;
        float hold;
    };
    static float duckDb(const Duck& d) {
        float lin = d.gain;
        if (d.t < kDuckAttack) lin = 1.f + (d.gain - 1.f) * (d.t / kDuckAttack);
        else if (d.t >= kDuckAttack + d.hold) lin = d.gain + (1.f - d.gain) * ((d.t - kDuckAttack - d.hold) / kDuckRelease);
        return gainToDb(lin);
    }

    mutable std::mutex m_;
    std::vector<Set> sets_;
    std::vector<Duck> ducks_;
    float last_[kGroups] = {};
};

} // namespace gn::audio
