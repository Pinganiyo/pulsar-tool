#include "PCH.h"
#include "Debug/Buddy.h"

#include "Network/NetworkHost.h"
#include "RemotePlayer/RemotePlayerManager.h"

#include <RE/P/ProcessLists.h>

#include <Windows.h>
#include <cmath>
#include <unordered_set>

using namespace std::string_view_literals;

// Everything here goes through vanilla Papyrus functions dispatched on the game's
// VM (PlaceAtMe, SetPlayerTeammate, PathToReference, MoveTo, Delete), so no new
// script and no ESP change are needed. Each dispatch's return value is logged:
// false means the VM rejected the call (unknown function or wrong argument list).
namespace Pulsar::Buddy {

    namespace {

        constexpr std::uint64_t kFindTimeoutMs   = 10000;  // spawn is async on the VM
        constexpr std::uint64_t kFollowPeriodMs  = 2000;
        // Metres: Starfield REFR positions are in metres (verified in game).
        constexpr float         kFollowStartDist = 6.0f;
        constexpr float         kTeleportDist    = 40.0f;
        constexpr float         kCatchUpOffset   = 1.5f;     // beside the player
        constexpr std::uint32_t kBitTeammate     = 1u << 26;  // Actor boolBits (+0x208)

        enum class Phase { kOff, kSpawning, kActive };

        Phase                             g_phase = Phase::kOff;
        std::uint32_t                     g_buddyID = 0;       // FormID, re-resolved each tick
        std::uint32_t                     g_baseID  = 0;       // base the buddy was spawned from
        std::uint64_t                     g_spawnAt = 0;
        std::uint64_t                     g_lastFollow = 0;
        bool                              g_teammateLogged = false;
        std::unordered_set<std::uint32_t> g_preSpawn;          // high actors before PlaceAtMe

        RE::BSScript::IVirtualMachine* VM()
        {
            auto* game = RE::GameVM::GetSingleton();
            return game ? game->GetVM() : nullptr;
        }

        // Script object for a form (binds one if the form has none yet).
        template <class T>
        RE::BSTSmartPointer<RE::BSScript::Object> ObjectFor(T* a_form)
        {
            RE::BSScript::Variable v;
            RE::BSScript::PackVariable(v, a_form);
            if (!v.is<RE::BSScript::Object>()) return nullptr;
            return RE::BSScript::get<RE::BSScript::Object>(v);
        }

        bool CallMethod(RE::BSScript::IVirtualMachine* a_vm,
                        const RE::BSTSmartPointer<RE::BSScript::Object>& a_self,
                        const char* a_func,
                        std::vector<RE::BSScript::Variable> a_args)
        {
            if (!a_vm || !a_self) return false;
            auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
                args.resize(static_cast<std::uint32_t>(a_args.size()));
                for (std::uint32_t i = 0; i < a_args.size(); ++i) args[i] = a_args[i];
                return true;
            };
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            return a_vm->DispatchMethodCall(a_self, RE::BSFixedString(a_func), argFn, callback, 0);
        }

        template <class T>
        RE::BSScript::Variable Arg(T a_val)
        {
            RE::BSScript::Variable v;
            RE::BSScript::PackVariable(v, a_val);
            return v;
        }

