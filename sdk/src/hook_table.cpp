#include "gridnode/hook_table.hpp"

#include <cinttypes>
#include <cstdio>

namespace gn::hook {

namespace {
std::string hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "0x%" PRIx64, v);
    return b;
}
} // namespace

Result HookTable::install(const std::string& owner, uint64_t slot, uint64_t expected, uint64_t detour,
                          uint64_t* original, std::string* why) {
    auto fail = [&](Result r, const std::string& msg) {
        if (why) *why = msg;
        return r;
    };
    if (!slot || !detour || !original || (slot & 7))
        return fail(Result::Argument, "slot must be a non-null, 8-byte aligned address; detour and original non-null");
    std::lock_guard<std::mutex> lock(m_);
    uint64_t current = 0;
    if (!io_.read(slot, current)) return fail(Result::Slot, "slot " + hex(slot) + " is not readable");
    auto& chain = slots_[slot];
    if (chain.empty()) {
        if (expected ? current != expected : !io_.isGameCode(current)) {
            slots_.erase(slot);
            return fail(Result::Slot, "slot " + hex(slot) + " holds " + hex(current) + ", expected " +
                                          (expected ? hex(expected) : std::string("a game function")));
        }
    } else if (current != chain.back().detour) {
        return fail(Result::Slot, "slot " + hex(slot) + " was changed outside GridNode (holds " + hex(current) +
                                      ", last hook " + hex(chain.back().detour) + ")");
    }
    // Publish the original before the swap: the detour may run on another thread at once.
    *original = current;
    if (!io_.write(slot, detour)) {
        if (chain.empty()) slots_.erase(slot);
        return fail(Result::Slot, "slot " + hex(slot) + " could not be written");
    }
    chain.push_back({owner, detour, current});
    return Result::Ok;
}

std::vector<std::pair<uint64_t, std::vector<Entry>>> HookTable::snapshot() const {
    std::lock_guard<std::mutex> lock(m_);
    return {slots_.begin(), slots_.end()};
}

size_t HookTable::count() const {
    std::lock_guard<std::mutex> lock(m_);
    size_t n = 0;
    for (const auto& s : slots_) n += s.second.size();
    return n;
}

} // namespace gn::hook
