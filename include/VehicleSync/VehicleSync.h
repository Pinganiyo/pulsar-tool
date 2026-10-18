#pragma once

#include <cstddef>
#include <cstdint>

namespace RE {
    class Actor;
}

namespace Pulsar::VehicleSync
{
    // Wire up once at plugin load.
    bool Init();

    // L.7 V13g - clear all cached clone/FURN pointers. Called by
    // WorldSync::OnSaveReceived alongside NPCSync::Reset() so the post-
    // save-load tick doesn't dereference dangling FURN pointers from the
    // pre-save session.
    void Reset();

    // Called every ~33 ms from the game thread (from Main.cpp).
    // Host: reads player->occupiedFurniture, sends throttled PKT_VEHICLE_POS.
    // Client: drains pending packets, runs matcher + SetPosition.
    void OnGameThreadTick();

    // Client: invoked by the network receive path in NetworkHost.cpp when a
    // PKT_VEHICLE_POS arrives. Queues the update for the game-thread tick.
    void OnPacketReceived(const uint8_t* a_data, std::size_t a_length);

    // Client: invoked by RemotePlayerManager when a PKT_POSITION with the
    // seated bit (animFlags bit 10) is decoded. Drives the sit/stand state
    // machine on the local clone. On 0→1 transition: finds nearest FURN to
    // the clone and activates it (engine-native sit animation). Each tick
    // while seated: SetPosition the FURN to the clone's position so the
    // seat-anchored clone moves naturally when host flies. On 1→0: no-op
    // (Bethesda's engine stands the actor up when the FURN is no longer
    // being driven).
    void OnRemoteSeatedUpdate(RE::Actor* a_clone, bool a_seated, float a_x, float a_y, float a_z);

    // Query: is the given clone currently in our seated state machine?
    // RemotePlayerManager uses this to skip clone SetPosition so the
    // engine's sit-anchor (tying clone to FURN) isn't yanked each tick.
    bool IsCloneSeated(RE::Actor* a_clone);

    // Client: refuse activating the vehicle the host is driving (formal entry
    // crashes the client). Shows a message (rate-limited). See RidePolicy.h.
    bool ShouldBlockClientEntry(RE::TESObjectREFR* a_ref);
}
