#pragma once

#include <cstdint>
#include <cstddef>

// F.2-v2 (2026-06-11): live worn-equipment sync.
// Each machine broadcasts what its LOCAL player currently wears (current
// weapon + every equipped ARMO piece), with plugin-encoded FormIDs and the
// M.2 mod array + sentinel per item. The receiver resolves cross-plugin and
// re-equips the partner CLONE via RemotePlayerManager, applying the mods to
// the clone's inventory items BEFORE equipping so visuals (skins, attachments,
// tier) match. Sent on change + 10 s heartbeat.
//
// Replaces the 2026-04-25-disabled PKT_EQUIP / PKT_EQUIPMENT paths.
namespace Pulsar::RemotePlayer::EquipSync
{
    // partial re-enable (2026-06-19): ARMOR sync ON, WEAPON sync OFF.
    // The weapon equip-apply (DoEquipWeapon) mutates the clone's animation graph
    // (draw/holster, weapon-type movement graph) → both-down pure-virtual crash
    // (the reason F.2-v2 was fully disabled). Armor pieces are biped-slot visuals
    // and don't touch the locomotion graph, so they're safe to apply. These gate
    // BOTH the send (don't transmit weapons) AND the apply (RemotePlayerManager
    // skips DoEquipWeapon). Flip kSyncWeapons true only after a safe weapon-equip
    // graph re-init exists.
    // 2026-06-20: BOTH armor + weapon ON, downed-gates removed. Testing the
    // hypothesis that the weapon-holster graph kick is what PLAYS the (armor-frozen)
    // bleedout anim - the gate-only builds (weapon OFF) blocked it; the weapon-ON
    // build played it. If both-on shows the bleedout, weapon sync was the enabler.
    inline constexpr bool kSyncArmor   = true;

    // 0.6.0: weapon sync is a setting (SyncCloneWeapons, default ON = unchanged)
    // so a tester hitting the clone anim-graph crash can turn it off without a
    // rebuild. Gates both the send and the apply, like kSyncWeapons did.
    bool SyncWeapons();

    // Game thread (Main.cpp tick, connected only) - collects the local
    // player's equipment, sends PKT_EQUIP_V2 on change / heartbeat.
    void TickSendLocalEquipment();

    // ENet receive thread - parses PKT_EQUIP_V2, resolves cross-plugin and
    // queues the set for the game-thread apply in RemotePlayerManager.
    void OnEquipV2Received(const std::uint8_t* a_data, std::size_t a_length);
}
