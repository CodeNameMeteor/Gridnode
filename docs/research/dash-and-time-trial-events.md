# Dash and Time Trial Events Reverse Engineering & Typed Event Interface

> [!NOTE]
> **Status**: Reverse engineered from binary dump (`MirrorsEdgeCatalyst_dump_SCY.exe`, Image Base `0x140000000`) and validated against `MECMP` reference implementation (`runs.cpp` / `runs.h`). Database annotations added to [`db/annotations/messages.yaml`](file:///D:/Dev/GridNode/db/annotations/messages.yaml) and [`db/annotations/ui.yaml`](file:///D:/Dev/GridNode/db/annotations/ui.yaml).
> **Confirmation Status**: Pure research for now; to be confirmed in game during live gameplay testing.

---

## 1. Executive Summary

Mirror's Edge Catalyst features two distinct timed run systems:
1. **Named Challenges**: Developer-crafted runs including **Dashes** ("Runner's Routes"), **Deliveries**, **GridNodes**, **Hackable Billboards**, and **MetaGrids**.
2. **User Generated Content (UGC) Time Trials**: Player-created runs defined by placement of start markers, checkpoints, and finish markers (alongside non-timed "Beat L.E. / ReachThis" location markers).

All challenge lifecycle events (start, finish, cancel, restart, stats upload) are dispatched via Frostbite's internal messaging infrastructure under the dedicated message category **`0x54776fc6`** (`kCategorySocialPlay` / `PamSocialPlay*Message`).

This document provides:
- The exact wire layout, byte offsets, and decoding logic for all Social Play message types.
- The mathematical reconstruction of exact run times and improvements from centisecond stat sets.
- Entity tracking and metadata resolution pipelines (`PamServerNamedChallengeEntity` and `PamServerChallengeMarkerEntity`).
- In-engine string localization via `localize` (`0x1436a8d40`).
- A clean, production-grade C++ typed wrapper interface (`onDash`, `onTimeTrial`).

---

## 2. Message Infrastructure (`kCategorySocialPlay`)

### 2.1 Message Categories & Manager Contexts
Every Frostbite message inherits from the base structure `fb::Message` (`0x30` bytes):

| Offset | Type | Member | Description |
|---|---|---|---|
| `+0x00` | `void*` | `__vftable` | Message virtual method table |
| `+0x08` | `uint32_t` | `category` | djb2-xor hash of category name (`0x54776fc6` = Social Play) |
| `+0x0c` | `uint32_t` | `typeId` | djb2-xor hash of message class name |
| `+0x10` | `uint32_t` | `realm` | `0xff` = any realm, `0` = client, `1` = server |

Messages in this category are dispatched on two game thread contexts:
- **Client Message Manager**: `*reinterpret_cast<uintptr_t*>(0x142401cb0) + 0x10`
- **Server Message Manager**: `*reinterpret_cast<uintptr_t*>(0x14253d670) + 0x10`

### 2.2 Listener Registration Interface
Listeners are registered using `MessageManager_registerListener` (`0x142a3ad40`):
```cpp
using RegisterFn = uint8_t (*)(uintptr_t manager, uint32_t category, void* listener, uint32_t realm);
// Signature: manager, category=0x54776fc6, listener, realm=0xff
```
A listener object requires a two-slot virtual function table:
- `slot 0`: `void onMessage(void* self, const void* message);`
- `slot 1`: `uint16_t priority(void* self);` (standard default returns `0`)

### 2.3 Base Class Hierarchy
All social play messages derive from `PamSocialPlayMessage`:
- Constructor: **`0x143474ae0`**
- Vtable: **`0x141b94668`**
- Constructor implementation:
```x86asm
0x143474ae0: push rbx
0x143474ae1: sub rsp, 0x20
0x143474ae5: mov rbx, rcx
0x143474ae8: call fb::Message_ctor           ; 0x142a37120 (category, typeId, realm)
0x143474aed: lea rax, [rip - 0x18e048c]      ; vtable 0x141b94668
0x143474af4: mov [rbx], rax
0x143474af7: xor eax, eax
0x143474af9: mov [rbx + 0x30], rax
0x143474afd: mov [rbx + 0x38], rax
0x143474b01: mov [rbx + 0x40], eax
0x143474b04: mov rax, rbx
0x143474b07: add rsp, 0x20
0x143474b0b: pop rbx
0x143474b0c: ret
```

---

## 3. Comprehensive Social Play Message Catalog

The executable initializes 35 distinct message types within category `0x54776fc6`. In the x64 binary, type IDs are calculated at compile time or disguised using arithmetic pairs (`mov r8d, X; lea r8d, [r8 +/- Y]`):

| TypeId | Message Class Name | Constructor VA | Vtable VA | Description / Key Fields |
|---|---|---|---|---|
| `0xd71d027f` | **`PamSocialPlayNamedChallengeFinishedMessage`** | **`0x144733a30`** | `0x141c38eb0` | Named challenge finish event (Dashes, etc.) |
| `0xc164590c` | **`PamSocialPlayNamedChallengeStartedMessage`** | **`0x144733a90`** | `0x141c38dc0` | Named challenge started (`+0x48 ContentId`) |
| `0x9a9a27c6` | **`PamSocialPlayNamedChallengeCancelledMessage`** | **`0x1447339e0`** | `0x141c38e10` | Named challenge aborted/abandoned (`+0x48 ContentId`) |
| `0xb7633230` | `PamSocialPlayNamedChallengeActivated` | `0x1447338e0` | `0x141c38d70` | Challenge activated in UI/world (`+0x48 ContentId`) |
| `0x606b598c` | `PamSocialPlayNamedChallengeTeleport` | `0x144733b50` | `0x141c39318` | Teleport player to challenge start marker |
| `0xa64f89ee` | `PamSocialPlayNamedChallengeStatsUploaded` | `0x144733af0` | `0x141c38cc0` | Leaderboard / telemetry stats uploaded |
| `0xa78a38f4` | `PamSocialPlayNamedChallengeBookmarkStatusRequest` | `0x144733930` | `0x141c39368` | Check if route is bookmarked by player |
| `0xe6fd16cc` | `PamSocialPlayNamedChallengeBookmarkStatusResult` | `0x144733980` | `0x141c393b8` | Bookmark state response (`+0x50` bool) |
| `0x955f5cbd` | `PamSocialPlayNamedChallengeUploadReplay` | `0x144733bc0` | `0x141c38e60` | Ghost/replay payload upload request |
| `0x9b35497c` | **`PamSocialPlayChallengeFinishedMessage`** | **`0x144733550`** | `0x141c38cd0` | Player-made Time Trial finish (`+0x48 Guid`, `+0x60 Time`) |
| `0x1cca484e` | **`PamSocialPlayStartChallengeMessage`** | **`0x144734060`** | `0x141c39408` | Start player-made Time Trial (`+0x48 Guid`) |
| `0x72f865f9` | **`PamSocialPlayRestartChallengeMessage`** | **`0x144733f10`** | `0x141c39458` | Restart player-made Time Trial (`+0x48 Guid`) |
| `0x924d6bd9` | `PamSocialPlayDownloadSharedContent` | `0x144733640` | `0x141c38d20` | Download UGC marker / route data |
| `0xd089d3e2` | `PamSocialPlayFetchSharedContents` | `0x1447336e0` | `0x141c39138` | Request nearby UGC routes |
| `0xfc61a69a` | `PamSocialPlayPinSharedContent` | `0x144733db0` | `0x141c391d8` | Pin challenge marker to HUD/map |
| `0x5a464701` | `PamSocialPlayUnpinSharedContent` | `0x144734120` | `0x141c39228` | Unpin challenge marker |
| `0x9a0dfb2b` | `PamSocialPlayRemoveSharedContent` | `0x144733e60` | `0x141c39188` | Delete/retire player UGC content |
| `0x33d42557` | `PamSocialPlaySocialTeleport` | `0x144733ff0` | `0x141c392c8` | Teleport to friend/social location |
| `0x855bb873` | `PamSocialPlayOnShowMap` | `0x144733c60` | `0x141c39548` | World map UI opened |
| `0x5b61f030` | `PamSocialPlayOnHideMap` | `0x144733c10` | `0x141c39558` | World map UI closed |
| `0x68f236ee` | `PamSocialPlayFriendsList` | `0x1447337a0` | `0x141c39068` | Origin friends roster payload |
| `0x052d5907` | `PamSocialPlayBlockedUsers` | `0x144733500` | `0x141c38fc8` | Blocked users roster |
| `0xe7d40012` | `PamSocialPlayBlockedListUpdated` | `0x1447334a0` | `0x141c38fb8` | Blocked list change notification |
| `0xa3118cf7` | `PamSocialPlayPermissionRequest` | `0x144733cb0` | `0x141c38f00` | UGC permission check request |
| `0xf1c3cee5` | `PamSocialPlayPermissionResponse` | `0x144733d20` | `0x141c38f68` | UGC permission response |
| `0xfc74c3ca` | `PamSocialPlayControl` | `0x1447335c0` | `0x141c38cb0` | General social control message |
| `0x85ebbf13` | `PamSocialPlayEchoCustomizationChanged` | `0x144733690` | `0x141c39528` | Runner echo visual customization updated |
| `0xaa327a65` | `PamSocialPlayInventoryChanged` | `0x144733840` | `0x141c39538` | Badges/echo customizations inventory |
| `0xa390e6be` | `PamSocialPlayRunnerKitOpened` | `0x144733f60` | `0x141c394d8` | Runner kit loot opening event |
| `0x1846085d` | `PamSocialPlayUgcFilterOptionChanged` | `0x1447340b0` | `0x141c39018` | Map UGC filter toggle |
| `0x320071b1` | `PamSocialPlayMapMarkerUpdates` | `0x144733890` | `0x141c390d0` | Map marker dynamic update stream |
| `0x36154959` | `PamSocialPlayFireEvent` | `0x144733770` | `0x141c39278` | Generic social entity event relay |
| `0x57d92a5d` | `PamSocialPlayRequestHackableBillboardLeader` | `0x144733ea0` | `0x141c394a8` | Billboard top player lookup |
| `0x82ad8c5d` | `PamSocialPlayRefreshHackableBillboardLeaderCache`| `0x144733df0` | `0x141c394b8` | Invalidate billboard leader cache |
| `0xf33d51ff` | `PamSocialPlayHackableBillboardsLeader` | `0x1447337f0` | `0x141c394c8` | Billboard leader roster payload |

---

## 4. Deep-Dive: Named Challenge Messages (Dashes)

### 4.1 `PamSocialPlayNamedChallengeFinishedMessage` (`0xd71d027f`)
When a player crosses the finish trigger of a Dash or delivery, the server evaluates their run time against existing stats (`0x1439deb10`) and posts `PamSocialPlayNamedChallengeFinishedMessage`.

#### Payload Layout (Size: `0x68` bytes)
```
+0x00: [0x30 bytes] Base fb::Message Header
       +0x08: category = 0x54776fc6
       +0x0c: typeId   = 0xd71d027f
       +0x10: realm    = 0xff
+0x30: [0x18 bytes] PamSocialPlayMessage Base Members
+0x48: uint32_t ContentId   - Unique identifier of the challenge
+0x4c: uint32_t Index       - Stat set index (0 = personal record)
+0x50: int32_t  Delta       - Improvement/difference in centiseconds
+0x54: uint8_t  HadRecord   - 1 if previous personal best existed, 0 if first run
+0x58: int64_t  Value       - Time/score in centiseconds (hundredths of a second)
+0x60: uint8_t  NewRecord   - 1 if this run set a new personal record, 0 otherwise
```

#### Run Time & Improvement Calculation
The Frostbite server handles `Value` and `Delta` with conditional semantics depending on `NewRecord` and `HadRecord`:
- **When a new personal record is set (`newRecord == true`)** or **on the player's first run (`hadRecord == false`)**:
  - `Value` holds the **new run's time** in centiseconds.
  - `Delta` holds the negative improvement ($t_{\text{new}} - t_{\text{old}} < 0$).
- **When the player fails to beat their record (`newRecord == false && hadRecord == true`)**:
  - `Value` holds the **existing personal best record**.
  - `Delta` holds the positive delta ($t_{\text{run}} - t_{\text{best}} > 0$).

Therefore, the exact run time is formulated as:
$$\text{runCentiseconds} = (\text{newRecord} \lor \neg\text{hadRecord}) \mathbin{?} \text{value} : \text{value} + \text{delta}$$
$$\text{timeMs} = \text{runCentiseconds} \times 10$$

The improvement over personal best is:
$$\text{improvementMs} = (\text{newRecord} \land \text{hadRecord} \land \text{delta} < 0) \mathbin{?} (-\text{delta}) \times 10 : 0$$

> [!TIP]
> The game sends one `NamedChallengeFinishedMessage` per stat set (e.g. personal best, friend comparison). `Index == 0` corresponds to the player's own record. Subsequent stat set messages dispatched within 5 seconds for the same `ContentId` must be filtered out as duplicates.

---

## 5. Deep-Dive: User Generated Content (UGC) Time Trials

### 5.1 `PamSocialPlayChallengeFinishedMessage` (`0x9b35497c`)
Player-created time trials do not send `NamedChallengeFinishedMessage`. Instead, they fire `PamSocialPlayChallengeFinishedMessage`:
- Constructor: **`0x144733550`**
- Vtable: **`0x141c38cd0`**

#### Payload Layout (Size: `0x78` bytes)
```
+0x00: [0x30 bytes] Base fb::Message Header (category=0x54776fc6, typeId=0x9b35497c)
+0x30: [0x18 bytes] PamSocialPlayMessage Base Members
+0x48: uint8_t[24] TimeTrialGuid - 24-byte unique UGC ContentId (PamSharedContentId)
+0x60: uint32_t    Time          - Elapsed run time in centiseconds (10 ms units)
```
$$\text{timeMs} = \text{Time} \times 10$$

### 5.2 Distinguishing Timed Runs vs "Beat L.E." / ReachThis
Both Time Trials and Beat L.E. ("ReachThis") markers trigger `PamSocialPlayStartChallengeMessage` (`0x1cca484e`) when activated.
To avoid incorrectly treating a location bookmark or billboard hack as a race against the clock, the challenge marker's data must be inspected:
- `PamServerChallengeMarkerEntity` (`0x141c7e3e8`) -> `+0x28 data` (`PamChallengeMarkerEntityData*`)
- `data + 0x68 CheckpointOrder` (`int32_t`):
  - `CheckpointOrder == 0`: **ReachThis / Beat L.E.** (Reach location; no checkpoints; NOT a timed run).
  - `CheckpointOrder != 0`: **Timed Time Trial** (Sequential or unordered checkpoints).

---

## 6. Challenge Metadata & Entity Resolution Pipeline

Messages only contain a numeric `ContentId` or 24-byte `Guid`. To resolve the human-readable route name and challenge category, GridNode tracks level entities created via Frostbite entity creator hooks.

### 6.1 Named Challenge Entity: `PamServerNamedChallengeEntity`
- Vtable: **`0x141c7ec70`**
- Creator Slot: `0x141c7ee00` (hookable via creator vtable)
- Original Factory: `0x143a834b0`

#### Entity Memory Layout
```
entity + 0x00: vtable = 0x141c7ec70
entity + 0x80: PamNamedChallengeData*
       data + 0x10: const char* Sid          ; Localization String ID, e.g. "ID_NC_THEVIEW02"
       data + 0x58: const char* MainStatCode  ; Stat identifier, e.g. "ch_rrt_tv2_time"
       data + 0x98: uint32_t    ContentId     ; Matches message +0x48
```

### 6.2 Challenge Type Deduction
The prefix of `MainStatCode` identifies the exact challenge activity:

| Prefix | Category | Activity Description |
|---|---|---|
| `ch_rrt_` | **Dash** | Runner's Route (timed race against author & leaderboard) |
| `ch_dlvry_` | **Delivery** | Fragile / timed delivery mission |
| `ch_gnd_` | **GridNode** | GridNode infiltration puzzle / run |
| `ch_hbb_` | **Hackable Billboard** | Billboard hack (score/completion, not timed) |
| `ch_mgd_` | **MetaGrid** | MetaGrid node challenge |
| `ch_aslt_` | **Assault** | Combat / security assault route |

### 6.3 Route Name In-Engine Localization
The game resolves `Sid` (e.g. `"ID_NC_THEVIEW02"`) into localized user text (e.g. `"The View"`) using `localize` at **`0x1436a8d40`**:
```cpp
using LocalizeFn = bool (*)(char* outBuf, uint32_t capacity, const char* sid, void* args);
// Example:
char nameBuf[256] = {};
void* noArgs = nullptr;
bool ok = reinterpret_cast<LocalizeFn>(0x1436a8d40)(nameBuf, sizeof(nameBuf), sid, &noArgs);
```
If localization returns false or is uninitialized, the fallback is to strip prefixes (`"ID_NC_"`, `"ID_BILLBOARD_"`, `"ID_"`) to extract the route token.

### 6.4 UGC Time Trial Entity: `PamServerChallengeMarkerEntity`
- Vtable: **`0x141c7e3e8`**
- Creator Slot: `0x141c7e9f0` (Original: `0x143a83460`)
- Marker Layout:
  - `marker + 0x80`: `PamSharedContentId` (24 bytes matching `TimeTrialGuid`)
  - `marker + 0xb8`: `const char* Name` (UTF-8 string authored by the player)

---

## 7. Typed C++ Event Interface Design

Here is the complete, high-performance, drop-in event interface suitable for GridNode integration:

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <mutex>

enum class ChallengeKind : uint8_t {
    Dash,
    Delivery,
    GridNode,
    TimeTrial,
    Billboard,
    MetaGrid,
    Assault,
    Other
};

struct DashEvent {
    uint32_t      contentId;
    ChallengeKind kind;
    std::string   routeName;
    std::string   statCode;
    int64_t       timeMs;
    int64_t       improvementMs;
    bool          isNewRecord;
    bool          hadPreviousRecord;
};

struct TimeTrialEvent {
    uint8_t       guid[24];
    std::string   routeName;
    uint32_t      timeMs;
};

using DashCallback = std::function<void(const DashEvent&)>;
using TimeTrialCallback = std::function<void(const TimeTrialEvent&)>;

namespace ChallengeEvents {
    // Subscription API
    void onDash(DashCallback cb);
    void onTimeTrial(TimeTrialCallback cb);

    // Lifecycle ticks
    void init();
    void clientTick();
    void serverTick();
}
```

### Event Implementation Logic
```cpp
// Inside ChallengeEvents implementation
void onMessageReceived(const uint8_t* msg) {
    uint32_t category = *reinterpret_cast<const uint32_t*>(msg + 0x08);
    uint32_t typeId   = *reinterpret_cast<const uint32_t*>(msg + 0x0c);
    if (category != 0x54776fc6) return;

    if (typeId == 0xd71d027f) { // NamedChallengeFinished
        uint32_t contentId = *reinterpret_cast<const uint32_t*>(msg + 0x48);
        uint32_t index     = *reinterpret_cast<const uint32_t*>(msg + 0x4c);
        if (index != 0) return; // Only process player personal record set

        int32_t delta      = *reinterpret_cast<const int32_t*>(msg + 0x50);
        bool    hadRecord  = msg[0x54] != 0;
        int64_t valCs      = *reinterpret_cast<const int64_t*>(msg + 0x58);
        bool    newRecord  = msg[0x60] != 0;

        int64_t runCs = (newRecord || !hadRecord) ? valCs : valCs + delta;
        if (runCs <= 0) return;

        int64_t timeMs = runCs * 10;
        int64_t improvementMs = (newRecord && hadRecord && delta < 0) ? static_cast<int64_t>(-delta) * 10 : 0;

        // Resolve metadata from tracked PamServerNamedChallengeEntity
        auto meta = resolveNamedChallenge(contentId);

        DashEvent evt{
            contentId,
            meta.kind,
            meta.localizedTitle,
            meta.statCode,
            timeMs,
            improvementMs,
            newRecord,
            hadRecord
        };

        for (const auto& cb : g_dashCallbacks) cb(evt);
    }
    else if (typeId == 0x9b35497c) { // ChallengeFinished (UGC Time Trial)
        const uint8_t* guid = msg + 0x48;
        if (getCheckpointOrder(guid) == 0) return; // Ignore Beat L.E. / ReachThis

        uint32_t cs = *reinterpret_cast<const uint32_t*>(msg + 0x60);
        if (cs == 0 || cs > 360000) return; // Sanity filter: max 1 hour

        TimeTrialEvent evt;
        memcpy(evt.guid, guid, 24);
        evt.timeMs = cs * 10;
        evt.routeName = resolveTimeTrialName(guid);

        for (const auto& cb : g_timeTrialCallbacks) cb(evt);
    }
}
```

---

## 8. Open Research & Next Steps

> [!IMPORTANT]
> The following areas require additional investigation once live gameplay verification is underway:
> 1. **Intermediate Checkpoint Split Events**: Investigating whether checkpoint triggers fire `PamSocialPlayFireEvent` (`0x36154959`) or sub-bus entity events to track live split timing deltas during an active run.
> 2. **Ghost / Replay Upload Format**: Decoding the payload layout passed to `PamSocialPlayNamedChallengeUploadReplay` (`0x955f5cbd`) to capture full trajectory checkpoints directly from official runs.
> 3. **Dynamic UGC Checkpoint Injection**: Evaluating whether `PamServerChallengeMarkerEntity_create` can be programmatically invoked to spawn custom user-designed time trials on the fly without game server round-trips.
