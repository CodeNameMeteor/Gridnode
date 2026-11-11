// Experiment template: counts calls to a game function through gn::hookFunction. Copy this file
// to experiments/<topic>.cpp, rename the namespace and the topic, and test the topic's claims.
//
// Rules for an experiment:
//   - only the GridNode API that exists today (gn::read, gn::function, gn::hookFunction, gn::patchBytes);
//     it never edits the SDK
//   - addresses and offsets come from claims that verified.md marks confirmed or needs-game-test, and the
//     comment next to each says which claim (c1, c2, ...)
//   - detours run on whatever thread calls the function: atomics only, and always call the original
//   - log what the claims predict next to what was seen, so the test result is a log you can send back
#include <atomic>

#include "gridnode/engine.hpp"
#include "gridnode/ui.hpp"
#include "lab.hpp"

namespace f = gn::engine::function;

namespace {

namespace example_events {

// Signature from db/annotations (EntityBus_postEvent); claim: example, not a real claim.
using PostEvent = void (*)(void* busPeer, void* source, uint32_t eventId, void* a4);
PostEvent g_post = nullptr;
std::atomic<uint64_t> g_calls{0};
std::atomic<uint32_t> g_lastId{0};

void post(void* busPeer, void* source, uint32_t id, void* a4) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_lastId.store(id, std::memory_order_relaxed);
    g_post(busPeer, source, id, a4);
}

bool install() {
    const bool ok = gn::hookFunction(f::EntityBus_postEvent, &post, &g_post);
    gn::log("[example] EntityBus_postEvent detour %s", ok ? "ok" : "FAILED");
    return ok;
}

void draw() {
    ImGui::Text("postEvent calls: %llu, last event id 0x%08x", static_cast<unsigned long long>(g_calls.load()), g_lastId.load());
}

void reset() { g_calls = 0; }

} // namespace example_events

} // namespace

LAB_EXPERIMENT(example_events, "example-events", "template: counts postEvent calls (not a research claim)", &example_events::install,
               &example_events::draw, &example_events::reset);
