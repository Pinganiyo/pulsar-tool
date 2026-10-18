#pragma once

#include <cstddef>
#include <cstdint>

namespace Pulsar::QuestSync {

    // Phase 1 - register sinks on the global TESQuestStageEvent /
    // TESQuestStartStopEvent BSTEventSource singletons (RVA 0x5978238 +
    // 0x5978288 respectively, both verified against SFSE 0.2.19
    // GameEvents.h:1846-1849). Resolves the native stage-set worker
    // FUN_140D0E1F0 @ RVA 0xD0DAC0 once at startup. Safe to call
    // multiple times; only first call performs registration.
    void Init();

    // Detach our sinks. Currently a no-op (engine event sources outlive
    // the plugin); kept for symmetry with other sync modules.
    void Shutdown();

    // Network-thread receive entry. Dispatches PKT_QUEST_STAGE / _START /
    // _STOP to the per-packet handlers. Apply runs on the network thread -
    // the native worker is thread-safe enough for an isolated stage write
    // (no allocation, no cell-streamer touch); if we later observe races
    // we'll switch to a game-thread queue. Mirrors LockSync's pending-vec
    // pattern if needed.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // Reset cached per-quest "last seen" map on disconnect so a reconnect
    // doesn't suppress legitimate first-time stage broadcasts.
    void Reset();

    // Missed-stage replay (0.5.0). The peer just disconnected: remember when, so
    // stages set from now on can be replayed if it reconnects without a bridge.
    void NotePeerDisconnected();

    // Host: the peer reconnected and the save bridge was skipped (reconnect
    // within 90 s) - send the stages set since it dropped (PKT_QUEST_STAGE_REPLAY).
    void ReplayMissedStagesToPeer();

    // A save bridge re-synced the worlds: nothing to replay.
    void ForgetMissedStages();

}  // namespace Pulsar::QuestSync
