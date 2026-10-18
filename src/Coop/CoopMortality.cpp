#include "PCH.h"
#include "Coop/CoopMortality.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "WorldSync/WorldSync.h"
#include "Cell3DAttach.h"   // IsQuiet() - gate anim-graph ops during reloads
#include "NASLog.h"
#include "Config.h"         // GetConfig() - dev-hotkey gate for the manual revive

#include <atomic>
#include <cstring>

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/ProcessLists.h>
#include <RE/A/ActorValueInfo.h>
#include <RE/B/BSFixedString.h>
#include <RE/T/TESForm.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>   // _ReturnAddress
#include <MinHook.h>

using namespace std::string_view_literals;

namespace Pulsar::Coop::Mortality
{
    static bool                g_inited   = false;
    static RE::ActorValueInfo* g_healthAV = nullptr;

    // Downed state machine.
    // Entered by the death-reload hook (Hook_PlayerDeathTick) on the exact tick
    // the player's life-state enters a death state (+0xF8 bits == 0x20000/0x40000)
    // the same tick the engine would start the reload countdown.  Race-free,
    // no polling required for entry.
    // atomic: written/read from the game thread (death-tick, OnGameThreadTick) AND
    // from engine worker threads (the life-state setter hook fires off-thread).
    static std::atomic<bool> g_localDowned{ false };
    static constexpr float kDownedFloor = 25.0f;  // cosmetic HP floor while downed

    // Way A (medpack revive) tunables.
    // Metres. Was 250 ("game units ~3 m"), but Starfield positions are metres, so
    // the revive key reached a downed partner up to 250 m away.
    static constexpr float kReviveRadius = 3.0f;    // to the downed clone
    static constexpr int   kActivateVK   = 0x45;    // 'E' (tap; rebind/controller = polish)

    // Partner's downed state, mirrored from PKT_PLAYER_DOWNED. Written on the
    // ENet receive thread (OnDownedReceived), read on the game thread (hook) →
    // atomic. When BOTH g_localDowned and g_remoteDowned are set, each machine
    // lets its own vanilla reload fire = controlled both-downed game-over.
    static std::atomic<bool> g_remoteDowned{ false };

    // Way A - medpack revive request received from the partner (PKT_PLAYER_REVIVE).
    // Written on the ENet receive thread (OnReviveReceived = queue only), consumed
    // on the game thread (OnGameThreadTick) → ReviveLocalPlayer. Engine actor
    // mutation must never run on the ENet thread.
    static std::atomic<bool> g_medpackRevivePending{ false };

    // Partner's debounced combat state, mirrored from PKT_PLAYER_COMBAT. Written on
    // the ENet receive thread (OnCombatStatusReceived), read on the game thread
    // (Way-B revive gate) → atomic. The DOWNED player can't read its own combat
    // reliably (its local enemies disengage the instant it goes down → its hostiles
    // scan reads "clear" mid-fight), so Way-B also requires the FIGHTER to report
    // combat-clear before standing the downed player back up.
    static std::atomic<bool> g_remoteCombatActive{ false };

