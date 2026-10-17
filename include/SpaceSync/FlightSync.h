#pragma once

// Phase L.3 V2 V1 - Coop sync of pilot flight state.
//
// Tap into ShipHudDataModel::StickDataProvider - the engine-managed property
// block that holds throttle, yaw/pitch input, boost, reverse, cruise level
// during flight. We hook the model's constructor (FUN_141556a50) to capture
// each new instance pointer; per-tick we read the fields on the pilot side
// and broadcast PKT_FLIGHT_STATE; receiver writes the same fields into its
// local instance so the cockpit mirrors host's flight controls.
//
// Read-only on the data layout - no engine state mutation beyond the same
// TUIValue<T> slots the engine itself writes. No allocations, no thread
// affinity hazards (all writes from game-thread tick).

#include <cstddef>
#include <cstdint>

namespace Pulsar::FlightSync {

void Init();
void Shutdown();

// Game-thread tick - host sends, client applies any pending state.
void Tick();

// Network thread - queues received state for game-thread application.
void OnPacketReceived(const std::uint8_t* data, std::size_t length);

// L.5 true while the post-load force-unseat retry-loop is active on
// client. Caller (Main.cpp teleport apply) defers SetPosition while
// the engine still has the player seated, otherwise the chair re-snaps the
// player back and we get an infinite push/snap loop.
bool IsPostLoadUnseatInProgress();

// Drop every cached Havok body pointer. Called on save receipt, before the
// bridge load frees the bodies they point at.
void ResetBodyCache();

}  // namespace Pulsar::FlightSync
