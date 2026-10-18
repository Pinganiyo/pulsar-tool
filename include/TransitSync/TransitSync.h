#pragma once

#include <cstddef>
#include <cstdint>

namespace Pulsar::TransitSync {

    // Phase K.1 - Universal Transit-Event Sync.
    //
    // Detection: CellDiag::Tick observes player.parentCell.formID changes
    // and worldspace changes. When a non-trivial transition is detected
    // (i.e., the player just finished a load-screen and entered a different
    // cell), CellDiag calls TransitSync::OnLocalTransit. The packet is
    // built and broadcast to the peer.
    //
    // Receive: NetworkHost dispatches PKT_TRANSIT to OnPacketReceived.
    // K.1 only logs the event for now; K.2 will add auto-follow.

    void Init();

    // Called by LootSync's Activate hook when the local player activates a
    // kDOOR. The most recent value is paired with the next loadMenu open
    // and broadcast as TransitData::sourceDoorFormID - receiver can then
    // Activate the same door to follow through with proper engine cell-load
    // (rather than blind SetPosition which only handles exterior).
    void NoteLocalDoorActivation(std::uint32_t a_doorFormID);
    std::uint32_t ConsumeRecentDoorActivation();  // 0 if nothing recent

    // Re-entry guard. K.2 V3 calls TESForm::vt[0x54] Activate directly on
    // the door REFR to follow peer through it. That call fires the engine's
    // door-trigger pipeline which includes OUR LootSync HookedActivate.
    // While this flag is true, LootSync's hook MUST NOT call
    // NoteLocalDoorActivation - otherwise our auto-follow's own activate
    // would loop back as a transit-broadcast ping.
    bool IsInAutoFollowActivate();

    // Called by CellDiag from the game-thread tick when the local player
    // just finished a load-screen and ended up in a different cell. Sender
    // classifies the transition based on cell flags + worldspace change
    // and broadcasts PKT_TRANSIT to the peer.
    //
    // Inputs are sampled from PlayerCharacter just AFTER LoadingMenu close.
    // Pass 0 for prevCellFormID / prevWorldspaceFormID if unknown (initial
    // session start). sourceDoorFormID is automatically pulled from the
    // recent-door-activation note (see NoteLocalDoorActivation) - pass
    // 0u to skip the door-based follow path.
    void OnLocalTransit(std::uint32_t prevCellFormID,
                         std::uint32_t newCellFormID,
                         std::uint32_t prevWorldspaceFormID,
                         std::uint32_t newWorldspaceFormID,
                         float x, float y, float z, float rotZ,
                         bool destIsInterior);

    // Network-thread entry. Validates + parses + posts to a small queue
    // for game-thread processing. K.1 just logs.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // Game-thread tick - drains the receive queue, logs.
    // K.2 will hand off to auto-follow handlers here.
    void Tick();

    // Diagnostic accessor - last peer-reported transit (formID dest cell).
    // Future K.2 / RemotePlayerManager can read this to drive auto-follow.
    std::uint32_t GetLastPeerDestCellFormID();
    std::uint32_t GetLastPeerDestWorldspaceFormID();

    // ------- K.2 Auto-Follow (Toggle-Hybrid) -------
    //
    // Default ON - when peer's PKT_TRANSIT arrives the local player auto-
    // teleports to peer's destination position. Hotkey (F7) toggles the
    // mode at runtime so the user can split off intentionally.
    //
    // Caveats (Gerüst V1):
    // SetPosition handles exterior cell-streaming. Static interior
    //     cells (Lodge, Ship) load when target coords map there. Procedural
    //     interiors (FF-prefix) end up as the local instance, not the peer's.
    // The peer's destPos is sampled right after their loadMenu CLOSED,
    //     so it corresponds to the correct cell on their side. We trust that.
    //
    // Future V2: route through Papyrus MoveTo on a target ref for interiors
    // when peer.destCellFormID is a static plugin form.

    bool IsFollowEnabled();
    void ToggleFollow();      // flips the bit + logs new state
    void SetFollow(bool a_on);

    // Polled from Main.cpp permanent task - checks edge of the supplied VK.
    // Default 0x76 = VK_F7 to preserve historical behaviour; the binding is
    // overridable via PULSAR.ini's HotkeyAutoFollow.
    void TickFollowHotkey(int a_vkey = 0x76);

    // Game-thread apply: if follow is ON and a pending peer destination is
    // set, teleport the local player. Called from Main.cpp tick.
    void TickFollowApply();

    // Client -> host "please regroup me" (PKT_BRIDGE_REQUEST), 20 s cooldown.
    // Used by client F8 and by the automatic space-divergence watch.
    void RequestHostBridgeFromClient(const char* a_why);

    // Client game-thread tick of the automatic space regroup (Coop/DivergenceWatch.h).
    void TickAutoRegroup(bool a_enabled);

    // A save bridge just arrived (WorldSync::OnSaveReceived): worlds re-synced.
    void NotifyBridged();

}  // namespace Pulsar::TransitSync