    // Broadcast our local downed transition to the peer (reliable).
    static void SendDownedState(bool a_downed)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        using namespace Pulsar::Network;
        std::uint8_t buf[1 + sizeof(PlayerDownedData)];
        buf[0] = PKT_PLAYER_DOWNED;
        PlayerDownedData d{};
        d.downed     = a_downed ? 1 : 0;
        d.playerSlot = net.IsHost() ? 0 : 1;
        std::memcpy(&buf[1], &d, sizeof(d));
        net.Send(buf, sizeof(buf), /*reliable=*/true);
        PLOG_INFO("[DOWNED] sent downed-state={} to peer"sv, d.downed);
    }

    // Broadcast our local (debounced) combat state to the peer. Sent both on change
    // AND on a periodic heartbeat (a_log=false for heartbeats to keep the log quiet) -
    // on-change-only was lost to the connect-time load barrier / a post-load reset and
    // never re-asserted, so the downed partner's gate never engaged (confirmed
    // 2026-06-09). A downed partner gates its Way-B combat-end revive on this.
    static void SendCombatStatus(bool a_inCombat, bool a_log)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        using namespace Pulsar::Network;
        std::uint8_t buf[1 + sizeof(PlayerCombatData)];
        buf[0] = PKT_PLAYER_COMBAT;
        PlayerCombatData d{};
        d.inCombat   = a_inCombat ? 1 : 0;
        d.playerSlot = net.IsHost() ? 0 : 1;
        std::memcpy(&buf[1], &d, sizeof(d));
        net.Send(buf, sizeof(buf), /*reliable=*/true);
        if (a_log) PLOG_INFO("[DOWNED] sent combat-state={} to peer"sv, d.inCombat);
    }

    // Way A - tell the downed peer (slot a_targetSlot) to get back up. Reliable.
    static void SendRevive(std::uint8_t a_targetSlot)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        using namespace Pulsar::Network;
        std::uint8_t buf[1 + sizeof(PlayerReviveData)];
        buf[0] = PKT_PLAYER_REVIVE;
        PlayerReviveData d{};
        d.playerSlot = a_targetSlot;
        std::memcpy(&buf[1], &d, sizeof(d));
        net.Send(buf, sizeof(buf), /*reliable=*/true);
        PLOG_INFO("[DOWNED] sent revive request to slot {}"sv, a_targetSlot);
    }

    // ----------------------------------------------------------------
    // PLAYER death-reload arm point - REL::ID 102812
    // FUN_141a5ba50: a PlayerCharacter-ONLY per-frame sub-tick.
    // ABI: void __fastcall(RE::PlayerCharacter* a_this /*RCX*/, float a_dt /*XMM1*/)
    //
    // Verified arm logic (Ghidra 1.16.236, disasm @ 0x141A5981E):
    //   eax = *(u32*)(this+0xF8) & 0x1E0000      ; life-state bits 17-20
    //   if (eax == 0x20000 || eax == 0x40000)    ; player in DEATH state (1 or 2)
    //       countdown -= a_dt;  (seeded from fPlayerDeathReloadTime)
    //       if (countdown < 0)  FUN_141ee3280(DAT_1461e7460);  // <<< SAVE-RELOAD FIRES
    //   else  reset countdown;                   ; no reload
    //
    // This is THE arm point - keyed only to the +0xF8 death bits, NOT to live
    // Health (so HP-floor clamping could never cancel it), and NOT the
    // lethal-check virtual (REL::ID 100986 - disproven, never on this path).
    //
    // FIX: while downed, temporarily mask the +0xF8 death bits to 0 before
    // calling the original, then restore.  The in-function gate then takes the
    // "reset countdown" branch → the reload is never armed.  Bleedout visuals
    // (state 7/8 = 0xE0000/0x100000) are OUTSIDE the masked death bits, so they
    // are preserved.  PlayerCharacter-only by construction → NPCs untouched.
    // ----------------------------------------------------------------
    using DeathTickFn = void(__fastcall*)(RE::PlayerCharacter*, float);
    static DeathTickFn s_origDeathTick = nullptr;

    static constexpr std::uintptr_t kLifeStateOffset = 0xF8;
    static constexpr std::uint32_t  kLifeStateMask   = 0x1E0000u;  // bits 17-20
    static constexpr std::uint32_t  kDeathState1     = 0x20000u;   // life-state 1 (death)
    static constexpr std::uint32_t  kDeathState2     = 0x40000u;   // life-state 2 (death)
    static constexpr std::uint32_t  kBleedoutEnum    = 7u;         // life-state enum 7 = bleedout (0xE0000 in bits)
    // Life-state ENUM values fed to the setter (bits = enum << 17). enum 0 = alive,
    // 1 = death-state-1 (0x20000), 2 = death-state-2 (0x40000), 7 = bleedout (0xE0000).
    static constexpr std::uint32_t  kAliveEnum       = 0u;
    static constexpr std::uint32_t  kDeathEnum1      = 1u;
    static constexpr std::uint32_t  kDeathEnum2      = 2u;

    // Life-state setter - REL::ID 100787 (FUN_14190e890, Actor vtable+0xB78).
    // ABI: void __fastcall(RE::Actor* this /*RCX*/, uint32 newStateEnum /*EDX*/).
    //
    // KEEP-ALIVE MODEL (2026-06-06 redesign): we HOOK this setter and SWALLOW every
    // "player -> death" call so the player's life-state stays ALIVE (0) - never death,
    // never bleedout. The game-thread tick then clamps HP > 0 (engine stops retrying)
    // and locks controls (frozen). Revive = unfreeze + restore HP (no life-state
    // change). Both-down = force real death via the orig setter -> vanilla reload.
    // Why not death/bleedout: the player has no kDead corpse (Resurrect no-ops) and
    // the engine reverts player bleedout->alive every frame (flip -> through floor);
    // both proven dead-ends. See the spec.
    using SetLifeStateFn = void(__fastcall*)(RE::Actor*, std::uint32_t);
    static SetLifeStateFn s_origSetLifeState = nullptr;  // MinHook trampoline (orig)

    // DEATH HANDLER (ragdoll source) - FUN_141908af0 (RVA 0x1906250). The big actor
    // Kill/death dispatcher. Runtime DIAG (2026-06-09) proved THIS is the function that
    // vtable-calls the SetLifeState+notify wrapper and, right after it (Ghidra
    // 0x141908494 CALL [+0xaf8] -> FUN_141ba1dd0), triggers the player ragdoll. The old
    // life-state-setter swallow only blocked a sub-step, leaving the rest of this handler
    // (ragdoll, AI-detach, death events) to run = "half-dead": alive flag but ragdolled
    // body. KEEP-ALIVE fix: hook the whole handler and swallow it for the local player →
    // no death processing at all → no ragdoll. Both-down game-over is driven separately
    // by OnGameThreadTick.  ABI (decompile): void(Actor* this, Actor* killer, void*,
    // void*, uint8, char).
    using DeathHandlerFn = void(__fastcall*)(RE::Actor*, RE::Actor*, void*, void*, std::uint8_t, char);
    static DeathHandlerFn s_origDeathHandler = nullptr;
    static constexpr std::uintptr_t kDeathHandlerRVA = 0x1906250ULL;

    // ACTOR::KILL dispatcher - REL::ID(100769) / RVA 0x1906010. EVERY lethal-damage
    // path funnels here; it runs the kill-decision (vtable[0x182] @0x1A3CC50) which
    // cues the GLOBAL death-mode audio (low-pass muffle via FUN_141b5c710) and THEN
    // calls the death handler (0x1906250). Swallowing only the death handler is too
    // late: the muffle is already cued. So we intercept at Actor::Kill ENTRY and, for
    // the local co-op player (not both-down), ABORT the whole kill -> the player never
    // dies, the kill-decision never runs, no death-mode audio is ever cued. The
    // game-thread HP clamp (OnGameThreadTick) floors HP back to kDownedFloor. Both-down
    // game-over is forced separately (SafeSetLifeStateRaw in OnGameThreadTick), which
    // bypasses this hook. ABI (244 disasm/decomp): rcx=actor, rdx=killer, xmm2=float,
    // r9b=u8, +3 stack bytes - declaring arg3 as float keeps xmm2 intact on pass-through.
    using ActorKillFn = void(__fastcall*)(RE::Actor*, RE::Actor*, float,
                                          std::uint8_t, char, char, char);
    static ActorKillFn s_origActorKill = nullptr;
    static constexpr std::uintptr_t kActorKillRVA = 0x1906010ULL;

    // Forward decls - defined further below, used earlier (death-tick / setter hooks).
    static int SafeSetLifeStateRaw(RE::Actor* a_actor, std::uint32_t a_enum);  // orig setter (bypass swallow)
    static int SafeSetPlayerControlsEnabled(bool a_enabled);                   // freeze/unfreeze (Task 1 RE)

    // SEH: read the masked life-state bits from player+0xF8.
    static bool SafeReadLifeStateBits(RE::PlayerCharacter* a_this, std::uint32_t& a_bits)
    {
        __try {
            const std::uint32_t v =
                *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(a_this) + kLifeStateOffset);
            a_bits = v & kLifeStateMask;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // SEH: read the engine combat-state flag (Actor virtual vtable index 0x16C).
    // NOTE - DISPROVEN for Way B: Actor::IsInCombat() stays true on the masked
    // death-state player even after all enemies are dead (engine never clears the
    // downed player's combat flag). Confirmed in-game (REVIVE-DIAG inCombat=1 with
    // 0 enemies). Kept only for reference; Way B uses SafeHostilesNear instead.
    static bool SafeIsInCombat(RE::PlayerCharacter* a_this, bool& a_inCombat)
    {
        __try {
            a_inCombat = a_this->IsInCombat();
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Plain-C SEH read of an actor's combat-relevant fields (same offsets NPCSync
    // uses: formID +0x28, boolBits +0x208, currentCombatTarget +0x298).
    static bool SafeReadActorCombat(RE::Actor* a_actor, std::uint32_t* a_fid,
                                    std::uint32_t* a_boolBits, std::uint32_t* a_combatTarget)
    {
        __try {
            auto* b = reinterpret_cast<std::uint8_t*>(a_actor);
            *a_fid          = *reinterpret_cast<std::uint32_t*>(b + 0x28);
            *a_boolBits     = *reinterpret_cast<std::uint32_t*>(b + 0x208);
            *a_combatTarget = *reinterpret_cast<std::uint32_t*>(b + 0x298);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Resolve high-process actor #i. operator[] + handle.get() use NiPointer
    // (has a destructor) so this MUST stay OUT of any __try (MSVC C2712).
    static RE::Actor* ResolveHighActor(RE::ProcessLists* a_pl, std::uint32_t a_i)
    {
        RE::NiPointer<RE::Actor> sp = a_pl->highActorHandles[a_i].get();
        return sp.get();
    }

    // Read the raw 32-bit handle value of highActorHandles[i] (POD read of the
    // array element - BSPointerHandle holds a single uint32 at offset 0). SEH-safe.
    static bool SafeReadRawHandle(RE::ProcessLists* a_pl, std::uint32_t a_i, std::uint32_t& a_h)
    {
        __try {
            auto* base = reinterpret_cast<std::uint32_t*>(a_pl->highActorHandles.data());
            if (!base) return false;
            a_h = base[a_i];
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // SEH wrapper around ResolveHighActor. Root-cause crash (observed: 0xC0000005
    // READ at 0xFFFFFFFFFFFFFFFF, R12=-1, engine handle-manager GetSmartPointer):
    // the CLIENT's highActorHandles table holds a -1 / stale sentinel handle (the
    // downed player + dead clones), and the engine's GetSmartPointer dereferences
    // an invalid slot for it. The caller pre-filters 0 / 0xFFFFFFFF handles so this
    // resolution only runs for plausibly-valid handles; this SEH then guards the
    // residual valid-at-check-but-freed-during-.get() race. ResolveHighActor must
    // live outside __try (NiPointer destructor → C2712), hence this wrapper returns
    // a raw pointer with no destructor objects.
    static RE::Actor* SafeResolveHighActor(RE::ProcessLists* a_pl, std::uint32_t a_i)
    {
        __try {
            return ResolveHighActor(a_pl, a_i);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // Are any living hostile actors still in combat near the player?
    //
    // We do NOT use the engine's ProcessLists::AreHostileActorsNear: CommonLibSF
    // binds it to REL::ID{0} (placeholder), and the versionlib entry (154040 →
    // 0x2DB4870) Ghidra-decompiles to an unrelated lifecycle routine (bit-flag +
    // list walk, no bool) - calling it would mutate ProcessLists state, not query
    // hostility. Instead we walk highActorHandles (the high-process = nearby
    // actors) and test the same per-actor fields NPCSync uses: kSearchingInCombat
    // (boolBits 1<<14) or a non-zero combat target - excluding the player, our
    // teammates/companions, the dead, and the remote clone. This is the reliable
    // "combat is over" signal for Way B (Actor::IsInCombat stays stuck true on the
    // masked death-state player - disproven in-game).
    static bool SafeHostilesNear(bool& a_any)
    {
        a_any = false;
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) return false;

        RE::Actor* clone = nullptr;
        __try { clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { clone = nullptr; }

        std::uint32_t count = 0;
        bool          metaOk = false;
        __try { count = static_cast<std::uint32_t>(pl->highActorHandles.size()); metaOk = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { metaOk = false; }
        if (!metaOk) return false;
        if (count > 512) count = 512;  // sanity cap

        constexpr std::uint32_t kBitDead      = 1u << 11;
        constexpr std::uint32_t kBitSearchInC = 1u << 14;
        constexpr std::uint32_t kBitTeammate  = 1u << 26;

        for (std::uint32_t i = 0; i < count; ++i) {
            // Root-cause guard: skip empty (0) and invalid (-1 sentinel) handles
            // BEFORE resolving - these crash the engine handle manager (the client
            // table holds a -1 entry for the downed player / dead clones).
            std::uint32_t rawH = 0;
            if (!SafeReadRawHandle(pl, i, rawH)) continue;
            if (rawH == 0 || rawH == 0xFFFFFFFFu) continue;

            RE::Actor* a = SafeResolveHighActor(pl, i);
            if (!a || a == clone) continue;
            std::uint32_t fid = 0, bits = 0, tgt = 0;
            if (!SafeReadActorCombat(a, &fid, &bits, &tgt)) continue;
            if (fid == 0x14)          continue;   // the player
            if (bits & kBitDead)      continue;   // dead enemy doesn't count
            if (bits & kBitTeammate)  continue;   // our companion/teammate
            if ((bits & kBitSearchInC) || tgt != 0) { a_any = true; break; }
        }
        return true;
    }

    // SEH: is the local player within a_radius of the downed remote clone?
    // Also returns the clone ptr + squared distance for diagnostics.
    // POD-only inside __try (NiPoint3 = 3 floats, raw ptrs) → C2712-safe.
    static bool SafeNearDownedClone(RE::PlayerCharacter* a_player, float a_radius,
                                    std::uintptr_t& a_cloneOut, float& a_distSqOut)
    {
        a_cloneOut  = 0;
        a_distSqOut = -1.0f;
        __try {
            RE::Actor* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
            a_cloneOut = reinterpret_cast<std::uintptr_t>(clone);
            if (!clone) return false;
            const RE::NiPoint3 pp = a_player->GetPosition();
            const RE::NiPoint3 cp = clone->GetPosition();
            const float dx = cp.x - pp.x, dy = cp.y - pp.y, dz = cp.z - pp.z;
            const float dsq = dx * dx + dy * dy + dz * dz;
            a_distSqOut = dsq;
            return dsq <= (a_radius * a_radius);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // SEH: call orig with the death bits temporarily cleared so the in-function
    // reload-countdown gate resets instead of arming.  No destructor objects (C2712).
    static void SafeDeathTickMasked(RE::PlayerCharacter* a_this, float a_dt)
    {
        __try {
            auto* ls = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(a_this) + kLifeStateOffset);
            const std::uint32_t saved = *ls;
            *ls = saved & ~kLifeStateMask;   // present as "alive" to the reload gate
            s_origDeathTick(a_this, a_dt);
            *ls = saved;                     // restore true state for everything else
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // SEH: call orig unmodified (normal path / not downed).
    static void SafeDeathTickPlain(RE::PlayerCharacter* a_this, float a_dt)
    {
        __try { s_origDeathTick(a_this, a_dt); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // L.7 V13i mitigation, reused for the both-downed game-over.
    // FUN_141843BF0 (the bit-0x4 load wrapper that the vanilla player-death
    // reload goes through) has a built-in process-restart trap:
    //   if (DAT_145e794dc > 1 && in-game) { relaunch .exe; swi(3) /*int3*/ }
    // The host has already loaded a co-op save this session (counter ≥ 1), so its
    // both-downed reload = 2nd load → counter > 1 → int3 fail-stop (the observed
    // 0xC0000025 crash). Clearing the counter to 0 each frame during the release
    // window keeps it ≤ 1 at the wrapper's check → safe in-process load branch.
    static void SafeClearLoadCounter()
    {
        __try {
            const auto addr = REL::Offset(0x5E6D6DCULL).address();
            if (addr) *reinterpret_cast<volatile std::uint32_t*>(addr) = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __fastcall Hook_PlayerDeathTick(RE::PlayerCharacter* a_this, float a_dt)
    {
        // Not in co-op, or no this → behave exactly like vanilla.
        if (!a_this || !Pulsar::Network::GetSingleton().IsConnected()) {
            SafeDeathTickPlain(a_this, a_dt);
            return;
        }

        std::uint32_t bits = 0;
        if (!SafeReadLifeStateBits(a_this, bits)) {
            SafeDeathTickPlain(a_this, a_dt);
            return;
        }

        const bool inDeath = (bits == kDeathState1 || bits == kDeathState2);

        // Death-state detected → enter our downed state (race-free: this is the
        // exact tick the engine would start counting down to the reload).
        // NOTE: forcing bleedout (life-state 7) via the engine setter was tried and
        // REVERTED - the player has no vanilla bleedout, the engine re-sets death
        // every frame and the death→bleedout flip flings the ragdoll. We keep the
        // stable masked death-ragdoll; the companion downed POSE needs dedicated RE.
        if (inDeath && !g_localDowned) {
            g_localDowned = true;
            SendDownedState(true);  // tell the peer we are down
            PLOG_INFO("[DOWNED] entered downed (lifeState=0x{:X}, reload suppressed)"sv, bits);
        }

        if (g_localDowned) {
            std::uint32_t live = 0;
            const bool ok = SafeReadLifeStateBits(a_this, live);

            // NOTE: the old "auto-recover when live==0" branch is REMOVED. Under
            // keep-alive the player is ALWAYS life-state alive (0) while downed, so it
            // would fire every frame and clear g_localDowned. Recovery now happens via
            // ReviveLocalPlayer and the load-tick post-load reset in OnGameThreadTick.
            const bool deathNow   = ok && (live == kDeathState1 || live == kDeathState2);
            const bool bothDowned = g_remoteDowned.load(std::memory_order_relaxed);

            // Throttle a status log to ~1 Hz.
            static std::uint64_t s_lastDownLogMs = 0;
            const std::uint64_t  nowMs = ::GetTickCount64();
            if (nowMs - s_lastDownLogMs >= 1000) {
                s_lastDownLogMs = nowMs;
                PLOG_INFO("[DOWNED] maintain: lifeState=0x{:X} deathNow={} remoteDowned={}"sv,
                          live, deathNow ? 1 : 0, bothDowned ? 1 : 0);
            }

            // Inc 5: BOTH players downed → controlled game-over (each side reloads).
            if (bothDowned) {
                // Keep the engine load counter ≤ 1 every frame so the imminent
                // reload avoids FUN_141843BF0's relaunch+int3 trap (host crash).
                SafeClearLoadCounter();
                // Player is in BLEEDOUT (not death) so the reload gate won't fire on
                // its own. Force REAL death via the original setter (bypasses our
                // death->bleedout redirect) so the vanilla reload gate arms.
                if (!deathNow) SafeSetLifeStateRaw(a_this, kDeathEnum1);
                static bool s_loggedBoth = false;
                if (!s_loggedBoth) {
                    s_loggedBoth = true;
                    PLOG_INFO("[DOWNED] BOTH players downed -> forcing death for game-over reload"sv);
                }
                SafeDeathTickPlain(a_this, a_dt);  // death set → reload gate fires
                return;
            }

            // Only we are downed → suppress the reload.
            if (deathNow) {
                SafeDeathTickMasked(a_this, a_dt);  // engine re-set death → mask to block reload
            } else {
                SafeDeathTickPlain(a_this, a_dt);   // bleedout/alive → reload gate misses
            }
            return;
        }

        SafeDeathTickPlain(a_this, a_dt);
    }

    // SEH: clear the +0xF8 death/bleedout bits → present the player as alive
    // again (used on revive so the player leaves the death life-state).
    static int SafeClearDeathState(RE::PlayerCharacter* a_this)
    {
        __try {
            auto* ls = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(a_this) + kLifeStateOffset);
            *ls = *ls & ~kLifeStateMask;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // SEH: is this actor the local player? The setter fires for ALL actors; we only
    // special-case the player. PlayerCharacter derives Actor via single inheritance,
    // so the pointers compare equal.
    static bool SafeIsLocalPlayer(RE::Actor* a_actor)
    {
        __try {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            return a_actor && pc && a_actor == static_cast<RE::Actor*>(pc);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // SEH: is this actor the PARTNER's clone (the remote-player representation on this
    // machine)? GetRemoteActor() returns a raw RE::Actor* (no destructor → __try-safe;
    // same call shape NPCSync/SafeHostilesNear use).
    static bool SafeIsRemoteClone(RE::Actor* a_actor)
    {
        __try {
            RE::Actor* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
            return a_actor && clone && a_actor == clone;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // SEH: fetch the partner-clone actor (nullptr when absent / not spawned).
    static RE::Actor* SafeGetRemoteClone()
    {
        __try {
            return Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // SEH: call the ORIGINAL (unhooked) life-state setter via the MinHook trampoline.
    // Used to drive the bleedout get-up on revive (enum 0 = alive) and to force real
    // death for the both-downed game-over, both BYPASSING our death->bleedout redirect.
    static int SafeSetLifeStateRaw(RE::Actor* a_actor, std::uint32_t a_enum)
    {
        __try {
            if (s_origSetLifeState) s_origSetLifeState(a_actor, a_enum);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // Hook on the life-state setter (REL::ID 100787). KEEP-ALIVE: swallow every
    // "player -> death" set so the life-state stays alive (0) - no death, no bleedout,
    // no ragdoll. NPCs and non-death sets pass straight through unchanged.
    static void __fastcall Hooked_SetLifeState(RE::Actor* a_this, std::uint32_t a_newState)
    {
        void* const ret = _ReturnAddress();  // caller = the death-process we want to find
        if (Pulsar::Network::GetSingleton().IsConnected() && SafeIsLocalPlayer(a_this)) {
            const bool isDeath    = (a_newState == kDeathEnum1 || a_newState == kDeathEnum2);
            const bool bothDowned = g_remoteDowned.load(std::memory_order_relaxed);
            if (isDeath && !bothDowned) {
                // Swallow the engine's player death-set entirely (do NOT call orig) →
                // the life-state is left untouched (stays alive / 0). The game-thread
                // tick then clamps HP > 0 (engine stops retrying) and locks controls
                // (frozen). First false→true edge only: flag + notify the peer.
                // DIAG: log the caller RVA - that is the death-PROCESS that also fires
                // the ragdoll (which we don't yet intercept). RE it next to prevent the
                // ragdoll entirely so the player stays upright-frozen.
                if (!g_localDowned.exchange(true)) {
                    // RVA = runtime return addr - actual Starfield.exe base (ASLR);
                    // Ghidra address = 0x140000000 + this RVA.
                    static const std::uintptr_t exeBase =
                        reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
                    const std::uintptr_t rva = reinterpret_cast<std::uintptr_t>(ret) - exeBase;
                    SendDownedState(true);
                    PLOG_INFO("[DOWNED] entered downed (keep-alive); death-set caller RVA=0x{:X} (Ghidra 0x{:X})"sv,
                              rva, 0x140000000ULL + rva);
                }
                return;  // swallow
            }
            // bothDowned: let death through (falls to orig below) → real death → reload.
        }
        if (s_origSetLifeState) s_origSetLifeState(a_this, a_newState);
    }

    // SEH: run the ORIGINAL (unhooked) death handler on the player - used by the
    // both-downed game-over in OnGameThreadTick to force a REAL death after we have been
    // swallowing it. Null killer / 0 flags = a plain self-death; the decompile guards
    // every param_2 (killer) use against null. Calls the trampoline → bypasses our hook.
    static int SafeRunDeathHandler(RE::Actor* a_actor)
    {
        __try {
            if (s_origDeathHandler) s_origDeathHandler(a_actor, nullptr, nullptr, nullptr, 0, 0);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // ACTOR::KILL intercept - the ROOT fix for the stuck death-mode audio. By aborting
    // the kill before the kill-decision runs, the global death-audio muffle is NEVER
    // cued (so there is nothing to clear on revive). This supersedes the death-handler
    // swallow below as the primary downed trigger for lethal damage; the handler hook
    // stays as defense-in-depth for any death path that bypasses Actor::Kill.
    static void __fastcall Hooked_ActorKill(RE::Actor* a_this, RE::Actor* a_killer, float a_p3,
                                            std::uint8_t a_p4, char a_p5, char a_p6, char a_p7)
    {
        if (Pulsar::Network::GetSingleton().IsConnected()) {
            if (SafeIsLocalPlayer(a_this)) {
                // Both-down game-over needs a REAL death (forced elsewhere, bypasses
                // this hook). Otherwise: abort the kill → no death, no death-audio.
                const bool bothDowned = g_remoteDowned.load(std::memory_order_relaxed);
                if (!bothDowned) {
                    if (!g_localDowned.exchange(true)) {
                        SendDownedState(true);  // tell the peer we are down
                        PLOG_INFO("[DOWNED] entered downed (Actor::Kill intercepted; no death-mode audio)"sv);
                    }
                    return;  // player survives at low HP (floored on the game thread)
                }
                // bothDowned → fall through to real death (game-over reload).
            } else if (SafeIsRemoteClone(a_this)) {
                // Keep the partner's clone upright (same as the death-handler swallow)
                // AND skip its kill-decision so a partner-down can't muffle our audio.
                return;
            }
        }
        s_origActorKill(a_this, a_killer, a_p3, a_p4, a_p5, a_p6, a_p7);
    }

    // PRIMARY keep-alive hook - the actor death handler FUN_141908af0 (the ragdoll
    // source). For the local player we ALWAYS swallow it: enter our downed state, tell
    // the peer, and return BEFORE the vanilla kill runs → no ragdoll, no AI-detach, no
    // death events, no life-state change. The both-downed game-over is NOT decided here
    // (a per-machine decision at kill-time would desync - the 2nd player to die would
    // game-over while the 1st only stays downed). Instead each machine drives it from
    // OnGameThreadTick when BOTH g_localDowned and g_remoteDowned are set, so both sides
    // converge to the reload. NPCs / non-player actors pass straight through.
    static void __fastcall Hooked_DeathHandler(RE::Actor* a_this, RE::Actor* a_killer,
                                               void* a3, void* a4, std::uint8_t a5, char a6)
    {
        if (Pulsar::Network::GetSingleton().IsConnected()) {
            if (SafeIsLocalPlayer(a_this)) {
                if (!g_localDowned.exchange(true)) {
                    SendDownedState(true);  // tell the peer we are down
                    PLOG_INFO("[DOWNED] entered downed (keep-alive; death handler FUN_141908af0 swallowed)"sv);
                }
                return;  // swallow → player never dies, never ragdolls
            }
            // The PARTNER's clone must never die independently on this machine - its life
            // is authoritative on the owning client (kept alive there by the swallow
            // above). Without this the host's engine combat ragdolls the clone into a
            // corpse while the real client is up and walking (observed 2026-06-09).
            // Swallow so the clone stays upright; the client drives down/up via packets.
            if (SafeIsRemoteClone(a_this)) {
                static std::uint64_t s_lastCloneLogMs = 0;
                const std::uint64_t nowMs = ::GetTickCount64();
                if (nowMs - s_lastCloneLogMs >= 1000) {
                    s_lastCloneLogMs = nowMs;
                    PLOG_INFO("[DOWNED] remote-clone death swallowed (kept alive on this machine)"sv);
                }
                return;  // swallow → clone stays upright instead of ragdolling
            }
        }
        if (s_origDeathHandler) s_origDeathHandler(a_this, a_killer, a3, a4, a5, a6);
    }

    static void InstallHooks()
    {
        // MH_Initialize defensively; Main.cpp calls it at kPostLoad but we
        // guard here for robustness.  MH_ERROR_ALREADY_INITIALIZED is success.
        MH_STATUS mhInit = MH_Initialize();
        if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
            PLOG_WARN("[DOWNED] MH_Initialize failed: {}"sv, static_cast<int>(mhInit));
            return;
        }

        auto target = reinterpret_cast<LPVOID>(REL::ID(102812).address());
        if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&Hook_PlayerDeathTick),
                          reinterpret_cast<LPVOID*>(&s_origDeathTick)) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            PLOG_INFO("[DOWNED] death-reload hook installed @ REL::ID 102812 (FUN_141a5ba50)"sv);
        } else {
            PLOG_WARN("[DOWNED] death-reload hook install FAILED"sv);
        }

        // Hook the life-state setter (REL::ID 100787) → redirect "player -> death"
        // to bleedout (the recoverable downed state). See Hooked_SetLifeState.
        auto setterTarget = reinterpret_cast<LPVOID>(REL::ID(100787).address());
        if (MH_CreateHook(setterTarget, reinterpret_cast<LPVOID>(&Hooked_SetLifeState),
                          reinterpret_cast<LPVOID*>(&s_origSetLifeState)) == MH_OK &&
            MH_EnableHook(setterTarget) == MH_OK) {
            PLOG_INFO("[DOWNED] life-state setter HOOKED @ REL::ID 100787 (keep-alive swallow)"sv);
        } else {
            PLOG_WARN("[DOWNED] life-state setter hook FAILED"sv);
        }

        // PRIMARY keep-alive hook: the actor death handler FUN_141908af0 (the ragdoll
        // source). Swallowing it for the local player stops the ragdoll at the root.
        auto deathTarget = reinterpret_cast<LPVOID>(REL::Offset(kDeathHandlerRVA).address());
        if (MH_CreateHook(deathTarget, reinterpret_cast<LPVOID>(&Hooked_DeathHandler),
                          reinterpret_cast<LPVOID*>(&s_origDeathHandler)) == MH_OK &&
            MH_EnableHook(deathTarget) == MH_OK) {
            PLOG_INFO("[DOWNED] death handler HOOKED @ RVA 0x{:X} (FUN_141908af0 keep-alive swallow)"sv,
                      kDeathHandlerRVA);
        } else {
            PLOG_WARN("[DOWNED] death handler hook FAILED"sv);
        }

        // PRIMARY keep-alive: intercept Actor::Kill (REL::ID 100769 / RVA 0x1906010)
        // BEFORE the kill-decision cues the global death-mode audio. Aborting here for
        // the local player is the root fix for the stuck death muffle after revive.
        auto killTarget = reinterpret_cast<LPVOID>(REL::Offset(kActorKillRVA).address());
        if (MH_CreateHook(killTarget, reinterpret_cast<LPVOID>(&Hooked_ActorKill),
                          reinterpret_cast<LPVOID*>(&s_origActorKill)) == MH_OK &&
            MH_EnableHook(killTarget) == MH_OK) {
            PLOG_INFO("[DOWNED] Actor::Kill HOOKED @ RVA 0x{:X} (no-death keep-alive, kills death-audio at root)"sv,
                      kActorKillRVA);
        } else {
            PLOG_WARN("[DOWNED] Actor::Kill hook FAILED"sv);
        }
    }

    // Cache the Health ActorValueInfo - copied from NPCSync's CombatProbe_HealthAV pattern.
    static RE::ActorValueInfo* HealthAV()
    {
        if (!g_healthAV) {
            g_healthAV = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                RE::BSFixedString("Health"));
        }
        return g_healthAV;
    }

    static RE::ActorValueInfo* SpeedMultAV()
    {
        static RE::ActorValueInfo* s_av = nullptr;
        if (!s_av) s_av = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
            RE::BSFixedString("SpeedMult"));
        return s_av;
    }

    // Player control-lock - MVP STOPGAP via SpeedMult (immobilize). MUST run on the
    // game thread (ActorValue mutation).
    //
    // The PROPER engine mechanism (RE'd 2026-06-07) is the BSInputEnableLayer system:
    // Papyrus "InputEnableLayer" native registrar = FUN_142028470; the per-layer
    // DisablePlayerControls handler = FUN_142027df0 @RVA 0x2024A90, EnablePlayerControls
    // = FUN_1420281a0 @RVA 0x2024E40 (plus EnableMovement/EnableActivate/EnableVATS/
    // EnableFastTravel/... methods). Using it cleanly needs a BSInputEnableLayer
    // instance + lifetime management → deferred follow-up. For the MVP we immobilize
    // the player via SpeedMult=0 (no extra RE). This stops movement but NOT camera or
    // attack - enough to validate the keep-alive core; full lock comes with the layer.
    static int SafeSetPlayerControlsEnabled(bool a_enabled)
    {
        // SpeedMult while downed = 35% (user wants movement for a showcase
        // video). The pose still reads as hurt; moving leaves the strict idle
        // kneel but is acceptable. A true crawl-locomotion is deferred.
        static constexpr float kDownedSpeedMult = 35.0f;
        static float s_savedSpeedMult = 100.0f;
        __try {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            auto* sm = SpeedMultAV();
            if (!pc || !sm) return 0;
            if (a_enabled) {
                pc->SetBaseActorValue(*sm, s_savedSpeedMult > 0.0f ? s_savedSpeedMult : 100.0f);
            } else {
                const float cur = pc->GetBaseActorValue(*sm);
                if (cur > kDownedSpeedMult) s_savedSpeedMult = cur;  // remember normal speed
                pc->SetBaseActorValue(*sm, kDownedSpeedMult);
            }
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // ----------------------------------------------------------------
    // downed POSE - anim-archetype switch (Papyrus ChangeAnimArchetype).
    // The player's own archetype (AnimArchetypePlayer) lacks most NPC anims;
    // UNA-type mods prove the engine native ChangeAnimArchetype(keyword) swaps
    // the set live. While downed we put player + partner-clone on the INJURED
    // archetype so they read as hurt instead of upright-frozen.
    // KYWD FormIDs verified by Starfield.esm scan (2026-06-10).
    // ----------------------------------------------------------------
    static constexpr std::uint32_t kKywdAnimArchetypeInjured = 0x000B39B2;
    static constexpr std::uint32_t kKywdAnimArchetypePlayer  = 0x0006B508;

    // downed POSE iteration 3 - the REAL engine enter-bleedout.
    // RE chain (2026-06-10): console "ForceBleedout" handler FUN_140c80370 →
    //   FUN_14197f0f0(Actor*) @ RVA 0x197C840 = start BGSBleedoutActivity +
    //   set life-state **8** via vt[+0xB78] (bleedout = 0x100000 bits, NOT
    //   enum 7 - the old dead-end used 7!). The life-state setter has a
    //   dedicated PLAYER path: pushes camera state 0x17 (CS_BLEEDOUT) on
    //   enter, pops it on exit ("Player bleedout" @ 0x14190ed98). State 8
    //   does NOT arm the death-reload tick (it checks 0x20000/0x40000 only)
    //   and our setter hook swallows only the death enums → 8 passes through.
    // ForceBleedout also sets actor+0x378 |= 0x100 (NoBleedoutRecovery) to
    // keep the actor down - we mirror that and clear it on revive.
    // (Iteration 2, ActionBleedoutStart/Stop via PerformAction, was a no-op:
    // the graph transition needs the life-state/activity, not just the action.)
    static constexpr std::uintptr_t kEnterBleedoutRVA = 0x197C840ULL;
    using EnterBleedoutFn = void(__fastcall*)(RE::Actor*);

    // AACT forms (Starfield.esm scan 2026-06-10) - drive the bleedout graph
    // transition directly on the LOCAL player (see ApplyDownedPose).
    static constexpr std::uint32_t kActionBleedoutStart = 0x00013EC9;
    static constexpr std::uint32_t kActionBleedoutStop  = 0x00013ECA;
    // C downed CRAWL: the Injured action drives the graph's MobilityInjured
    // locomotion (NPCs move while injured) - unlike bleedout which is idle-only.
    // Firing it alongside bleedout gives a moving downed pose instead of upright
    // walking at 35%. AACT FormIDs from Starfield.esm (2026-06-11).
    static constexpr std::uint32_t kActionInjuredStart = 0x0001E965;
    static constexpr std::uint32_t kActionInjuredStop  = 0x0001E966;
    // Engine sheathe action - the same dispatcher path the player's manual
    // weapon-holster uses. Fired on the down edge to manufacture the
    // weapon-state change the bleedout pose transition needs.
    static constexpr std::uint32_t kActionSheathe = 0x00046BAF;

    static int SafeEnterBleedout(RE::Actor* a_actor)
    {
        __try {
            reinterpret_cast<EnterBleedoutFn>(
                REL::Offset(kEnterBleedoutRVA).address())(a_actor);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static int SafeSetNoBleedoutRecovery(RE::Actor* a_actor, bool a_no)
    {
        __try {
            auto* flags = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(a_actor) + 0x378);
            if (a_no) *flags |= 0x100u; else *flags &= ~0x100u;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // Stop the actor's active named audio-activity (the "death"/"bleedout" sound +
    // global low-pass muffle that the engine LifeState handler pushes when the
    // player enters death/bleedout). Because PULSAR HOLDS the death status for the
    // downed player, the engine never pops this on its own -> on revive the audio
    // stays dumpf and the down loop keeps playing. This engine helper (RE 2026-06-27)
    // reads the active activity off actor+0xC8 itself, clears the actor flag via
    // vt[+0xB8](0x80000000), and stops it. It has a built-in null-guard (no active
    // activity -> no-op), so it is safe to call unconditionally on every revive.
    using StopActorAudioActivityFn = void(__fastcall*)(RE::Actor*);
    static constexpr std::uintptr_t kStopActorAudioActivityRVA = 0xB6E400ULL;

    static int SafeStopActorAudioActivity(RE::Actor* a_actor)
    {
        __try {
            reinterpret_cast<StopActorAudioActivityFn>(
                REL::Offset(kStopActorAudioActivityRVA).address())(a_actor);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // SEH: force-holster (same engine API the clone weapon-sync uses).
    static int SafeSheatheWeapon(RE::Actor* a_actor)
    {
        __try {
            a_actor->DrawWeaponMagicHands(false);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }


    // DIRECT engine call - NO Papyrus. Both Papyrus attempts crashed:
    // (1) PackVariable(TESForm*) -> stale REL::ID(0) fatal (VehicleSync trap),
    // (2) int-FormID variant -> AV 0x90 synchronously INSIDE DispatchStaticCall
    //     (crash helper FUN_1422cdd10, null arena; suspected on-demand load of
    //     the new pex). The Papyrus native handler FUN_141fc8b00 (Ghidra-RE'd
    //     2026-06-10) just validates "keyword type 7" and calls the CORE:
    //       FUN_14179a030(Actor*, BGSKeyword*) @ RVA 0x17976C0
    //     = remove current type-7 keyword(s), add new one, notify the anim
    //     graph (process+0x228), refresh. kw == nullptr -> reset to default.
    using ChangeAnimArchetypeCoreFn = std::uint64_t(__fastcall*)(RE::Actor*, RE::TESForm*);
    static constexpr std::uintptr_t kChangeAnimArchetypeCoreRVA = 0x17976C0ULL;

    static int SafeChangeAnimArchetypeCall(RE::Actor* a_actor, RE::TESForm* a_kw)
    {
        __try {
            auto fn = reinterpret_cast<ChangeAnimArchetypeCoreFn>(
                REL::Offset(kChangeAnimArchetypeCoreRVA).address());
            fn(a_actor, a_kw);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static bool DispatchChangeAnimArchetype(RE::Actor* a_actor, std::uint32_t a_kwFormID)
    {
        if (!a_actor) return false;
        RE::TESForm* kw = nullptr;
        if (a_kwFormID) {
            kw = RE::TESForm::LookupByID(a_kwFormID);
            if (!kw) {
                PLOG_WARN("[DOWNED] anim-archetype keyword 0x{:X} not found"sv, a_kwFormID);
                return false;
            }
        }
        const int ok = SafeChangeAnimArchetypeCall(a_actor, kw);
        PLOG_INFO("[DOWNED] anim-archetype direct call kw=0x{:X} ok={}"sv, a_kwFormID, ok);
        return ok == 1;
    }

    // Combined downed-pose apply/revert: injured archetype + bleedout pose.
    //
    // CLONE  = REAL engine bleedout: FUN_14197f0f0 (activity + life-state 8) +
    //          NoBleedoutRecovery; exit = clear flag + raw life-state 0. The
    //          clone is not input-driven, so the state-8 input lock is moot.
    // PLAYER = GRAPH-ONLY bleedout: life-state 8 looks right but the engine
    //          locks player movement input in that state (in-game 2026-06-10:
    //          "konnte mich nicht bewegen") - bleedout is a cutscene state for
    //          the player. Keep life-state 0 (alive → input works → 10% crawl
    //          speed) and drive only the graph: ActionBleedoutStart/Stop + a
    //          per-tick iSyncBleedout=1 hold (the exact recipe that holds the
    //          clone's pose).
    static void ApplyDownedPose(RE::Actor* a_actor, bool a_downed, const char* a_who)
    {
        DispatchChangeAnimArchetype(a_actor,
            a_downed ? kKywdAnimArchetypeInjured
                     : (SafeIsLocalPlayer(a_actor) ? kKywdAnimArchetypePlayer : 0u));
        int ok;
        if (SafeIsLocalPlayer(a_actor)) {
            // No shooting while downed: swallow combat actions in the dispatcher.
            // NOTE: do NOT force-holster here - DrawWeaponMagicHands plays a
            // sheathe animation that overrides the bleedout pose, and blocking
            // ActionDraw made the engine spam draw-attempts every frame (log
            // 2026-06-11: 'player combat action BLOCKED: ActionDraw' 1/s) which
            // kept the graph oscillating instead of holding the crawl pose. The
            // combat-action block alone stops actual shooting; the weapon just
            // stays in hand (cosmetic).
            Pulsar::RemotePlayer::SetPlayerCombatActionsBlocked(a_downed);
            // USER INSIGHT (2026-06-11): a DRAWN weapon blocks the bleedout pose
            // ("kommt nicht in die animation"); the pose only engaged back when
            // the weapon was holstered. So sheathe on the down edge AND hold
            // iSyncWeaponDrawState=0 every tick (see OnGameThreadTick) so the
            // engine's combat auto-draw can't re-draw it and pop the pose
            // ("will kurz auf den boden gehen aber kommt dann wieder hoch").
            // Draw is NOT blocked (that spammed retries) - holding the graph var
            // keeps it visually sheathed without fighting the dispatcher.
            // Pose trigger via weapon-state change DISABLED for the equip-off
            // isolation test (2026-06-11). The DrawWeaponMagicHands toggle and
            // the ActionSheathe dispatch both ran extra graph ops on the down
            // edge; with equip off we want the minimal down path to see if the
            // both-down pure-virtual crash is the equip apply or this. The
            // weapon just stays as-is; pose may need a manual sheathe for now.
            Pulsar::RemotePlayer::SetActorGraphInt(a_actor, "iSyncWeaponDrawState", a_downed ? 0 : 1);
            Pulsar::RemotePlayer::SetActorGraphInt(a_actor, "iSyncBleedout", a_downed ? 1 : 0);
            // Bleedout = the stable idle downed pose (confirmed working with a
            // sheathed weapon). ActionInjured (MobilityInjured) was tried for a
            // moving downed pose but pulled the player back UPRIGHT out of the
            // bleedout idle ("steht immer wieder auf", 2026-06-11 test) - it's an
            // upright limp, not a ground crawl. Reverted to bleedout-only for a
            // stable pose; a true downed-crawl is a separate harder problem
            // (the player biped graph has no bleedout LOCOMOTION state).
            ok = Pulsar::RemotePlayer::PerformActionOnActor(
                     a_actor, a_downed ? kActionBleedoutStart : kActionBleedoutStop) ? 1 : 0;
            PLOG_INFO("[DOWNED] bleedout {} (graph-only, weapon sheathed) on {} ok={}"sv,
                      a_downed ? "ENTER" : "EXIT", a_who, ok);
            return;
        }
        if (a_downed) {
            SafeSetNoBleedoutRecovery(a_actor, true);
            ok = SafeEnterBleedout(a_actor);
        } else {
            SafeSetNoBleedoutRecovery(a_actor, false);
            ok = SafeSetLifeStateRaw(a_actor, 0);
        }
        PLOG_INFO("[DOWNED] bleedout {} (engine path) on {} ok={}"sv,
                  a_downed ? "ENTER" : "EXIT", a_who, ok);
    }

    // SEH-isolated: read health, dead flag, and raw boolBits/boolFlags words.
    // Uses the exact GetActorValue accessor pattern from NPCSync.
    static bool SafeReadState(RE::Actor*          a_actor,
                              RE::ActorValueInfo* a_av,
                              float&              a_health,
                              bool&               a_dead,
                              std::uint32_t&      a_rawBits,
                              std::uint32_t&      a_rawFlags)
    {
        __try {
            a_health   = a_actor->GetActorValue(*a_av);
            a_dead     = a_actor->IsDead();
            a_rawBits  = a_actor->boolBits.underlying();
            a_rawFlags = a_actor->boolFlags.underlying();
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH-isolated: raise effective health TO a_target by injecting a positive
    // kDamage-modifier delta equal to the current deficit.
    //
    // Root cause of the bug this replaces: SetActorValue writes the base permanent
    // value. The player's large negative effective health comes from the kDamage
    // modifier accumulating weapon-hit negatives (same modifier the combat-sync
    // damage path uses). GetActorValue = base + all modifiers, so a SetActorValue
    // write never moved the number we read back. Proved by in-game logs:
    //   [DOWNED] HP-floor clamp health=-213.8 -> 25.0   (SetActorValue call)
    //   [DOWNED-PROBE] health=-213.8 ...                (still -213.8 next tick)
    //
    // Fix: ModActorValue(kDamage, +delta) adds to the kDamage bucket. Because
    // kDamage normally holds a large negative, a positive add reduces its
    // magnitude and raises GetActorValue's result. This is the exact inverse
    // of NPCSync's damage-apply path (NPCSync.cpp:1706-1707).
    //
    // Verified against:
    // NPCSync.cpp:1706-1707: actor->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
    //       *healthAV, -d.damage, hostPlayer)   [4-arg overload, source = hostPlayer]
    // ActorValueOwner.h vt[06]: ModActorValue(ACTOR_VALUE_MODIFIER, const ActorValueInfo&,
    //       float, TESObjectREFR* a_ref)
    // We pass the player itself as a_ref (same pattern - the source actor of the
    // modifier). No C++ objects with destructors inside __try (C2712 safe).
    static int SafeRaiseHealthTo(RE::Actor* a_actor, RE::ActorValueInfo* a_av, float a_target)
    {
        __try {
            const float cur   = a_actor->GetActorValue(*a_av);
            const float delta = a_target - cur;
            if (delta > 0.0f) {
                // 4-arg overload: ModActorValue(modifier, av, delta, sourceRef)
                // Matches NPCSync.cpp:1706-1707 call shape exactly.
                a_actor->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *a_av, delta, a_actor);
            }
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // SEH-isolated: read max (permanent) health.
    // Mirrors NPCSync::CombatProbe_SafeRead line 59: actor->GetPermanentActorValue(*av).
    // Verified against ActorValueOwner.h vt[0x02].
    static int SafeReadMaxHealth(RE::Actor* a_actor, RE::ActorValueInfo* a_av, float& a_out)
    {
        __try {
            a_out = a_actor->GetPermanentActorValue(*a_av);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Shared revive primitive - used by F10 debug revive, Way A (medpack), and
    // Way B (combat-end). Always runs on the downed player's OWN machine.
    // KEEP-ALIVE model: the player was never dead (we swallowed the death-set), so
    // revive is just: clear the downed flag, tell the peer, UNFREEZE controls, and
    // restore Health. No life-state change / Resurrect needed.
    // g_remoteDowned is intentionally NOT touched here - it mirrors the PEER's state.
    static void ReviveLocalPlayer(RE::PlayerCharacter* a_player,
                                  RE::ActorValueInfo*  a_av,
                                  float                a_hpFraction,
                                  const char*          a_reason)
    {
        if (!a_player || !a_av) return;

        g_localDowned = false;
        SendDownedState(false);
        SafeSetPlayerControlsEnabled(true);  // unfreeze (we locked controls on entry)
        // Kill the held death/bleedout audio (muffle + down loop) - the engine
        // can't pop it because we hold the death status. Null-guarded, safe always.
        SafeStopActorAudioActivity(a_player);

        float maxHealth = 0.0f;
        float restoreHP = kDownedFloor;
        const bool gotMax = (SafeReadMaxHealth(a_player, a_av, maxHealth) && maxHealth > 0.0f);
        if (gotMax) restoreHP = maxHealth * a_hpFraction;
        SafeRaiseHealthTo(a_player, a_av, restoreHP);

        PLOG_INFO("[DOWNED] revive ({}) -> health={:.1f}{}"sv,
                  a_reason, restoreHP, gotMax ? "" : " (max-read failed)");
    }

    void Init()
    {
        InstallHooks();
        g_inited = true;
        PLOG_INFO("[DOWNED] CoopMortality initialized"sv);
    }

    void OnGameThreadTick()
    {
        if (!g_inited) return;

        // Gate: only run during an active co-op session.
        if (!Pulsar::Network::GetSingleton().IsConnected()) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        auto* av = HealthAV();
        if (!av) return;

        // ----------------------------------------------------------------
        // Read current health every tick (NOT throttled) - the clamp must
        // be responsive enough to outrun the game-over watcher.
        // ----------------------------------------------------------------
        float         health   = 0.0f;
        bool          dead     = false;
        std::uint32_t rawBits  = 0;
        std::uint32_t rawFlags = 0;

        if (!SafeReadState(player, av, health, dead, rawBits, rawFlags)) return;

        // ----------------------------------------------------------------
        // Way A - consume a queued medpack-revive request (set on the ENet
        // thread by OnReviveReceived). Engine mutation runs here on the game
        // thread. 50% HP - an item-paid revive.
        // ----------------------------------------------------------------
        if (g_medpackRevivePending.exchange(false, std::memory_order_relaxed)) {
            if (g_localDowned) {
                ReviveLocalPlayer(player, av, 0.5f, "medpack");
            } else {
                PLOG_INFO("[DOWNED] medpack revive request dropped (not downed)"sv);
            }
        }

        // ----------------------------------------------------------------
        // Post-load / post-revive recovery (load-INDEPENDENT).
        //
        // The death-tick hook (FUN_141a5ba50) only ticks while the player is in a
        // death life-state, so after a both-downed game-over reload it never runs
        // again - its auto-recover branch can't fire and g_localDowned stays stuck
        // at 1 (observed: DOWNED-PROBE downed=1 with health 240/dead=0 after the
        // load).  This game-thread tick always runs, so do the recovery here:
        // if we are flagged downed but the +0xF8 life-state reads alive (bits == 0),
        // leave downed and tell the peer.  During normal single-down the stored
        // life-state is still death (masking is transient inside the hook only), so
        // this never fires spuriously.  We also clear g_remoteDowned: a both-downed
        // game-over reloads BOTH sides, and the peer re-asserts downed=1 via packet
        // if it goes down again - this prevents a stale remoteDowned from turning a
        // later single death into a false both-downed game-over.
        // ----------------------------------------------------------------
        // Post-load recovery (LOAD-TICK keyed). Under keep-alive the player is ALWAYS
        // life-state alive while downed, so recovery can NOT key off life-state==0 (it
        // would fire every frame). Instead: when a game LOAD completes (e.g. the
        // both-downed game-over reload), clear stale downed flags and unfreeze.
        {
            static std::uint64_t s_lastLoadSeen = 0;
            const std::uint64_t loadTick = Pulsar::WorldSync::GetLastLoadCompletedTick();
            // ALSO recover on a loadMenu close edge. The both-down game-over is a
            // VANILLA death-reload, which does NOT bump GetLastLoadCompletedTick
            // (that's only the save-bridge LOADED path) - so the load-tick keyed
            // recovery never fired and the client stayed downed/blocked after the
            // reload (2026-06-11: couldn't shoot post-reload). WasInLoadMenu()
            // true→false is the reliable "a load just finished" edge for any load.
            static bool s_prevInLoadMenu = false;
            const bool nowInLoadMenu = Pulsar::Cell3DAttach::WasInLoadMenu();
            const bool loadMenuClosedEdge = s_prevInLoadMenu && !nowInLoadMenu;
            s_prevInLoadMenu = nowInLoadMenu;

            if (loadTick != s_lastLoadSeen || loadMenuClosedEdge) {
                s_lastLoadSeen = loadTick;
                if (g_localDowned || g_remoteDowned.load(std::memory_order_relaxed)) {
                    g_localDowned = false;
                    g_remoteDowned.store(false, std::memory_order_relaxed);
                    g_remoteCombatActive.store(false, std::memory_order_relaxed);
                    SafeSetPlayerControlsEnabled(true);
                    Pulsar::RemotePlayer::SetPlayerCombatActionsBlocked(false);
                    SendDownedState(false);
                    PLOG_INFO("[DOWNED] post-load recovery - cleared downed state + unfroze (loadMenuEdge={})"sv,
                              loadMenuClosedEdge ? 1 : 0);
                }
            }
        }

        // ----------------------------------------------------------------
        // Both-downed game-over (game-thread trigger).
        //
        // Under keep-alive the player is never in a death life-state, so the death-tick
        // hook (FUN_141a5ba50) is never called and could not trigger the game-over - it
        // MUST live on this always-running tick (the previously KNOWN GAP). When BOTH
        // sides are flagged down, run the REAL vanilla death once (SafeRunDeathHandler →
        // the trampoline, bypassing our swallow). That puts the player in a true death
        // state, so the engine's death tick starts firing again and Hook_PlayerDeathTick's
        // bothDowned branch arms the save-reload (clearing the 2nd-load trap counter).
        // Each machine decides this independently from its own two flags → both converge
        // to the reload. We early-return while pending so no revive path interferes.
        // ----------------------------------------------------------------
        {
            static bool s_bothDownTriggered = false;
            const bool bothDown = g_localDowned && g_remoteDowned.load(std::memory_order_relaxed);
            if (bothDown) {
                SafeClearLoadCounter();  // keep the load counter ≤ 1 every frame (host 2nd-load trap)
                if (!s_bothDownTriggered) {
                    s_bothDownTriggered = true;
                    PLOG_INFO("[DOWNED] BOTH players downed -> real death for game-over reload"sv);
                    SafeRunDeathHandler(player);
                }
                return;  // skip revive / clamp paths while the game-over reload is pending
            }
            s_bothDownTriggered = false;
        }

        // Control-lock edge + HP clamp (game thread). The setter hook (worker thread)
        // set g_localDowned; apply the freeze here once on the false→true edge, and
        // keep HP > 0 every frame so the engine stops issuing player death-sets.
        {
            static bool s_controlsLocked = false;
            // Gate the pose edge on !IsQuiet(): ApplyDownedPose fires graph +
            // weapon (PerformAction) ops, which pure-virtual-crash if the edge
            // lands during a reload teardown (host crash 21:54). During a reload
            // the recovery clears g_localDowned anyway, so deferring the edge
            // costs nothing.
            const bool poseSafe = !Pulsar::Cell3DAttach::IsQuiet();
            if (g_localDowned && !s_controlsLocked && poseSafe) {
                s_controlsLocked = true;
                SafeSetPlayerControlsEnabled(false);  // freeze (SpeedMult=0)
                ApplyDownedPose(player, true, "player");
                PLOG_INFO("[DOWNED] downed pose applied (bleedout, frozen)"sv);
            } else if (!g_localDowned && s_controlsLocked) {
                s_controlsLocked = false;
                SafeSetPlayerControlsEnabled(true);   // restore normal SpeedMult
                SafeStopActorAudioActivity(player);   // clear held death/bleedout audio (ungated by poseSafe)
                if (poseSafe) ApplyDownedPose(player, false, "player");
            }
            if (g_localDowned && health < kDownedFloor) {
                SafeRaiseHealthTo(player, av, kDownedFloor);
            }
            // Graph-only player bleedout: re-assert iSyncBleedout=1 every 100ms
            // while downed (the engine's player-graph update can reset it; the
            // clone needs the same hold - see RemotePlayerManager anim sync).
            // CRASH FIX (2026-06-11): gate the graph-var hold on !IsQuiet().
            // During the both-down game-over reload, g_localDowned is still set
            // and this hold kept calling SetActorGraphInt on the player whose
            // anim-graph was mid-teardown → pure-virtual call (STATUS
            // 0xC0000025, NOT SEH-catchable; client crash 18:49). IsQuiet() is
            // true while the loadMenu is open + grace, so we stop touching the
            // graph during the reload.
            // Combat-action block is SELF-HEALING: bind it to g_localDowned
            // every tick instead of only toggling it inside ApplyDownedPose.
            // After a both-down reload the recovery clears g_localDowned but the
            // edge that calls ApplyDownedPose(false) didn't always fire, so the
            // block stayed stuck and the client couldn't shoot post-reload
            // (2026-06-11: 'combat action BLOCKED: ActionFireSingle' forever).
            Pulsar::RemotePlayer::SetPlayerCombatActionsBlocked(g_localDowned);

            static std::uint64_t s_poseHoldMs = 0;
            const std::uint64_t poseNowMs = ::GetTickCount64();
            if (g_localDowned && !Pulsar::Cell3DAttach::IsQuiet()
                && poseNowMs - s_poseHoldMs >= 100) {
                s_poseHoldMs = poseNowMs;
                Pulsar::RemotePlayer::SetActorGraphInt(player, "iSyncBleedout", 1);
                // Keep the weapon sheathed - combat auto-draw otherwise re-draws
                // it and pops the bleedout pose (user-confirmed: a drawn weapon
                // blocks the pose). Force-holster every 100ms AND hold the graph
                // var; log the weapon bits so we can see if the holster sticks.
                Pulsar::RemotePlayer::SetActorGraphInt(player, "iSyncWeaponDrawState", 0);
                const std::uint8_t weapBits =
                    static_cast<std::uint8_t>(player->actorState2 & 0x3);
                // Only re-holster when the weapon is actually out (bits != 0) -
                // calling DrawWeaponMagicHands(false) every tick would restart
                // the sheathe anim endlessly. weapBits != 0 = combat auto-draw
                // pulled it back; force it down again.
                if (weapBits != 0) SafeSheatheWeapon(player);
                static std::uint64_t s_weapDiagMs = 0;
                if (poseNowMs - s_weapDiagMs >= 1000) {
                    s_weapDiagMs = poseNowMs;
                    PLOG_INFO("[DOWNED] pose-hold: weaponBits={} (0=sheathed,3=drawn)"sv, weapBits);
                }
            }
        }

        // downed pose on the PARTNER-CLONE (helper side): mirror the injured
        // archetype so the downed partner reads as hurt instead of upright-frozen.
        // Reset (None = clone's default archetype) when the partner revives.
        // Gate clone-pose graph ops on !IsQuiet() too - during the both-down
        // reload the clone is mid-teardown; ApplyDownedPose's graph/engine calls
        // on it would pure-virtual-call (same crash class as the player hold).
        if (!Pulsar::Cell3DAttach::IsQuiet()) {
            static bool s_cloneInjured = false;
            const bool remoteDown = g_remoteDowned.load(std::memory_order_relaxed);
            if (remoteDown && !s_cloneInjured) {
                if (RE::Actor* clone = SafeGetRemoteClone()) {
                    s_cloneInjured = true;
                    // Flag FIRST: the very next anim-sync tick must already hold
                    // iSyncBleedout=1, else it aborts the pose one tick in.
                    Pulsar::RemotePlayer::SetCloneBleedoutPose(true);
                    ApplyDownedPose(clone, true, "clone");
                }
            } else if (!remoteDown && s_cloneInjured) {
                s_cloneInjured = false;
                Pulsar::RemotePlayer::SetCloneBleedoutPose(false);
                if (RE::Actor* clone = SafeGetRemoteClone()) {
                    ApplyDownedPose(clone, false, "clone");
                }
            }
        }

        // ----------------------------------------------------------------
        // Combat scan + broadcast (always while connected) - feeds Way-B on BOTH sides.
        // ONE SafeHostilesNear scan per 250 ms drives:
        //   (1) our broadcast: the DEBOUNCED "active fight near me" state, so a downed
        //       partner can gate its revive on OUR (the fighter's) combat. SafeHostilesNear
        //       is reliable on the FIGHTING machine (real enemies carry the combat flags);
        //       the downed player's own scan is not (its enemies disengage instantly), so
        //       the downed side leans on this remote signal. Sent on change AND on a
        //       500 ms HEARTBEAT - on-change-only was lost to the connect-time load barrier
        //       / a post-load reset and never re-asserted (confirmed 2026-06-09).
        //   (2) Way-B below, which reuses s_localHostiles / s_localHostilesOk.
        // We suppress our own broadcast while WE are downed (we are not the fighter).
        // ----------------------------------------------------------------
        static std::uint64_t s_combatScanMs   = 0;
        static bool          s_localHostiles   = true;   // last scan result (shared with Way-B)
        static bool          s_localHostilesOk = false;
        {
            static bool          s_combatStable     = false;  // our debounced broadcast state
            static std::uint64_t s_combatClearStart = 0;
            static int           s_lastSentCombat   = -1;      // -1 = nothing sent yet
            static std::uint64_t s_lastSentMs       = 0;
            constexpr std::uint64_t kCombatEndDebounceMs = 1000;  // ride out between-wave gaps
            constexpr std::uint64_t kHeartbeatMs         = 500;
            const std::uint64_t nowMs = ::GetTickCount64();
            if (nowMs - s_combatScanMs >= 250) {
                s_combatScanMs = nowMs;
                bool hostiles = true;
                s_localHostilesOk = SafeHostilesNear(hostiles);
                s_localHostiles   = hostiles;
                // SafeHostilesNear proved UNRELIABLE - it returns 0 hostiles even
                // mid-fight on the fighting machine (downed=0, in active combat;
                // confirmed 2026-06-09). Use the engine's own player combat flag
                // (IsInCombat) as the broadcast signal instead. Suppressed while WE
                // are downed (IsInCombat sticks true on the kept-alive player).
                bool pInCombat = false;
                const bool inCombatOk = SafeIsInCombat(player, pInCombat);

                const bool raw = inCombatOk && pInCombat && !g_localDowned;
                if (raw) {
                    s_combatStable     = true;
                    s_combatClearStart = 0;
                } else if (s_combatStable) {
                    if (s_combatClearStart == 0) s_combatClearStart = nowMs;
                    if (nowMs - s_combatClearStart >= kCombatEndDebounceMs) s_combatStable = false;
                }
                const int  cur     = s_combatStable ? 1 : 0;
                const bool changed = (cur != s_lastSentCombat);
                if (changed || nowMs - s_lastSentMs >= kHeartbeatMs) {
                    s_lastSentCombat = cur;
                    s_lastSentMs     = nowMs;
                    SendCombatStatus(s_combatStable, /*log=*/changed);
                }

                // DIAG ~1 Hz: confirm the FIGHTING machine's SafeHostilesNear actually
                // detects combat (the whole gate depends on it). Remove once verified.
                static std::uint64_t s_diagCMs = 0;
                if (nowMs - s_diagCMs >= 1000) {
                    s_diagCMs = nowMs;
                    PLOG_INFO("[COMBAT-DIAG] inCombat={} hostilesNear={} stable={} downed={}"sv,
                              pInCombat ? 1 : 0, s_localHostiles ? 1 : 0,
                              s_combatStable ? 1 : 0, g_localDowned ? 1 : 0);
                }
            }
        }

        // ----------------------------------------------------------------
        // Way B - combat-end auto-revive. While downed, once BOTH our local hostiles
        // scan AND the partner's broadcast report no combat for kCombatClearMs
        // continuously, stand back up at 25% HP. The remote gate is essential: the
        // downed player's own enemies disengage the moment it goes down, so its local
        // SafeHostilesNear reads "clear" mid-fight - without the partner's signal it
        // pops up in the middle of the host's ongoing fight (confirmed 2026-06-09).
        // Reuses the shared scan above (s_localHostiles / s_localHostilesOk).
        // ----------------------------------------------------------------
        if (g_localDowned) {
            static std::uint64_t s_clearSinceMs = 0;   // 0 = combat present / unknown
            constexpr std::uint64_t kCombatClearMs = 1500;
            const std::uint64_t nowMs = ::GetTickCount64();
            const bool ok           = s_localHostilesOk;
            const bool hostilesNear = s_localHostiles;
            const bool remoteCombat = g_remoteCombatActive.load(std::memory_order_relaxed);

            // DIAG: log the combined combat signal ~1 Hz while downed.
            static std::uint64_t s_diagBMs = 0;
            if (nowMs - s_diagBMs >= 1000) {
                s_diagBMs = nowMs;
                PLOG_INFO("[REVIVE-DIAG] wayB downed=1 hostilesOk={} hostilesNear={} remoteCombat={}"sv,
                          ok ? 1 : 0, hostilesNear ? 1 : 0, remoteCombat ? 1 : 0);
            }

            if (ok && !hostilesNear && !remoteCombat) {
                if (s_clearSinceMs == 0) s_clearSinceMs = nowMs;
                if (nowMs - s_clearSinceMs >= kCombatClearMs) {
                    s_clearSinceMs = 0;
                    ReviveLocalPlayer(player, av, 0.25f, "combat-over");
                }
            } else {
                s_clearSinceMs = 0;  // local OR remote combat (or read failed) → reset
            }
        }

        // ----------------------------------------------------------------
        // Way A - medpack revive (helper side), v2: proximity + Activate TAP.
        // If a peer is downed and we are NOT downed, a rising-edge press of the
        // Activate key within kReviveRadius of the downed clone sends the revive.
        // (v1 required a 2 s continuous hold - disproven in-game: the tester
        // tapped E expecting an interact, so the hold never completed. Tap is the
        // natural "interact" gesture.) Item check + consume = next iteration.
        // ----------------------------------------------------------------
        {
            static bool          s_activatePrev = false;
            static std::uint64_t s_lastFireMs   = 0;
            const std::uint64_t  nowMs = ::GetTickCount64();
            const bool activateDown = (::GetAsyncKeyState(kActivateVK) & 0x8000) != 0;

            if (!g_localDowned && g_remoteDowned.load(std::memory_order_relaxed)) {
                std::uintptr_t clonePtr = 0;
                float          distSq   = -1.0f;
                const bool inRange = SafeNearDownedClone(player, kReviveRadius, clonePtr, distSq);

                // DIAG ~1 Hz.
                static std::uint64_t s_diagAMs = 0;
                if (nowMs - s_diagAMs >= 1000) {
                    s_diagAMs = nowMs;
                    PLOG_INFO("[REVIVE-DIAG] wayA remoteDowned=1 clone=0x{:X} distSq={:.0f} "
                              "radiusSq={:.0f} inRange={} activateE={}"sv,
                              clonePtr, distSq, kReviveRadius * kReviveRadius,
                              inRange ? 1 : 0, activateDown ? 1 : 0);
                }

                // Rising edge + in range + cooldown → trigger.
                const bool rising = activateDown && !s_activatePrev;
                if (rising && inRange && (nowMs - s_lastFireMs >= 1000)) {
                    s_lastFireMs = nowMs;
                    auto& net = Pulsar::Network::GetSingleton();
                    const std::uint8_t downedSlot = net.IsHost() ? 1 : 0;  // the OTHER player
                    SendRevive(downedSlot);
                    PLOG_INFO("[DOWNED] medpack revive triggered for slot {} (v2 tap; no item consume yet)"sv,
                              downedSlot);
                }
            }
            s_activatePrev = activateDown;  // track edge even when not applicable
        }

        // ----------------------------------------------------------------
        // HP-floor MAINTAIN clamp - DISABLED for the bleedout model. Bleedout is
        // HP-linked: raising Health > 0 while downed can make the engine auto-recover
        // the player out of bleedout uncontrolled. We deliberately leave Health low
        // so bleedout stays stable until an explicit revive (which raises HP itself).
        // ----------------------------------------------------------------
        // (intentionally no HP clamp while downed)

        // ----------------------------------------------------------------
        // Debug manual-revive (dev-only) - edge-triggered, configurable VK.
        // Gated behind devHotkeys so it doesn't fire/collide for normal players.
        // ----------------------------------------------------------------
        if (Pulsar::GetConfig().devHotkeys) {
            static bool s_reviveKeyDown = false;
            const bool  keyNow = (::GetAsyncKeyState(Pulsar::GetConfig().hotkeyDevManualRevive) & 0x8000) != 0;
            if (keyNow && !s_reviveKeyDown) {
                // Rising edge
                if (g_localDowned) {
                    ReviveLocalPlayer(player, av, 0.5f, "manual-revive");
                }
            }
            s_reviveKeyDown = keyNow;
        }

        // ----------------------------------------------------------------
        // Diagnostic probe - throttled to ~4 Hz (250 ms).
        // Always log at low HP; also log when downed regardless of HP.
        // ----------------------------------------------------------------
        static std::uint64_t s_lastLogMs = 0;
        const std::uint64_t  nowMs2      = ::GetTickCount64();
        if (nowMs2 - s_lastLogMs < 250) return;

        if (health > 20.0f && !g_localDowned) return;  // only log near-death or while downed

        s_lastLogMs = nowMs2;
        PLOG_INFO(
            "[DOWNED-PROBE] health={:.1f} dead={} downed={} boolBits=0x{:08X} boolFlags=0x{:08X}"sv,
            health,
            dead ? 1 : 0,
            g_localDowned ? 1 : 0,
            rawBits,
            rawFlags);
    }

    // Called on the ENet receive thread when the peer broadcasts a downed-state
    // transition. We only mirror the bit; the game-thread hook reads it.
    void OnDownedReceived(const std::uint8_t* a_data, std::size_t a_len)
    {
        using namespace Pulsar::Network;
        if (!a_data || a_len < 1 + sizeof(PlayerDownedData)) return;
        PlayerDownedData d{};
        std::memcpy(&d, a_data + 1, sizeof(d));
        g_remoteDowned.store(d.downed != 0, std::memory_order_relaxed);
        PLOG_INFO("[DOWNED] peer downed-state received = {} (slot {})"sv, d.downed, d.playerSlot);
    }

    // True while the remote partner (their clone on this machine) is downed.
    // EquipSync gates its clone equip-apply on this so the armor/weapon 3D/graph
    // update doesn't reset the bleedout pose.
    bool IsRemoteDowned()
    {
        return g_remoteDowned.load(std::memory_order_relaxed);
    }

    bool IsLocalDowned()
    {
        return g_localDowned.load(std::memory_order_relaxed);
    }

    // ship destruction → both-down game-over. When the (shared) ship's hull
    // reaches 0 the ship explodes, but that does NOT kill the on-foot/piloting
    // player (verified from logs: no death-handler, no reload). So there is no
    // death to intercept - instead route the local player into the downed state
    // directly, exactly as the keep-alive death-handler does on a lethal hit
    // (set g_localDowned + notify the peer). Both machines call this (host on its
    // own hull-read, client on the mirrored PKT_SHIP_AV), so both flag down + tell
    // the other → OnGameThreadTick's both-downed trigger runs the real death and
    // reloads BOTH sides - the proven both-down scheme. We deliberately do NOT
    // force the life-state here: the both-down handler calls SafeRunDeathHandler
    // itself once both flags are set, and an early forced death would fight the
    // keep-alive HP-clamp/control-lock and fling the ragdoll. Idempotent.
    void TriggerShipDowned()
    {
        if (!Pulsar::Network::GetSingleton().IsConnected()) return;
        if (!g_localDowned.exchange(true)) {
            SendDownedState(true);  // tell the peer we are down (drives bothDowned)
            PLOG_INFO("[DOWNED] ship destruction (hull=0) -> entered downed (both-down game-over)"sv);
        }
    }

    // ENet receive thread - mirror the peer's debounced combat state. The game-thread
    // Way-B revive gate reads g_remoteCombatActive so a downed player stays down while
    // the (non-downed) partner is still fighting.
    void OnCombatStatusReceived(const std::uint8_t* a_data, std::size_t a_len)
    {
        using namespace Pulsar::Network;
        if (!a_data || a_len < 1 + sizeof(PlayerCombatData)) return;
        PlayerCombatData d{};
        std::memcpy(&d, a_data + 1, sizeof(d));
        g_remoteCombatActive.store(d.inCombat != 0, std::memory_order_relaxed);
        PLOG_INFO("[DOWNED] peer combat-state received = {} (slot {})"sv, d.inCombat, d.playerSlot);
    }

    // ENet receive thread - queue only (engine mutation happens on the game
    // thread in OnGameThreadTick). The partner sends this with the DOWNED
    // player's slot; we act only if that is us and we are actually downed.
    void OnReviveReceived(const std::uint8_t* a_data, std::size_t a_len)
    {
        using namespace Pulsar::Network;
        if (!a_data || a_len < 1 + sizeof(PlayerReviveData)) return;
        PlayerReviveData d{};
        std::memcpy(&d, a_data + 1, sizeof(d));

        auto& net = Pulsar::Network::GetSingleton();
        const std::uint8_t mySlot = net.IsHost() ? 0 : 1;
        if (d.playerSlot != mySlot) return;

        g_medpackRevivePending.store(true, std::memory_order_relaxed);
        PLOG_INFO("[DOWNED] medpack revive request received (slot {})"sv, d.playerSlot);
    }

    // Way A - robust trigger via the engine Activate core (REL::ID 63380, hooked in
    // LootSync). Fires when the local player activates the downed partner-clone via
    // the real interact input (key, rebind, controller - anything the engine maps),
    // unlike the GetAsyncKeyState('E') tap which never registered on the client
    // machine. The clone activation is consumed so the talk dialog never opens.
    bool OnActivateMaybeRevive(RE::TESObjectREFR* a_self, RE::TESObjectREFR* a_activator)
    {
        if (!a_self || !a_activator) return false;
        if (!Pulsar::Network::GetSingleton().IsConnected()) return false;
        if (g_localDowned || !g_remoteDowned.load(std::memory_order_relaxed)) return false;
        if (!SafeIsLocalPlayer(reinterpret_cast<RE::Actor*>(a_activator))) return false;

        // DIAG: while the partner is downed, log what the player's interact actually
        // hits - tells us whether the clone produces Activate calls at all.
        const bool isClone = SafeIsRemoteClone(reinterpret_cast<RE::Actor*>(a_self));
        PLOG_INFO("[REVIVE-DIAG] wayA activate self=0x{:X} isClone={}"sv,
                  reinterpret_cast<std::uintptr_t>(a_self), isClone ? 1 : 0);
        if (!isClone) return false;

        static std::uint64_t s_lastFireMs = 0;
        const std::uint64_t  nowMs = ::GetTickCount64();
        if (nowMs - s_lastFireMs < 1000) return true;  // debounce, still consume
        s_lastFireMs = nowMs;

        const std::uint8_t downedSlot = Pulsar::Network::GetSingleton().IsHost() ? 1 : 0;
        SendRevive(downedSlot);
        PLOG_INFO("[DOWNED] medpack revive triggered for slot {} (Activate on downed clone; no item consume yet)"sv,
                  downedSlot);
        return true;
    }

}  // namespace Pulsar::Coop::Mortality
