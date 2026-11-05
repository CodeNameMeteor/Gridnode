// Faith's last 15 seconds (gn_api replay_recent, API 27): the keyframe ring the game's replay recorder
// keeps for time trial ghosts (PamClientReplayRecordingComponent +0x48 -> ReplayRecorder). Layout read
// in Ghidra on 2026-11-05 (ReplayRecorder_SampleAndStream 0x143974390, the ring reset 0x14394f1c0;
// db/annotations/replay.yaml): each client tick head moves on, wrapping at capacity, and the keyframe
// there is written {f32 time, u32 flag, ..., bones at +0x10}. Until head first catches tail
// (notWrapped) keyframes 1..head are the written ones; after that all of them, oldest at tail.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {
namespace e = gn::engine;
constexpr uint32_t kMaxCapacity = 4096; // the game asks for 450
constexpr uint32_t kMaxBones = 64;      // MaxReplayBones is 32
} // namespace

int replayRecent(gn_replay_info* info, float* times, gn_replay_bone* bones, uint32_t maxFrames) {
    if (!info || info->size < sizeof(gn_replay_info)) return -GN_ERR_ARGUMENT;
    if (!isClientThread()) return -GN_ERR_THREAD;
    void* faith = localClientCharacter();
    static const reflect::Type* type = registry().find("PamClientReplayRecordingComponent");
    if (!faith || !type) return -GN_ERR_UNAVAILABLE;
    const uintptr_t c = reinterpret_cast<uintptr_t>(getComponent(faith, reinterpret_cast<const gn_type*>(type)));
    const uintptr_t r = c ? mem::readOr<uintptr_t>(c + e::cls::PamClientReplayRecordingComponent::off::recorder) : 0;
    if (!r) return -GN_ERR_UNAVAILABLE;
    namespace k = e::st::ReplayRecorder::off;
    const uint64_t stride = mem::readOr<uint64_t>(r + k::stride);
    const uint32_t capacity = mem::readOr<uint32_t>(r + k::capacity);
    const uintptr_t base = mem::readOr<uintptr_t>(r + k::base);
    const uint32_t head = mem::readOr<uint32_t>(r + k::head), tail = mem::readOr<uint32_t>(r + k::tail);
    const bool notWrapped = mem::readOr<uint32_t>(r + k::notWrapped) != 0;
    const uint32_t boneCount = mem::readOr<uint32_t>(r + k::boneCount);
    // Not set up yet, or not what was read in Ghidra: nothing rather than garbage.
    if (!base || !capacity || capacity > kMaxCapacity || !boneCount || boneCount > kMaxBones || stride != 0x10ull + boneCount * 0x20ull ||
        head >= capacity || tail >= capacity)
        return -GN_ERR_UNAVAILABLE;
    const uint32_t frames = notWrapped ? head : capacity;
    const uint32_t oldest = notWrapped ? 1 : tail;
    const uint32_t size = info->size;
    *info = gn_replay_info{};
    info->size = size;
    info->frames = frames, info->capacity = capacity, info->bones = boneCount;
    if (frames) info->newest_time = mem::readOr<float>(base + head * stride);
    const uint32_t n = std::min(maxFrames, frames);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t at = (oldest + (frames - n) + i) % capacity;
        const uintptr_t kf = base + at * stride;
        if (times) times[i] = mem::readOr<float>(kf);
        if (bones && !mem::read(kf + 0x10, bones + static_cast<size_t>(i) * boneCount, sizeof(gn_replay_bone) * boneCount))
            return -GN_ERR_UNAVAILABLE;
    }
    return static_cast<int>(n);
}

// The console's "replay": what the ring holds now, and where the root and trajectory bones are in
// its newest keyframe (to check the layout in game).
std::string replayCommand() {
    gn_replay_info info{};
    info.size = sizeof(info);
    if (const int r = replayRecent(&info, nullptr, nullptr, 0); r < 0) return "replay: not available (" + std::to_string(-r) + ": no Faith, or her recorder has not started)";
    std::vector<float> times(2);
    std::vector<gn_replay_bone> bones(static_cast<size_t>(info.bones) * 2);
    const int n = replayRecent(&info, times.data(), bones.data(), 2);
    char b[512];
    int at = snprintf(b, sizeof(b), "replay: %u of %u keyframes, %u bones, newest at %.2f s", info.frames, info.capacity, info.bones, info.newest_time);
    if (n > 0 && info.bones >= 2) {
        const gn_replay_bone* last = bones.data() + static_cast<size_t>(n - 1) * info.bones;
        at += snprintf(b + at, sizeof(b) - at, "; newest: root (%.2f, %.2f, %.2f), trajectory (%.2f, %.2f, %.2f)", last[0].translation[0],
                       last[0].translation[1], last[0].translation[2], last[1].translation[0], last[1].translation[1], last[1].translation[2]);
        if (n == 2) snprintf(b + at, sizeof(b) - at, "; %.3f s between the last two", times[1] - times[0]);
    }
    GN_LOG("%s", b);
    return b;
}

} // namespace gn::host
