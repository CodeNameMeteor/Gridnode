#include "gridnode/hook_table.hpp"

#include <cinttypes>
#include <cstddef>
#include <iterator>
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
    const bool foreign = io_.isForeignHook && io_.isForeignHook(current);
    if (chain.empty()) {
        if (!foreign && (expected ? current != expected : !io_.isGameCode(current))) {
            slots_.erase(slot);
            return fail(Result::Slot, "slot " + hex(slot) + " holds " + hex(current) + ", expected " +
                                          (expected ? hex(expected) : std::string("a game function")));
        }
    } else if (current != chain.back().detour && !foreign) {
        return fail(Result::Slot, "slot " + hex(slot) + " was changed outside GridNode (holds " + hex(current) +
                                      ", last hook " + hex(chain.back().detour) + ")");
    }
    // Publish the original before the swap: the detour may run on another thread at once.
    *original = current;
    if (!io_.write(slot, detour)) {
        if (chain.empty()) slots_.erase(slot);
        return fail(Result::Slot, "slot " + hex(slot) + " could not be written");
    }
    chain.push_back({owner, detour, current, original});
    return Result::Ok;
}

size_t HookTable::remove(const std::function<bool(const Entry&)>& which, std::vector<std::string>* refused) {
    std::lock_guard<std::mutex> lock(m_);
    size_t removed = 0;
    for (auto it = slots_.begin(); it != slots_.end();) {
        auto& chain = it->second;
        // From the top down, so a hook above a removed one has already been dealt with.
        for (size_t k = chain.size(); k-- > 0;) {
            const Entry e = chain[k];
            if (!which(e)) continue;
            bool done = false;
            if (k + 1 == chain.size()) {
                uint64_t current = 0;
                done = io_.read(it->first, current) && current == e.detour && io_.write(it->first, e.previous);
            } else if (chain[k + 1].previous == e.detour && chain[k + 1].original) {
                // The hook above calls this one: it calls what this one called instead. Its original is
                // read by its detour on any thread: one aligned 8-byte store.
                *const_cast<volatile uint64_t*>(chain[k + 1].original) = e.previous;
                chain[k + 1].previous = e.previous;
                done = true;
            }
            if (done) {
                chain.erase(chain.begin() + static_cast<std::ptrdiff_t>(k));
                ++removed;
            } else if (refused) {
                refused->push_back(e.owner + " at " + hex(it->first) + ": another hook is on top of it");
            }
        }
        it = chain.empty() ? slots_.erase(it) : std::next(it);
    }
    return removed;
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
