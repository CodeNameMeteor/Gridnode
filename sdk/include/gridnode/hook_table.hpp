#pragma once
// The bookkeeping behind gn_api::hook_slot: which slots are hooked, by whom, in what order.
// Portable: the actual memory access is injected, so the chaining rules are tested on the host.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gn::hook {

struct SlotAccess {
    std::function<bool(uint64_t slot, uint64_t& value)> read;
    // Swaps the pointer at slot (atomically); returns false if the slot cannot be written.
    std::function<bool(uint64_t slot, uint64_t value)> write;
    // Is this a function in the game image? (used when no expected value is given)
    std::function<bool(uint64_t fn)> isGameCode;
    // Optional: is this another mod's hook (code of another loaded module)? Such a slot is chained
    // instead of refused, so GridNode and MECMP can be loaded in either order.
    std::function<bool(uint64_t fn)> isForeignHook;
};

struct Entry {
    std::string owner;
    uint64_t detour = 0;
    uint64_t previous = 0; // what the slot held before this hook (the "original" it calls)
};

enum class Result { Ok = 0, Argument = 1, Slot = 3 };

class HookTable {
public:
    explicit HookTable(SlotAccess io) : io_(std::move(io)) {}

    // slot and expected are runtime addresses. expected = 0: any game function.
    // A slot may be hooked several times: each hook receives the previous one as its original.
    // Refused when the slot does not hold the expected game function (first hook) or no longer
    // holds the last hook GridNode installed (someone else changed it), unless what it holds is
    // another mod's hook (isForeignHook): then GridNode goes on top and calls that hook.
    Result install(const std::string& owner, uint64_t slot, uint64_t expected, uint64_t detour, uint64_t* original,
                   std::string* why = nullptr);

    std::vector<std::pair<uint64_t, std::vector<Entry>>> snapshot() const;
    size_t count() const;

private:
    SlotAccess io_;
    mutable std::mutex m_;
    std::map<uint64_t, std::vector<Entry>> slots_;
};

} // namespace gn::hook
