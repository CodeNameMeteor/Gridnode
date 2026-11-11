#pragma once
// One hook on a virtual of a class, in every vtable of the class (and its subclasses): what
// gn_api hook_virtual hands out. Subclasses override, so there is no single original: each vtable
// keeps its own, and the original an object needs is found by its vtable (hook_original). The
// chaining itself is the HookTable's, as for hook_slot. Portable, tested on the host with fake
// vtables (tests/runtime_core_test.cpp).
//
// GridNode wrapped "this slot in every subclass" three times with its own templates (destructors,
// events, creators); this is the general form.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "gridnode/hook_table.hpp"

namespace gn::vhook {

class Set {
public:
    // vtables: runtime addresses (any order, duplicates allowed); offset: the slot's byte offset.
    Set(std::string owner, std::vector<uint64_t> vtables, uint32_t offset, uint64_t detour) : owner_(std::move(owner)), offset_(offset), detour_(detour) {
        std::sort(vtables.begin(), vtables.end());
        vtables.erase(std::unique(vtables.begin(), vtables.end()), vtables.end());
        slots_.reserve(vtables.size()); // never grows again: the hook table keeps &original
        for (uint64_t vt : vtables) slots_.push_back({vt, 0, false});
    }
    Set(const Set&) = delete;
    Set& operator=(const Set&) = delete;

    // Hooks every vtable's slot. Returns how many took; `firstWhy` gets the first refusal.
    size_t install(hook::HookTable& table, std::string* firstWhy = nullptr) {
        size_t ok = 0;
        for (Slot& s : slots_) {
            std::string why;
            if (table.install(owner_, s.vtable + offset_, 0, detour_, &s.original, &why) == hook::Result::Ok) {
                s.hooked = true;
                ++ok;
            } else if (firstWhy && firstWhy->empty()) {
                *firstWhy = why;
            }
        }
        return ok;
    }

    // What an object with this vtable should call: its class's function at the slot (or the hook
    // below this one). 0 for a vtable this set never hooked. After removal it stays what it last was
    // (a thread may still be inside the detour, and that function is still there).
    uint64_t original(uint64_t vtable) const {
        const auto it = std::lower_bound(slots_.begin(), slots_.end(), vtable, [](const Slot& s, uint64_t v) { return s.vtable < v; });
        return it != slots_.end() && it->vtable == vtable ? it->original : 0;
    }

    // Takes the hook out of every slot it is in. How many came out (a slot with another mod's hook
    // on top stays, as with every hook).
    size_t remove(hook::HookTable& table, std::vector<std::string>* refused = nullptr) {
        // An entry is this set's when its original is one this set keeps (HookTable::remove does not
        // say which slot an entry is in).
        const auto ours = [this](const uint64_t* p) {
            return !slots_.empty() && p >= &slots_.front().original && p <= &slots_.back().original;
        };
        const size_t n = table.remove([&](const hook::Entry& e) { return e.detour == detour_ && e.owner == owner_ && ours(e.original); }, refused);
        // What stayed (another mod's hook on top) is still hooked.
        for (Slot& s : slots_) s.hooked = false;
        for (const auto& [slot, chain] : table.snapshot())
            for (const hook::Entry& e : chain)
                if (e.detour == detour_ && ours(e.original))
                    for (Slot& s : slots_)
                        if (&s.original == e.original) s.hooked = true;
        return n;
    }

    size_t vtableCount() const { return slots_.size(); }
    size_t hookedCount() const {
        return static_cast<size_t>(std::count_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.hooked; }));
    }
    uint32_t offset() const { return offset_; }
    uint64_t detour() const { return detour_; }
    const std::string& owner() const { return owner_; }

private:
    struct Slot {
        uint64_t vtable;
        uint64_t original; // written by the hook table (the hook below this one, or the game's)
        bool hooked;
    };
    std::string owner_;
    uint32_t offset_;
    uint64_t detour_;
    std::vector<Slot> slots_; // sorted by vtable
};

} // namespace gn::vhook
