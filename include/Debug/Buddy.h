#pragma once

#include <cstdint>

// Solo testing aid: a hotkey toggles a "buddy" NPC that the game's own AI drives
// (combat, pathing), set as the player's teammate and kept close to the player.
// It stands in for "someone else is here" while only one PC is available.
//
// It does NOT exercise PULSAR's co-op sync: the buddy is a plain local NPC, not
// the remote-player clone, and nothing about it goes over the network. Refused
// while a peer is connected so it never leaks into a real session.
namespace Pulsar::Buddy {

    // Game-thread tick: edge-detects the hotkey, spawns/despawns, keeps it following.
    // a_baseFormID: Starfield.esm base NPC to spawn; 0 = a copy of the player.
    void Tick(int a_hotkeyVK, std::uint32_t a_baseFormID);

}  // namespace Pulsar::Buddy
