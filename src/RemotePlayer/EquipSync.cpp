#include "PCH.h"
#include "Config.h"
#include "RemotePlayer/EquipSync.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "WorldSync/LootSync.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/PluginLookup.h"
#include "Coop/CoopMortality.h"   // skip equip SEND while locally downed

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/B/BGSInventoryItem.h>
#include <RE/B/BSContainer.h>
#include <RE/T/TESForm.h>
#include <RE/F/FormTypes.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::RemotePlayer::EquipSync
{
    bool SyncWeapons() { return Pulsar::GetConfig().syncCloneWeapons; }

    namespace
    {
        struct CollectedItem
        {
            std::uint32_t formID = 0;
            bool          isWeapon = false;
            std::uint16_t sentinel = 0xFFFF;
            std::vector<std::uint32_t> mods;
        };

        // FNV-1a over the full collected state - cheap change detection.
        std::uint64_t HashEquipment(const std::vector<CollectedItem>& a_items)
        {
            std::uint64_t h = 0xcbf29ce484222325ull;
            auto mix = [&h](std::uint64_t v) {
                h ^= v;
                h *= 0x100000001b3ull;
            };
            for (const auto& it : a_items) {
                mix(it.formID);
                mix(it.isWeapon ? 1u : 0u);
                mix(it.sentinel);
                for (auto m : it.mods) mix(m);
            }
            return h;
        }
    }  // anonymous

    void TickSendLocalEquipment()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        // (2026-06-20): the local-downed SEND-gate was REMOVED - in-game logs
        // proved the send was already suppressed during downed yet the bleedout was
        // still blocked, so the send is not the cause. Re-enabling full equip flow
        // (incl. the weapon holster) which is what triggers the bleedout anim.

        static std::uint64_t s_lastScanMs = 0;
        const std::uint64_t now = ::GetTickCount64();
        if (now - s_lastScanMs < 2000) return;  // scan every 2s
        s_lastScanMs = now;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Collect the worn set: current weapon + every equipped ARMO piece.
        std::vector<CollectedItem> items;
        player->ForEachEquippedItem(
            [&](const RE::BGSInventoryItem& a_item) -> RE::BSContainer::ForEachResult {
                if (a_item.object && items.size() < Network::kEquipV2MaxItems) {
                    const auto ft = a_item.object->GetFormType();
                    if (SyncWeapons() && a_item.object->IsWeapon()) {
                        items.push_back({ a_item.object->GetFormID(), true, 0xFFFF, {} });
                    } else if (kSyncArmor && ft == RE::FormType::kARMO) {
                        items.push_back({ a_item.object->GetFormID(), false, 0xFFFF, {} });
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });

        // Mods + sentinel per item (M.2 primitives, SEH-guarded inside).
        for (auto& it : items) {
            Pulsar::WorldSync::Loot::ReadItemMods(
                static_cast<RE::TESObjectREFR*>(player), it.formID, it.mods, it.sentinel);
            if (it.mods.size() > Network::kEquipV2MaxModsPerItem) {
                it.mods.resize(Network::kEquipV2MaxModsPerItem);
            }
        }

        // Send on change, refresh every 10s (covers clone respawn / late join).
        static std::uint64_t s_lastHash   = 0;
        static std::uint64_t s_lastSendMs = 0;
        const std::uint64_t hash = HashEquipment(items);
        if (hash == s_lastHash && now - s_lastSendMs < 10000) return;
        s_lastHash   = hash;
        s_lastSendMs = now;

        std::vector<std::uint8_t> buf;
        buf.reserve(1 + sizeof(Network::EquipV2Header)
                    + items.size() * (sizeof(Network::EquipV2Item)
                                      + Network::kEquipV2MaxModsPerItem
                                            * sizeof(Network::LootDropMod)));
        buf.push_back(static_cast<std::uint8_t>(Network::PKT_EQUIP_V2));

        Network::EquipV2Header hdr{};
        hdr.itemCount  = static_cast<std::uint8_t>(items.size());
        hdr.playerSlot = net.IsHost() ? 0 : 1;
        const auto* hp = reinterpret_cast<const std::uint8_t*>(&hdr);
        buf.insert(buf.end(), hp, hp + sizeof(hdr));

        for (const auto& it : items) {
            Network::EquipV2Item w{};
            w.base        = it.formID;
            w.localID     = NPCSync::PluginLookup::FormIDToLocalID(it.formID);
            NPCSync::PluginLookup::FormIDToPluginName(it.formID, w.plugin, sizeof(w.plugin));
            w.isWeapon    = it.isWeapon ? 1 : 0;
            w.modCount    = static_cast<std::uint8_t>(it.mods.size());
            w.modSentinel = it.sentinel;
            const auto* wp = reinterpret_cast<const std::uint8_t*>(&w);
            buf.insert(buf.end(), wp, wp + sizeof(w));

            for (auto modID : it.mods) {
                Network::LootDropMod m{};
                m.base    = modID;
                m.localID = NPCSync::PluginLookup::FormIDToLocalID(modID);
                NPCSync::PluginLookup::FormIDToPluginName(modID, m.plugin, sizeof(m.plugin));
                const auto* mp = reinterpret_cast<const std::uint8_t*>(&m);
                buf.insert(buf.end(), mp, mp + sizeof(m));
            }
        }

        net.Send(buf.data(), buf.size(), /*reliable=*/true);
        PLOG_INFO("[PULSAR] F.2v2 equip TX: {} items, {} bytes"sv,
                  hdr.itemCount, static_cast<std::uint32_t>(buf.size()));
    }

    void OnEquipV2Received(const std::uint8_t* a_data, std::size_t a_length)
    {
        using namespace Pulsar::Network;
        if (!a_data || a_length < 1 + sizeof(EquipV2Header)) return;

        EquipV2Header hdr{};
        std::memcpy(&hdr, a_data + 1, sizeof(hdr));
        if (hdr.itemCount > kEquipV2MaxItems) return;

        std::size_t off = 1 + sizeof(EquipV2Header);
        std::vector<EquipV2Entry> entries;
        entries.reserve(hdr.itemCount);

        for (std::uint8_t i = 0; i < hdr.itemCount; ++i) {
            if (off + sizeof(EquipV2Item) > a_length) return;  // malformed
            EquipV2Item w{};
            std::memcpy(&w, a_data + off, sizeof(w));
            off += sizeof(w);
            if (w.modCount > kEquipV2MaxModsPerItem) return;
            if (off + static_cast<std::size_t>(w.modCount) * sizeof(LootDropMod) > a_length) return;

            w.plugin[sizeof(w.plugin) - 1] = '\0';
            std::uint32_t resolved = 0;
            if (w.plugin[0] != '\0') {
                resolved = NPCSync::PluginLookup::ResolveCrossPlugin(w.plugin, w.localID);
            }
            if (!resolved) resolved = w.base;

            EquipV2Entry e{};
            e.formID      = resolved;
            e.isWeapon    = w.isWeapon != 0;
            e.modSentinel = w.modSentinel;
            e.mods.reserve(w.modCount);
            for (std::uint8_t mi = 0; mi < w.modCount; ++mi) {
                LootDropMod m{};
                std::memcpy(&m, a_data + off, sizeof(m));
                off += sizeof(m);
                m.plugin[sizeof(m.plugin) - 1] = '\0';
                std::uint32_t rm = 0;
                if (m.plugin[0] != '\0') {
                    rm = NPCSync::PluginLookup::ResolveCrossPlugin(m.plugin, m.localID);
                }
                if (!rm) rm = m.base;
                e.mods.push_back(rm);
            }
            entries.push_back(std::move(e));
        }

        PLOG_INFO("[PULSAR] F.2v2 equip RX: {} items (slot {})"sv,
                  hdr.itemCount, hdr.playerSlot);
        GetRemotePlayerManager().RequestEquipV2(std::move(entries));
    }
}  // namespace Pulsar::RemotePlayer::EquipSync