        void Notify(const char* a_msg)
        {
            auto* vm = VM();
            if (!vm) return;
            RE::BSScript::Variable msg = Arg(RE::BSFixedString(a_msg));
            auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
                args.resize(1);
                args[0] = msg;
                return true;
            };
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            vm->DispatchStaticCall(RE::BSFixedString("Debug"), RE::BSFixedString("Notification"),
                                   argFn, callback, 0);
        }

        RE::Actor* ResolveBuddy()
        {
            if (!g_buddyID) return nullptr;
            auto* form = RE::TESForm::LookupByID(g_buddyID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor) return nullptr;
            auto* npc = actor->GetNPC();
            return (npc && npc->formID == g_baseID) ? actor : nullptr;
        }

        // ── high-process actor walk (same guarded pattern as CoopMortality) ──

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

        bool SafeReadRawHandle(RE::ProcessLists* a_pl, std::uint32_t a_i, std::uint32_t& a_h)
        {
            __try {
                auto* base = reinterpret_cast<std::uint32_t*>(a_pl->highActorHandles.data());
                if (!base) return false;
                a_h = base[a_i];
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        std::uint32_t SafeHighActorCount(RE::ProcessLists* a_pl)
        {
            __try { return static_cast<std::uint32_t>(a_pl->highActorHandles.size()); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }

        bool SafeReadBoolBits(RE::Actor* a_actor, std::uint32_t& a_bits)
        {
            __try {
                a_bits = *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(a_actor) + 0x208);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        template <class F>
        void ForEachHighActor(F&& a_fn)
        {
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!pl) return;
            std::uint32_t count = SafeHighActorCount(pl);
            if (count > 512) count = 512;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t raw = 0;
                if (!SafeReadRawHandle(pl, i, raw) || raw == 0 || raw == 0xFFFFFFFFu) continue;
                if (auto* a = SafeResolveHighActor(pl, i)) a_fn(a);
            }
        }

        float Distance(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // ── state transitions ──

        void Spawn(RE::PlayerCharacter* a_player, std::uint32_t a_baseFormID)
        {
            RE::TESNPC* base = a_baseFormID ? RE::TESForm::LookupByID<RE::TESNPC>(a_baseFormID)
                                            : a_player->GetNPC();
            if (!base) {
                PLOG_WARN("[PULSAR] Buddy: base NPC 0x{:08X} not found"sv, a_baseFormID);
                Notify("PULSAR buddy: base NPC not found (check BuddyBaseFormID)");
                return;
            }

            g_preSpawn.clear();
            ForEachHighActor([](RE::Actor* a) { g_preSpawn.insert(a->formID); });

            auto* vm = VM();
            auto self = ObjectFor(static_cast<RE::Actor*>(a_player));
            // PlaceAtMe(akFormToPlace, aiCount, abForcePersist, abInitiallyDisabled,
            // abDeleteWhenAble) - the same 5-argument call PulsarRemoteControl uses.
            const bool ok = CallMethod(vm, self, "PlaceAtMe",
                { Arg(static_cast<RE::TESForm*>(base)), Arg(std::int32_t(1)), Arg(true), Arg(false), Arg(false) });
            PLOG_INFO("[PULSAR] Buddy: PlaceAtMe(base=0x{:08X}) dispatched={}"sv, base->formID, ok);
            if (!ok) {
                Notify("PULSAR buddy: spawn failed (see PULSAR.log)");
                return;
            }
            g_baseID  = base->formID;
            g_buddyID = 0;
            g_spawnAt = GetTickCount64();
            g_teammateLogged = false;
            g_phase   = Phase::kSpawning;
        }

        void Configure(RE::Actor* a_buddy, RE::PlayerCharacter* a_player)
        {
            auto* vm = VM();
            auto self = ObjectFor(a_buddy);
            // SetPlayerTeammate(abTeammate, abCanDoFavor, abGivePlayerXP): joins the
            // player's fights and is never hostile to them.
            const bool tm = CallMethod(vm, self, "SetPlayerTeammate", { Arg(true), Arg(true), Arg(false) });
            // SetRelationshipRank(akOther, aiRank) - 3 = ally.
            const bool rr = CallMethod(vm, self, "SetRelationshipRank",
                { Arg(static_cast<RE::Actor*>(a_player)), Arg(std::int32_t(3)) });
            PLOG_INFO("[PULSAR] Buddy: configured 0x{:08X} SetPlayerTeammate={} SetRelationshipRank={}"sv,
                      a_buddy->formID, tm, rr);
        }

        void Despawn()
        {
            if (auto* buddy = ResolveBuddy()) {
                auto* vm = VM();
                auto self = ObjectFor(buddy);
                const bool dis = CallMethod(vm, self, "DisableNoWait", { Arg(false) });
                const bool del = CallMethod(vm, self, "Delete", {});
                PLOG_INFO("[PULSAR] Buddy: despawn 0x{:08X} DisableNoWait={} Delete={}"sv,
                          buddy->formID, dis, del);
            }
            g_phase   = Phase::kOff;
            g_buddyID = 0;
            Notify("PULSAR buddy dismissed");
        }

        void TickSpawning(RE::PlayerCharacter* a_player)
        {
            RE::Actor* found = nullptr;
            ForEachHighActor([&](RE::Actor* a) {
                if (found || a == a_player || g_preSpawn.count(a->formID)) return;
                if ((a->formID & 0xFF000000u) != 0xFF000000u) return;     // placed refs are dynamic
                if (a == Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor()) return;
                auto* npc = a->GetNPC();
                if (npc && npc->formID == g_baseID) found = a;
            });

            if (found) {
                g_buddyID = found->formID;
                g_phase   = Phase::kActive;
                g_lastFollow = 0;
                PLOG_INFO("[PULSAR] Buddy: spawned 0x{:08X}"sv, g_buddyID);
                Configure(found, a_player);
                Notify("PULSAR buddy is here");
            } else if (GetTickCount64() - g_spawnAt > kFindTimeoutMs) {
                PLOG_WARN("[PULSAR] Buddy: spawned actor not found within {} ms"sv, kFindTimeoutMs);
                Notify("PULSAR buddy: could not find the spawned NPC (see PULSAR.log)");
                g_phase = Phase::kOff;
            }
        }

        void TickActive(RE::PlayerCharacter* a_player)
        {
            auto* buddy = ResolveBuddy();
            if (!buddy) {
                PLOG_INFO("[PULSAR] Buddy: 0x{:08X} gone (load / cell unload) - off"sv, g_buddyID);
                g_phase = Phase::kOff;
                g_buddyID = 0;
                return;
            }

            if (!g_teammateLogged) {
                std::uint32_t bits = 0;
                if (SafeReadBoolBits(buddy, bits) && (bits & kBitTeammate)) {
                    PLOG_INFO("[PULSAR] Buddy: teammate flag confirmed"sv);
                    g_teammateLogged = true;
                }
            }

            const std::uint64_t now = GetTickCount64();
            if (now - g_lastFollow < kFollowPeriodMs) return;
            g_lastFollow = now;
            if (buddy->IsDead()) return;

            auto* vm = VM();
            auto self = ObjectFor(buddy);
            auto playerObj = static_cast<RE::Actor*>(a_player);
            const bool sameCell = buddy->parentCell == a_player->parentCell;
            const float dist = Distance(buddy->data.location, a_player->data.location);

            if (!sameCell || dist > kTeleportDist) {
                // MoveTo(akTarget, afXOffset, afYOffset, afZOffset, abMatchRotation)
                const bool ok = CallMethod(vm, self, "MoveTo",
                    { Arg(static_cast<RE::TESObjectREFR*>(playerObj)), Arg(kCatchUpOffset), Arg(kCatchUpOffset), Arg(0.0f), Arg(false) });
                PLOG_INFO("[PULSAR] Buddy: catch-up MoveTo (sameCell={}, dist={:.1f}) dispatched={}"sv,
                          sameCell, dist, ok);
            } else if (dist > kFollowStartDist) {
                // PathToReference(aTarget, afWalkRunPercent) - latent; walks/runs over.
                CallMethod(vm, self, "PathToReference",
                    { Arg(static_cast<RE::TESObjectREFR*>(playerObj)), Arg(1.0f) });
            }
        }
    }

    void Tick(int a_hotkeyVK, std::uint32_t a_baseFormID)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;

        static bool s_down = false;
        const bool down = a_hotkeyVK > 0 && (GetAsyncKeyState(a_hotkeyVK) & 0x8000) != 0;
        const bool pressed = down && !s_down;
        s_down = down;

        if (pressed) {
            if (g_phase != Phase::kOff) {
                Despawn();
            } else if (Pulsar::Network::GetSingleton().IsConnected()) {
                PLOG_INFO("[PULSAR] Buddy: refused - a co-op peer is connected"sv);
                Notify("PULSAR buddy is for solo testing - disconnect first");
            } else {
                Spawn(player, a_baseFormID);
            }
        }

        // A peer joined while the buddy was out: remove it so it never enters a session.
        if (g_phase != Phase::kOff && Pulsar::Network::GetSingleton().IsConnected()) {
            PLOG_INFO("[PULSAR] Buddy: peer connected - dismissing"sv);
            Despawn();
            return;
        }

        if (g_phase == Phase::kSpawning) TickSpawning(player);
        else if (g_phase == Phase::kActive) TickActive(player);
    }

}  // namespace Pulsar::Buddy
