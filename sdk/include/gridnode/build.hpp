#pragma once
// Recognises the Mirror's Edge Catalyst PC build GridNode was made for.
//
// There is one build (the game is no longer updated), so the check is a safety net: if the
// process is not that build, GridNode stays passive instead of reading or writing the wrong
// addresses.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gn::build {

// The image GridNode's addresses come from: the EA app build. The Steam copy launches the game
// through the EA app, so it runs the same exe.
constexpr uint32_t kTimestamp = 0x5762e3b7;
constexpr uint32_t kImageSize = 0x727b000;
constexpr size_t kMinTypes = 9000; // the PC build registers 9,988 types

// Waits until the engine has registered its types (the count is at least kMinTypes and has not
// changed for a second). Returns the count, or 0 on timeout or when `stop` is set.
size_t waitForTypeList(uint32_t timeoutMs, const std::atomic<bool>* stop = nullptr);

struct Result {
    bool ok = false;      // every required check passed
    int failed = 0;       // required checks that failed
    int warnings = 0;     // informational differences (timestamp, image size)
    std::string details;  // one line per failure or warning
};

// Compares known TypeInfo addresses, vtable slots and the first bytes of functions GridNode calls.
Result check();

// One fingerprint code entry by name (db/annotations/fingerprint.yaml), e.g. "Console_executeCommand":
// true if the bytes are as expected. Features behind optional fingerprints check this first.
bool codeMatches(const char* what);

} // namespace gn::build
