#include "PCH.h"
#include "Coop/TraitAliases.h"
#include "Coop/TraitAliasesGame.h"

#include "Network/NetworkHost.h"
#include "InventorySync/DirectRestore.h"

#include <RE/B/BGSPerk.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/ProcessLists.h>
#include <RE/T/TESQuest.h>

#include <Windows.h>
#include <array>

using namespace std::string_view_literals;

// Client side of Coop/TraitAliases.h: in the host's world, force-fill the
// background-trait NPC aliases the HOST's TraitQuest cleared but this player's
// own traits need (e.g. Andy Singh for an Enlightened client next to a
// Universal host). Uses ReferenceAlias.ForceRefTo on the alias object read
// from TraitQuest's fragment-script variables; every step is checked and
// logged, nothing is assumed about the result.
namespace Pulsar::Coop::TraitAliases {

    namespace {
        std::array<bool, kAliases.size()> g_done{};
        std::uint64_t                     g_lastTickMs = 0;
        constexpr std::uint64_t           kTickMs = 2000;

        bool SafeHasPerk(RE::PlayerCharacter* a_player, std::uint32_t a_perk)
        {
            __try {
                auto* pk = a_player->perks;
                if (pk && pk->perkRanks) {
                    for (const auto& prd : *pk->perkRanks)
                        if (prd.perk && prd.perk->formID == a_perk) return true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return false;
        }

        Traits ReadTraits(RE::PlayerCharacter* p)
        {
            Traits t;
            t.extrovert   = SafeHasPerk(p, 0x00227FD7);
            t.introvert   = SafeHasPerk(p, 0x00227FD8);
            t.taskmaster  = SafeHasPerk(p, 0x00227FE0);
            t.universal   = SafeHasPerk(p, 0x00227FD1);
            t.enlightened = SafeHasPerk(p, 0x00227FD2);
            t.serpent     = SafeHasPerk(p, 0x00227FD0);
            t.neon        = SafeHasPerk(p, 0x00227FD3);
            t.fc          = SafeHasPerk(p, 0x00227FD5);
            t.uc          = SafeHasPerk(p, 0x00227FD4);
            return t;
        }

        // ── nearby unique actor by base (high-process walk, CoopMortality pattern) ──
        RE::Actor* ResolveHighActor(RE::ProcessLists* a_pl, std::uint32_t a_i)
        {
            RE::NiPointer<RE::Actor> sp = a_pl->highActorHandles[a_i].get();
            return sp.get();
        }
        RE::Actor* SafeResolveHighActor(RE::ProcessLists* a_pl, std::uint32_t a_i)
        {
            __try { return ResolveHighActor(a_pl, a_i); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        }
        bool SafeRawHandle(RE::ProcessLists* a_pl, std::uint32_t a_i, std::uint32_t& a_h)
        {
            __try {
                auto* base = reinterpret_cast<std::uint32_t*>(a_pl->highActorHandles.data());
                if (!base) return false;
                a_h = base[a_i];
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        std::uint32_t SafeCount(RE::ProcessLists* a_pl)
        {
            __try { return static_cast<std::uint32_t>(a_pl->highActorHandles.size()); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }
        RE::Actor* FindLoadedActor(std::uint32_t a_npcBase)
        {
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!pl) return nullptr;
            std::uint32_t n = SafeCount(pl);
            if (n > 512) n = 512;
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint32_t h = 0;
                if (!SafeRawHandle(pl, i, h) || h == 0 || h == 0xFFFFFFFFu) continue;
                auto* a = SafeResolveHighActor(pl, i);
                if (!a) continue;
                auto* npc = a->GetNPC();
                if (npc && npc->formID == a_npcBase) return a;
            }
            return nullptr;
        }

        // ── TraitQuest alias -> ForceRefTo ──
        bool ForceFill(const AliasInfo& a_alias, RE::Actor* a_actor)
        {
            auto* game = RE::GameVM::GetSingleton();
            auto* vm = game ? game->GetVM() : nullptr;
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(kTraitQuest);
            if (!vm || !quest) return false;

            const auto& handles = vm->GetObjectHandlePolicy();
            const auto handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::TESQuest>(), quest);
            if (handle == handles.EmptyHandle()) return false;

            RE::BSTSmartPointer<RE::BSScript::Object> questObj;
            if (!vm->FindBoundObject(handle, kTraitQuestScript, false, questObj, false) || !questObj) {
                PLOG_WARN("[PULSAR] TraitAliases: TraitQuest script object not found"sv);
                return false;
            }
            RE::BSScript::Variable aliasVar;
            if (!vm->GetVariableValue(questObj, a_alias.varIndex, aliasVar) ||
                !aliasVar.is<RE::BSScript::Object>()) {
                PLOG_WARN("[PULSAR] TraitAliases: Alias_{} variable not readable"sv, a_alias.name);
                return false;
            }
            auto aliasObj = RE::BSScript::get<RE::BSScript::Object>(aliasVar);
            if (!aliasObj || !aliasObj->type || aliasObj->type->name != "ReferenceAlias") {
                PLOG_WARN("[PULSAR] TraitAliases: Alias_{} is not a ReferenceAlias - script layout changed?"sv,
                          a_alias.name);
                return false;
            }

            RE::BSScript::Variable refVar;
            RE::BSScript::PackVariable(refVar, static_cast<RE::TESObjectREFR*>(a_actor));
            auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
                args.resize(1);
                args[0] = refVar;
                return true;
            };
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            return vm->DispatchMethodCall(aliasObj, RE::BSFixedString("ForceRefTo"), argFn, callback, 0);
        }
    }

    void Tick(bool a_enabled)
    {
        const std::uint64_t now = GetTickCount64();
        if (now - g_lastTickMs < kTickMs) return;
        g_lastTickMs = now;

        auto& net = Pulsar::Network::GetSingleton();
        if (!a_enabled || !net.IsConnected() || net.IsHost()) return;   // client in the host's world
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;
        // Right after a bridge the player still carries the HOST's perks until
        // DirectRestore swaps in this player's own: read traits only after that.
        if (!Pulsar::InventorySync::DirectRestore::RestoreComplete()) return;

        const Traits traits = ReadTraits(player);
        for (std::size_t i = 0; i < kAliases.size(); ++i) {
            if (g_done[i] || !Needs(i, traits)) continue;
            RE::Actor* npc = FindLoadedActor(kAliases[i].npcBase);
            if (!npc) continue;   // fill when the NPC is loaded near the player
            const bool ok = ForceFill(kAliases[i], npc);
            PLOG_INFO("[PULSAR] TraitAliases: ForceRefTo Alias_{} -> 0x{:08X} dispatched={}"sv,
                      kAliases[i].name, npc->formID, ok);
            g_done[i] = true;     // one attempt per world; a failure is logged, not retried in a loop
        }
    }

    void OnSaveLoaded()
    {
        g_done.fill(false);       // a new copy of the host's world: aliases cleared again
    }
}
