#include "PCH.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "RemotePlayer/EquipSync.h"   // kSyncWeapons/kSyncArmor gates
#include "Config.h"
#include "Coop/CoopMortality.h"        // IsRemoteDowned() - gate equip-apply while downed
#include "VehicleSync/VehicleSync.h"
#include "FlashlightSync/FlashlightSync.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "HookGuard.h"
#include "Cell3DAttach.h"

#include <RE/A/ActorValueInfo.h>
#include <RE/B/BSFixedString.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiUpdateData.h>
#include <RE/N/NiPoint.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESGlobal.h>
#include <RE/B/BGSInventoryItem.h>
#include <RE/B/BGSObjectInstance.h>
#include <RE/T/TESObjectWEAP.h>
#include "Animation/BoneSync.h"
#include "Network/PacketTypes.h"
#include "CloneManager/CloneManager.h"
#include "Engine/ProcGenGate.h"
#include "NPCSync/NPCSync.h"
#include "WorldSync/LootSync.h"   // F.2-v2: ApplyItemMods on clone equip
#include "NASLog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <unordered_set>
#include <MinHook.h>

using namespace std::string_view_literals;

namespace Pulsar::RemotePlayer
{
    static RemotePlayerManager g_remotePlayerManager;

    // weapon-swap fix fwd-decl - defined near DoEquipWeapon, used in ProcessPending.
    static void SafeForceWeaponSheathedState(RE::Actor* a_actor) noexcept;

    void RemotePlayerManager::SetRemoteCellID(uint32_t a_cellID)
    {
        m_remoteCellID.store(a_cellID, std::memory_order_release);
        m_remoteCellAtMs.store(GetTickCount64(), std::memory_order_release);
    }

    RemotePlayerManager& GetRemotePlayerManager()
    {
        return g_remotePlayerManager;
    }

    // --- EvaluatePackage hook via MinHook (blocks AI on clone) ---
    using EvaluatePackage_t = void(__fastcall*)(RE::Actor*, bool, bool);
    static EvaluatePackage_t s_origEvaluatePackage = nullptr;
    static RE::Actor* s_hookCloneActor = nullptr;
    static bool s_mhInitialized = false;

    // also block the PLAYER-clone's engine AI-eval in proc cells (static
    // cells already block it). The April-2026 silent-death from blocking it came
    // from the clone losing collision/navmesh upkeep - but DoSetPosition now drives
    // its Havok body every frame, so that prerequisite is gone. Gated for instant
    // revert: set false to fall back to the StopCombat-only safety net (Layer 1).
    static std::atomic<bool> g_blockPlayerCloneAiInProc{ true };

    // belt (Layer 1): forcibly pull the player-clone out of any engine combat
    // each tick so its (now weapon-synced) gun can't autonomously machine-gun NPCs.
    // Cheap-gated on currentCombatTarget (+0x298) so StopCombat() only fires when the
    // clone is actually in combat. SEH-guarded (fragile proc-cell actor). Replayed
    // cosmetic fire (, forced PerformAction) is NOT AI combat → unaffected.
    static void NeutralizeCloneCombatSafe(RE::Actor* a_clone) noexcept
    {
        if (!a_clone) return;
        __try {
            uint32_t* tgt = reinterpret_cast<uint32_t*>(
                reinterpret_cast<std::uint8_t*>(a_clone) + 0x298);
            if (*tgt != 0) {
                a_clone->StopCombat();   // CommonLibSF resolves the correct vtable slot
                *tgt = 0;                // clear currentCombatTarget handle
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static int SafeCallEvaluatePackage(RE::Actor* a_this, bool a_immediate, bool a_resetAI)
    {
        __try {
            s_origEvaluatePackage(a_this, a_immediate, a_resetAI);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static uint32_t s_lastCrashID = 0;
            uint32_t id = a_this ? a_this->GetFormID() : 0;
            if (id != s_lastCrashID) {
                s_lastCrashID = id;
                PLOG_WARN("[PULSAR] SEH in EvaluatePackage for actor 0x{:08X}"sv, id);
            }
            return 0;
        }
    }

    // SEH-wrapped cell-prefix check. In procedural cells the engine reshuffles
    // parentCell pointers while cells page in/out; an unlocked deref of
    // actor->parentCell->formID races with that and derefs freed memory. The
    // __try swallows the AV; a result of 0 means "not proc" or "unknown".
    static int IsActorInProcCell(RE::Actor* a_actor)
    {
        __try {
            auto* cell = a_actor ? a_actor->parentCell : nullptr;
            if (!cell) return 0;
            return ((cell->formID & 0xFF000000) == 0xFF000000) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Returns true if this actor is one we drive remotely (player's clone OR
    // NPCSync-spawned remote NPC clone). Same AI-suppression treatment as
    // the player clone - static cells block AI, proc cells pass through
    // (collision/navmesh state still needed).
    static bool IsRemoteOwnedActor(RE::Actor* a_actor)
    {
        if (!a_actor) return false;
        if (a_actor == s_hookCloneActor) return true;
        __try {
            return Pulsar::NPCSync::IsRemoteOwned(a_actor->formID);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static void __fastcall HookedEvaluatePackage(RE::Actor* a_this, bool a_immediate, bool a_resetAI)
    {
        // HookGuard: bail to engine during save-load + cell-stream chaos.
        // Our IsRemoteOwnedActor check reads form fields that engine save-load
        // invalidates → UAF risk. Suspend covers it.
        if (!Pulsar::HookGuard::IsSafe()) {
            SafeCallEvaluatePackage(a_this, a_immediate, a_resetAI);
            return;
        }

        // 2026-04-19 REVERT: in proc cells EvaluatePackage must pass through.
        // Test showed blocking it causes silent-death crash after ~6s of walking
        // (no log marker - process dies in an engine thread). Empirically the
        // engine relies on package-eval for clone's collision/navmesh state even
        // when action-dispatch is fully blocked. Static cells still get the full
        // block (cities work fine with this). Planet clones get AI-eval pass-
        // through but all PerformAction calls blocked (the action-block alone
        // was enough to bring planet animations closer to city quality).
        //
        // 2026-04-20 Widened to all remote-owned actors (player clone +
        // NPCSync-spawned NPC clones). Needed because PlaceAtMe'd NPCs
        // auto-follow the client as companions otherwise.
        if (IsRemoteOwnedActor(a_this)) {
            // P3 combat-live: let this NPC's engine combat AI run (so it aims
            // and fires visibly + engages the local player). Position stays loosely
            // host-corrected in TickLerp. Overrides the AI-block below.
            if (Pulsar::NPCSync::IsNpcCombatLive(a_this->formID)) {
                SafeCallEvaluatePackage(a_this, a_immediate, a_resetAI);
                return;
            }
            if (IsActorInProcCell(a_this) == 1) {
                // the PLAYER-clone never needs its own AI in proc cells now
                // that DoSetPosition drives its Havok body every frame - block its
                // AI-eval so the engine combat package can't autonomously fire its
                // (now weapon-synced) gun. Gated for instant revert if the April
                // silent-death returns (then Layer-1 StopCombat carries it). Only the
                // player-clone - NPC clones keep their existing proc-cell behavior.
                if (a_this == s_hookCloneActor &&
                    g_blockPlayerCloneAiInProc.load(std::memory_order_relaxed)) {
                    return;  // block - no AI eval for the player clone
                }
                // P1: the 2026-04-19 proc-cell silent-crash was blocking AI
                // here WITHOUT a position writer - the engine relied on package-eval
                // for the actor's collision/navmesh state. We now drive the Havok
                // CC body every frame (SafeNpcHavokBodySetPos), which maintains that
                // state, so retry the block in proc cells for DirectControl-puppet
                // NPCs only (gated npcDirectControl). Clones keep the safe pass-
                // through. If this reintroduces the silent-death, set the ini to 0.
                if (!Pulsar::NPCSync::IsNpcAiBlocked(a_this->formID)) {
                    SafeCallEvaluatePackage(a_this, a_immediate, a_resetAI);
                }
                return;
            }
            return;  // static-cell remote-owned - block AI
        }
        SafeCallEvaluatePackage(a_this, a_immediate, a_resetAI);
    }

    static void InstallEvalPackageHook()
    {
        if (!s_mhInitialized) {
            MH_Initialize();
            s_mhInitialized = true;
        }
        if (s_origEvaluatePackage) return;  // Already hooked

        uintptr_t addr = REL::ID(150640).address();
        MH_STATUS status = MH_CreateHook(
            reinterpret_cast<LPVOID>(addr),
            &HookedEvaluatePackage,
            reinterpret_cast<LPVOID*>(&s_origEvaluatePackage));

        if (status == MH_OK) {
            MH_EnableHook(reinterpret_cast<LPVOID>(addr));
            PLOG_INFO("[PULSAR] EvaluatePackage hook installed via MinHook"sv);
        } else {
            PLOG_WARN("[PULSAR] MinHook failed: {}"sv, static_cast<int>(status));
        }
    }

    // --- PerformAction hook via MinHook (blocks ALL actions on clone) ---
    // Skyrim Together pattern: block all actions for remote actors,
    // allow forced actions through via g_forceAnimation flag.
    // DAT_145fb7bf0 is a global function pointer = Starfield's PerformAction dispatch.
    // NOTE: Skyrim Together uses thread_local, but Starfield dispatches actions on
    // different threads than the caller. Using atomic instead.
    std::atomic<bool> g_forceAnimation{false};
    static uint64_t s_forceAnimUntil = 0;  // timer for g_forceAnimation bypass window

    // Pitch bone overlay state (Phase 0: sinus test, later: network pitch)
    static BoneSync::BoneCache s_clonePitchBoneCache;
    static RE::Actor* s_clonePitchActor = nullptr;

    // IMovementDirectControl state (initialized in ProbeMovementController)
    static void* s_directCtrl = nullptr;
    static void** s_directCtrlVtable = nullptr;
    static bool s_directCtrlEnabled = false;
    static bool s_movCtrlProbed = false;

    // DirectControl vtable method types
    using DCEnable_t    = void(__fastcall*)(void* thisPtr);
    using DCSetSpeed_t  = void(__fastcall*)(void* thisPtr, float speed);
    using DCSetTarget_t = void(__fastcall*)(void* thisPtr, void* pos, float speed);

    // SEH wrappers (can't mix __try with C++ objects in same function)
    static bool SafeCallDCEnable(void* ctrl, void* func)
    {
        __try {
            reinterpret_cast<DCEnable_t>(func)(ctrl);
            return true;
        } __except(1) { return false; }
    }

    static bool SafeCallDCSetTarget(void* ctrl, void* func, void* pos, float speed)
    {
        __try {
            reinterpret_cast<DCSetTarget_t>(func)(ctrl, pos, speed);
            return true;
        } __except(1) { return false; }
    }

    static bool SafeCallDCSetSpeed(void* ctrl, void* func, float speed)
    {
        __try {
            reinterpret_cast<DCSetSpeed_t>(func)(ctrl, speed);
            return true;
        } __except(1) { return false; }
    }

    // L.7 D3 - SEH-guard GetAnimationGraphManagerImpl. The clone's
    // IAnimationGraphManagerHolder subobject (actor+0x60) has a null/garbage
    // vtable while the clone is being torn down / rebuilt during rapid space
    // transitions (host takeoff/gravjump/landing). The virtual call through it
    // then faults (READ at 0x18 in DoSetPosition - the 2026-05-25 space-flight
    // CTD; clone was 0x0 / "No MovementController" in the log). DoSetPosition
    // runs on BSTaskPool threads concurrently with clone respawn, so it races
    // the teardown. Skipping graph writes for one tick is harmless (the next
    // tick re-applies once the clone is stable again).
    static bool SafeGetAnimGraphManager(
        RE::Actor* a_actor,
        RE::BSTSmartPointer<RE::BSAnimationGraphManager>& a_gMgr)
    {
        __try {
            return a_actor->GetAnimationGraphManagerImpl(a_gMgr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH: is the clone fully 3D-attached? loadedData @ actor+0xB8 (BSGuarded
    // ptr), data3D @ loadedData+0x08 (NiAVObject*). After a both-down reload the
    // clone respawns but its 3D is null for a few frames - running DoEquipArmor's
    // Set3DUpdateFlag (vt+0x4b0) on it then null-vtable-faults (host crash 18:31).
    static bool SafeCloneHas3D(RE::Actor* a_actor)
    {
        __try {
            if (!a_actor) return false;
            auto* ld = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(a_actor) + 0xB8);
            if (!ld) return false;
            auto* d3d = *reinterpret_cast<void**>(ld + 0x08);
            return d3d != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // SEH wrapper for the vtable Set3DUpdateFlag call. SafeCloneHas3D's null
    // check is not enough: data3D can be non-null while the actor's vtable is
    // still a teardown sentinel after a both-down respawn, so the virtual call
    // faults READ at 0xFFFF... (client crash 20:40, RVA 0x1431B6). This AV IS
    // SEH-catchable (unlike the pure-virtual 0xC0000025 case).
    static int SafeSet3DUpdateFlag(RE::Actor* a_actor, std::uint32_t a_flag)
    {
        __try {
            if (a_actor) a_actor->Set3DUpdateFlag(a_flag);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // SEH-guarded NotifyAnimationGraphImpl. The clone (m_remoteActor) can be
    // non-null but pointing at an actor the engine invalidated (remote player
    // death/reload/cell-stream) - an unguarded virtual call then faults with
    // READ at 0xFFFFFFFFFFFFFFFF (null vtable slot). Mirrors SafeGetAnimGraphManager's
    // guard. Returns true on success, false on AV (skipping one anim event is harmless).
    static bool SafeNotifyAnimGraph(RE::Actor* a_actor, const RE::BSFixedString& a_event)
    {
        if (!a_actor) return false;
        __try {
            a_actor->NotifyAnimationGraphImpl(a_event);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SafeCallPerformAction defined after PerformAction_t
    using PerformActionFn_t = uint8_t(__fastcall*)(void*);
    static bool SafeCallPerformAction(PerformActionFn_t func, void* actionData)
    {
        __try {
            func(actionData);
            return true;
        } __except(1) { return false; }
    }

    using PerformAction_t = uint8_t(__fastcall*)(void*);
    static PerformAction_t s_origPerformAction = nullptr;

    // downed pose - while the remote player is downed, the per-tick anim
    // sync must assert iSyncBleedout=1 on the clone instead of zeroing it
    // (the hard 0 yanked the clone straight back out of the bleedout state).
    static std::atomic<bool> s_cloneBleedoutPose{ false };
    void SetCloneBleedoutPose(bool a_on)
    {
        s_cloneBleedoutPose.store(a_on, std::memory_order_relaxed);
    }

    // downed - while the LOCAL player is downed, swallow their combat
    // actions in the PerformAction hook (first person still has the weapon
    // usable otherwise: fire/reload/draw all dispatch through here).
    static std::atomic<bool> s_playerCombatBlock{ false };
    void SetPlayerCombatActionsBlocked(bool a_on)
    {
        s_playerCombatBlock.store(a_on, std::memory_order_relaxed);
    }

    // Combat V2-1e: timestamp of the local player's last ATTACK action
    // (fire/melee/throw). NPCSync's damage-forward gates on this so only
    // player-attributable health drops get forwarded - without it, the
    // clone's replayed (visual) projectiles and NPC-vs-NPC fights were
    // misclassified as client damage and forwarded (double counting).
    static std::atomic<std::uint64_t> s_lastLocalPlayerAttackMs{ 0 };
    std::uint64_t GetLastLocalPlayerAttackMs()
    {
        return s_lastLocalPlayerAttackMs.load(std::memory_order_relaxed);
    }

    // downed pose - generic PerformAction on any actor (player or clone),
    // same ActionData layout the clone fire-sync uses. Calls the MinHook
    // trampoline directly, so the clone AI-action filter does not interfere.
    bool PerformActionOnActor(RE::Actor* a_actor, uint32_t a_actionFormID)
    {
        if (!a_actor || !s_origPerformAction) return false;
        auto* actionForm = RE::TESForm::LookupByID(a_actionFormID);
        if (!actionForm) return false;

        static REL::Relocation<void*> s_bgaVtable{ REL::ID(393260) };
        static REL::Relocation<void*> s_aoVtable{ REL::ID(393264) };

        alignas(16) uint8_t actionData[0x68]{};
        *reinterpret_cast<void**>(actionData + 0x00) = s_bgaVtable.get();
        *reinterpret_cast<RE::Actor**>(actionData + 0x08) = a_actor;
        *reinterpret_cast<RE::TESForm**>(actionData + 0x18) = actionForm;
        *reinterpret_cast<void**>(actionData + 0x28) = s_aoVtable.get();
        return SafeCallPerformAction(s_origPerformAction, actionData);
    }

    // Track which action IDs we've already logged (log each unique action once with name)
    // Accessed from multiple action-dispatch threads - must be protected by s_actionLogMutex.
    static std::unordered_set<uint32_t> s_loggedActionIDs;
    static std::mutex s_actionLogMutex;

    // Actions that MUST be allowed through for the clone to have a valid pose
    // Without these the clone stays in T-Pose
    static bool IsWhitelistedAction(uint32_t formID)
    {
        switch (formID) {
            case 0x0005704C: // ActionInstantInitializeGraphToBaseState
            case 0x0002FFA9: // ActionInitializeGraphToBaseState
            // 2026-04-19 REMOVED: ActionFall (0x000937F4) + ActionLand (0x000937F5).
            // These fire when the engine's physics detects the clone is airborne
            // (which it is - we snap Z to host's jump apex). The engine's Fall/Land
            // handling then mutates physics state that races with our SetPosition
            // writes and crashed the client (2026-04-19 log timestamp 18:20:28).
            // Jump animation is driven by iSyncJumpState graph variable + the
            // direct target-Z during jump states, so Fall/Land actions are
            // redundant now.
            //
            // ActionSighted (0x00004A57) NOT whitelisted - triggers engine timeout
            // that auto-releases after ~3s. Graph vars control sighted state instead.
                return true;
            default:
                return false;
        }
    }

    // 2026-04-24: name-prefix whitelist for locomotion / pose actions.
    // Graph-var writes (FacingBackward, IsBoostPackActive, iSyncStrafeState,
    // TurnDeltaSmoothed, ...) need the engine's Action dispatch to trigger
    // the anim state-machine transitions - without those events the graph
    // var sits read but never commits the new state. The original whitelist
    // was formID-only and let only 2 init actions through, so Boost/Walk/
    // Backward/Turn/Strafe events never reached the clone's anim tree.
    //
    // Matching on EditorID prefix: covers ActionBoostpack_DO, ActionWalk*,
    // ActionRun*, ActionSprint*, ActionStrafe*, ActionTurn*, ActionFace*,
    // ActionSwim*, ActionIdle*, ActionJump*, ActionLand, ActionFall, etc.
    // Combat / fire / interact actions (ActionAttack*, ActionFire*, Action-
    // Reload*, ActionActivate*) are NOT prefixed with any of these and
    // remain blocked - preserves the Sarah-combat / shooting-crash fixes.
    static bool IsLocomotionAction(RE::BGSAction* action)
    {
        if (!action) return false;
        const char* name = action->GetFormEditorID();
        if (!name) return false;
        static const char* const kPrefixes[] = {
            "ActionWalk",    "ActionRun",     "ActionSprint",
            "ActionMove",    "ActionIdle",    "ActionTurn",
            "ActionStrafe",  "ActionLean",    "ActionBoost",
            "ActionHover",   "ActionJetpack", "ActionZeroG",
            "ActionBackward","ActionFace",    "ActionStumble",
            "ActionFlinch",  "ActionInjured", "ActionSwim",
            "ActionFall",    "ActionLand",    "ActionJump",
            "ActionPost",    "ActionChargen", "ActionBleedout",
            // P1: combat-anim actions. The combat AI dispatches these even
            // with packages blocked; they drive the aim-stance / combat-move /
            // cover / fire ANIMATIONS. Without them a synced combat NPC can't
            // enter the combat-movement state → "tippel steps" instead of a real
            // walk/strafe, and never plays the fire/cover anim. (Movement DECISION
            // is still blocked via EvaluatePackage - this only lets the anims play;
            // host-authoritative fire timing is a later step.)
            "ActionCombat",  "ActionChange",  "ActionFire",
            "ActionAttack",  "ActionShoot",   "ActionReload",
            "ActionAim",     "ActionCover",   "ActionMelee",
            "ActionPowerAttack",
            // Weapon-ready / alert stance - needed to enter the combat-movement
            // state (ActionGunReady/GunAlert were BLOCKED → tippel-steps).
            "ActionGun",
        };
        for (const char* p : kPrefixes) {
            const char* nm = name;
            const char* pp = p;
            while (*pp && *nm && *nm == *pp) { ++nm; ++pp; }
            if (*pp == '\0') return true;   // prefix matched
        }
        return false;
    }

    // Track desired weapon state so PerformAction can block conflicting engine actions
    static std::atomic<bool> g_wantWeaponDrawn{false};

    // SEH-wrapped SetPosition + angle write. Proc-cell clones race with engine
    // cell transitions; the engine's SetPosition can touch the cell pointer.
    static bool SafeSetActorPosition(RE::Actor* actor, const RE::NiPoint3A& pos, float angleZ)
    {
        __try {
            actor->SetPosition(pos, true);
            actor->data.angle.z = angleZ;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Proc-cell-safe position write. Ghidra RE (T1/T3/T4) showed that
    // Actor::SetPosition's stage-1 (FUN_140B4EB30) rewrites the cell
    // quadrant hashmap unconditionally and races with proc-gen streaming
    // that swaps parentCell. Bypass: write OBJ_REFR data.location (+0x8C)
    // directly, then mirror into the loaded NiAVObject's local.translate
    // (+0x70). NiAVObject::UpdateWorldData is lock-free; a torn render
    // read is at worst one-frame visual, never memory corruption.
    // Inner raw-write + immediate transform propagation. Split from
    // SafeProcCellWriteNode because __try can't coexist with the
    // BSAutoReadLock destructor. Caller holds the read lock.
    // Uses a raw byte buffer instead of RE::NiUpdateData{} to avoid C++
    // object unwinding inside __try (C2712).
    static void UnsafeWriteNodeTranslate(RE::NiAVObject* node, const RE::NiPoint3A& pos)
    {
        __try {
            auto* t = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(node) + 0x70);  // local.translate (T4)
            t[0] = pos.x;
            t[1] = pos.y;
            t[2] = pos.z;

            // Force immediate world.translate recompute. Without this the
            // render thread can sample stale world for up to one frame while
            // our local.translate is already updated → visible tremble.
            // Raw zero-init buffer + default non-zero fields (see NiUpdateData).
            alignas(8) uint8_t updateBuf[0x60]{};
            *reinterpret_cast<uint32_t*>(updateBuf + 0x38) = 0x10;  // flags
            *reinterpret_cast<uint32_t*>(updateBuf + 0x3C) = 1;     // flags2
            *reinterpret_cast<uint32_t*>(updateBuf + 0x40) = 1;     // flags3
            node->UpdateTransforms(reinterpret_cast<RE::NiUpdateData*>(updateBuf));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // 3D freed between lock acquire and deref / UpdateTransforms AV.
        }
    }

    // Layer 3: write the 3D node's local.translate under loadedData's
    // BSReadWriteLock read-lock. Prevents the proc-cell data3D-detach race
    // documented in T2/T4. No __try here - BSAutoReadLock needs unwinding.
    static void SafeProcCellWriteNode(RE::Actor* actor, const RE::NiPoint3A& pos)
    {
        auto guard = actor->loadedData.LockRead();
        const auto* data = *guard;
        if (!data || !data->data3D) return;
        UnsafeWriteNodeTranslate(data->data3D.get(), pos);
    }

    // Havok Stage-3 emulation - reversed from Actor::SetPosition via Ghidra T1
    // (VA 0x14193B410 decomp). Calls hknpBody::setPosition + setLinearVelocity(0)
    // + updateBroadphase + cleanup, all without going through Stage 1
    // (cell-refmap) which is the proc-cell crasher.
    //
    // Path: actor+0x228 (currentProcess) → +0x08 (middleHigh) → +0x458 (body pointer).
    // vtable[0x240] = setPosition(NiPoint4*, bool)
    // vtable[0x278] = setLinearVelocity(NiPoint4*)
    // FUN_1423607E0 @ VA 0x14235E1F0 = hknpWorld::updateBroadphaseAfterTransformChange
    // FUN_1423792A0 @ VA 0x142376C70 = hkArrayBase<hknpBodyId>::operator[]
    // FUN_1423B48A0 @ VA 0x1423B2290 = releaseCachedShape/Constraint
    // Cache module base at first-use to avoid REX object construction inside __try.
    static uintptr_t GetStarfieldBaseCached()
    {
        static uintptr_t s_base = REX::FModule::GetExecutingModule().GetBaseAddress();
        return s_base;
    }

    // Inner helper. Calls bhkCharRigidBodyController::setPosition directly via
    // its vtable. T7+T8+T9 RE confirmed:
    //   CC at middleHigh+0x458 = bhkCharRigidBodyController (vtable 0x144D67400)
    //   CC + 0x20 = hknpCharacterProxy/body wrapper (null until Havok attach)
    //   vtable[72] @ 0x1423D27B0 = setPosition(this, float* pos4, char flag1, char flag2)
    //   vtable[79] @ 0x1423D36D0 = setLinearVelocity(this, float* vel4)
    //   vtable[87] @ 0x1423BD670 = hknpWorld-getter (null-safe: returns 0 if CC+0x20 null)
    //
    // setPosition internally calls vtable[87] and dereferences its return at
    // +0x30 WITHOUT a null check → crash on fresh CC where Havok-pipeline
    // hasn't attached the body yet. Solution: gate the call on CC+0x20 != null.
    static void SafeHavokBodySetPositionImpl(RE::Actor* actor, float x, float y, float z)
    {
        __try {
            auto u = reinterpret_cast<uintptr_t>(actor);
            uintptr_t aiProc = *reinterpret_cast<uintptr_t*>(u + 0x228);
            if (!aiProc) return;
            uintptr_t middleHigh = *reinterpret_cast<uintptr_t*>(aiProc + 0x08);
            if (!middleHigh) return;
            void* ccObj = *reinterpret_cast<void**>(middleHigh + 0x458);
            if (!ccObj) return;

            // Havok-attach gate - TWO levels (matches slot 87's null checks):
            //   CC+0x20 = body wrapper (null until attach start)
            //   *(CC+0x20)+0x20 = inner Havok object (null until attach complete)
            uintptr_t bodyWrapper = *reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uintptr_t>(ccObj) + 0x20);
            if (!bodyWrapper) return;
            uintptr_t innerHavokObj = *reinterpret_cast<uintptr_t*>(bodyWrapper + 0x20);
            if (!innerHavokObj) return;

            auto** vtable = *reinterpret_cast<void***>(ccObj);
            if (!vtable) return;

            alignas(16) float pos4[4] = { x, y, z, 0.0f };

            // Only setPosition - setLinearVelocity (slot 85 path) has separate
            // null constraints we haven't fully RE'd yet.
            using SetPosFn = void(__fastcall*)(void*, float*, char, char);
            auto setPos = reinterpret_cast<SetPosFn>(vtable[0x240 / 8]);
            setPos(ccObj, pos4, 1, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Body race despite gate; Layer 2 (data.location) still updated.
        }
    }

    static void SafeHavokBodySetPosition(RE::Actor* actor, const RE::NiPoint3A& pos)
    {
        SafeHavokBodySetPositionImpl(actor, pos.x, pos.y, pos.z);
    }

    // Havok setLinearVelocity - vtable slot 79 (byte 0x278). T8 reversed the
    // signature as `void(ccObj, float* vel4)` - 2 params, 4-float vector.
    // Needed so the NPC anim graph sees proper physics state during jumps:
    // graph checks hknpBody.linearVelocity.z to pick JumpUp / JumpApex /
    // JumpFall transitions. Without this write, graph sees zero velocity +
    // iSyncJumpState=1 and gets stuck in weird blend ("ball" artifact).
    static void SafeHavokBodySetLinearVelocityImpl(RE::Actor* actor, float vx, float vy, float vz)
    {
        __try {
            auto u = reinterpret_cast<uintptr_t>(actor);
            uintptr_t aiProc = *reinterpret_cast<uintptr_t*>(u + 0x228);
            if (!aiProc) return;
            uintptr_t middleHigh = *reinterpret_cast<uintptr_t*>(aiProc + 0x08);
            if (!middleHigh) return;
            void* ccObj = *reinterpret_cast<void**>(middleHigh + 0x458);
            if (!ccObj) return;

            // Same two-level attach gate as setPosition.
            uintptr_t bodyWrapper = *reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uintptr_t>(ccObj) + 0x20);
            if (!bodyWrapper) return;
            uintptr_t innerHavokObj = *reinterpret_cast<uintptr_t*>(bodyWrapper + 0x20);
            if (!innerHavokObj) return;

            auto** vtable = *reinterpret_cast<void***>(ccObj);
            if (!vtable) return;

            alignas(16) float vel4[4] = { vx, vy, vz, 0.0f };

            using SetVelFn = void(__fastcall*)(void*, float*);
            auto setVel = reinterpret_cast<SetVelFn>(vtable[0x278 / 8]);
            setVel(ccObj, vel4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Velocity write raced with body detach - same pattern as setPos.
        }
    }

    // SEH-guarded raw write of refr->parentCell (offset 0xB0). Isolated into
    // its own plain-C function because __try is incompatible with functions
    // needing C++ object unwinding (C2712).
    static int SafePinParentCell(RE::Actor* actor, RE::TESObjectCELL* tgtCell)
    {
        __try {
            if (!actor || !tgtCell) return 0;
            if (actor->parentCell != tgtCell) actor->parentCell = tgtCell;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Layer-2-only: just update refr+0x8C data.location + data.angle.z.
    // Used when visible movement is driven by DirectControl (CC-aware path)
    // so we don't also write NiAVObject directly (would conflict with CC).
    static bool SafeWriteActorDataLocation(RE::Actor* actor, const RE::NiPoint3A& pos, float angleZ)
    {
        __try {
            auto* locPtr = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(actor) + 0x8C);
            locPtr[0] = pos.x;
            locPtr[1] = pos.y;
            locPtr[2] = pos.z;
            actor->data.angle.z = angleZ;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool SafeProcCellPositionWrite(RE::Actor* actor, const RE::NiPoint3A& pos, float angleZ)
    {
        // Layer 2: raw data.location write (OBJ_REFR at +0x80, location at +0xC → +0x8C).
        __try {
            auto* locPtr = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(actor) + 0x8C);
            locPtr[0] = pos.x;
            locPtr[1] = pos.y;
            locPtr[2] = pos.z;
            actor->data.angle.z = angleZ;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        // Layer 3: visible transform update, locked against data3D detach.
        SafeProcCellWriteNode(actor, pos);
        return true;
    }

    // SEH-wrapped actor read. actionData is a live engine struct, but the actor
    // it points to could be racing with cell transitions in proc cells.
    static RE::Actor* SafeReadActor(void* actionData)
    {
        __try {
            return *reinterpret_cast<RE::Actor**>(
                reinterpret_cast<uintptr_t>(actionData) + 0x08);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    static uint8_t __fastcall HookedPerformAction(void* actionData)
    {
        if (!actionData) return 0;

        // HookGuard: bail to engine during save-load + cell-stream chaos.
        // SafeReadActor + IsRemoteOwnedActor walk pointers that engine
        // save-load may have just freed.
        if (!Pulsar::HookGuard::IsSafe()) {
            return s_origPerformAction(actionData);
        }

        RE::Actor* actor = SafeReadActor(actionData);
        if (!actor) {
            // Can't identify actor safely - just forward to original and bail.
            return s_origPerformAction(actionData);
        }

        // 2026-04-20: widened to all remote-owned actors (player clone +
        // NPCSync-spawned NPC clones). Same blocking logic - NPC clones
        // should only play anims driven by host packets, not engine actions.
        if (IsRemoteOwnedActor(actor)) {
            // P3 combat-live: let this NPC's own combat actions (fire/attack/
            // reload/etc.) execute so it visibly shoots + damages the local player.
            if (Pulsar::NPCSync::IsNpcCombatLive(actor->formID)) {
                return s_origPerformAction(actionData);
            }
            RE::BGSAction* action = *reinterpret_cast<RE::BGSAction**>(
                reinterpret_cast<uintptr_t>(actionData) + 0x18);
            uint32_t formID = action ? action->GetFormID() : 0;

            ///(2026-06-20): while the remote partner is DOWNED, the
            // engine's own state machine fires ActionBleedoutStop (0x00013ECA) on
            // the FULL-HEALTH clone ~0.5s after we put it into engine-bleedout -
            // which cancels the downed knee-pose and stands the clone right back up
            // ("kurz auf den Boden, kommt dann wieder hoch", verified Sottle log
            // 09:57:55). Swallow Stop while the remote is downed so the pose holds.
            // The legit revive uses SafeSetLifeStateRaw (CoopMortality::ApplyDownedPose,
            // engine path), NOT this action, so blocking it never blocks getting up.
            if (formID == 0x00013ECA &&
                Pulsar::Coop::Mortality::IsRemoteDowned()) {
                std::lock_guard<std::mutex> lock(s_actionLogMutex);
                if (s_loggedActionIDs.find(formID) == s_loggedActionIDs.end()) {
                    s_loggedActionIDs.insert(formID);
                    PLOG_INFO("[PULSAR] BLOCKED ActionBleedoutStop on downed clone (hold pose)"sv);
                }
                return 0;
            }

            // Seated bypass - when VehicleSync has the clone in a sit state,
            // let EVERYTHING through so the engine's sit/stand sequence can
            // actually play. Our normal PerformAction block caused the sit
            // to start (engine queued ActionGetUp) but then immediately get
            // cancelled, which is why the clone looked like it was floating
            // in front of the FURN instead of sitting in it.
            if (actor == s_hookCloneActor &&
                Pulsar::VehicleSync::IsCloneSeated(actor)) {
                return s_origPerformAction(actionData);
            }

            // Always allow whitelisted actions (graph initialization etc.)
            if (IsWhitelistedAction(formID)) {
                {
                    std::lock_guard<std::mutex> lock(s_actionLogMutex);
                    if (s_loggedActionIDs.find(formID) == s_loggedActionIDs.end()) {
                        s_loggedActionIDs.insert(formID);
                        const char* editorID = action->GetFormEditorID();
                        PLOG_INFO("[PULSAR] ALLOWED action 0x{:08X} '{}' on clone (whitelisted)"sv,
                            formID, editorID ? editorID : "?");
                    }
                }
                return s_origPerformAction(actionData);
            }

            // 2026-04-24: allow all locomotion/pose actions through by
            // name-prefix. Required for boost, walk, turn, strafe, backward-
            // walk state transitions to commit on the clone anim tree.
            if (IsLocomotionAction(action)) {
                {
                    std::lock_guard<std::mutex> lock(s_actionLogMutex);
                    if (s_loggedActionIDs.find(formID) == s_loggedActionIDs.end()) {
                        s_loggedActionIDs.insert(formID);
                        const char* editorID = action->GetFormEditorID();
                        PLOG_INFO("[PULSAR] ALLOWED action 0x{:08X} '{}' on clone (locomotion)"sv,
                            formID, editorID ? editorID : "?");
                    }
                }
                return s_origPerformAction(actionData);
            }

            // During force window: allow forced actions but block conflicting ones
            // e.g. SneakStart triggers engine ActionSheathe - block if weapon should stay drawn
            if (g_forceAnimation) {
                // Block sheathe if weapon should be drawn
                if (formID == 0x00046BAF && g_wantWeaponDrawn) {  // ActionSheathe
                    return 0;
                }
                // Block draw if weapon should be holstered
                if (formID == 0x000132AF && !g_wantWeaponDrawn) {  // ActionDraw
                    return 0;
                }
                return s_origPerformAction(actionData);
            }

            // Not forced: block all actions on clone
            if (action) {
                std::lock_guard<std::mutex> lock(s_actionLogMutex);
                if (s_loggedActionIDs.find(formID) == s_loggedActionIDs.end()) {
                    s_loggedActionIDs.insert(formID);
                    const char* editorID = action->GetFormEditorID();
                    PLOG_INFO("[PULSAR] BLOCKED action 0x{:08X} '{}' idx={} on clone"sv,
                        formID,
                        editorID ? editorID : "?",
                        action->index);
                }
            }
            return 0;
        }

        // Log ALL actor actions for fire tracking (player + Sarah + any NPC)
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (actor != s_hookCloneActor) {
            RE::BGSAction* anyAction = *reinterpret_cast<RE::BGSAction**>(
                reinterpret_cast<uintptr_t>(actionData) + 0x18);
            if (anyAction) {
                const char* anyEditorID = anyAction->GetFormEditorID();
                if (anyEditorID) {
                    std::string_view anyName(anyEditorID);
                    if (anyName.find("Fire") != std::string_view::npos ||
                        anyName.find("Attack") != std::string_view::npos ||
                        anyName.find("Reload") != std::string_view::npos ||
                        anyName.find("attack") != std::string_view::npos) {
                        bool isSarah = (actor != player);
                        static std::unordered_set<uint32_t> s_npcFireIDs;
                        uint32_t aID = anyAction->GetFormID();
                        if (isSarah) {
                            std::lock_guard<std::mutex> lock(s_actionLogMutex);
                            if (s_npcFireIDs.find(aID) == s_npcFireIDs.end()) {
                                s_npcFireIDs.insert(aID);
                                PLOG_INFO("[PULSAR] NPC FIRE ACTION 0x{:08X} '{}' actor=0x{:08X}"sv,
                                    aID, anyEditorID, actor->GetFormID());
                            }
                        }
                    }
                }
            }
        }

        if (actor == player) {
            RE::BGSAction* action = *reinterpret_cast<RE::BGSAction**>(
                reinterpret_cast<uintptr_t>(actionData) + 0x18);
            if (action) {
                uint32_t formID = action->GetFormID();
                const char* editorID = action->GetFormEditorID();
                // Log fire/attack actions (log each unique one once)
                static std::unordered_set<uint32_t> s_playerActionIDs;
                {
                    std::lock_guard<std::mutex> lock(s_actionLogMutex);
                    if (s_playerActionIDs.find(formID) == s_playerActionIDs.end()) {
                        s_playerActionIDs.insert(formID);
                        PLOG_INFO("[PULSAR] PLAYER ACTION 0x{:08X} '{}' idx={}"sv,
                            formID, editorID ? editorID : "?", action->index);
                    }
                }
                // while the local player is downed, swallow combat actions
                // entirely - no shot fires, no fire-packet to the peer. Locomotion
                // and ActionBleedout* don't match any of these substrings.
                if (editorID && s_playerCombatBlock.load(std::memory_order_relaxed)) {
                    std::string_view bname(editorID);
                    // Block actual ATTACKS only. Draw/Sighted/Release are NOT
                    // blocked: blocking them made the engine retry every frame
                    // (ActionDraw spam) which fought the bleedout pose. Without
                    // a fire/attack the weapon stays harmless even if drawn.
                    if (bname.find("Fire") != std::string_view::npos ||
                        bname.find("Attack") != std::string_view::npos ||
                        bname.find("Reload") != std::string_view::npos ||
                        bname.find("Melee") != std::string_view::npos ||
                        bname.find("Throw") != std::string_view::npos ||
                        bname.find("Grenade") != std::string_view::npos) {
                        static std::uint64_t s_blockLogMs = 0;
                        const std::uint64_t bNow = GetTickCount64();
                        if (bNow - s_blockLogMs >= 1000) {
                            s_blockLogMs = bNow;
                            PLOG_INFO("[DOWNED] player combat action BLOCKED: 0x{:08X} '{}'"sv,
                                formID, editorID);
                        }
                        return 0;  // swallow while downed
                    }
                }
                // Detect fire actions by name (we don't know FormIDs yet)
                if (editorID) {
                    std::string_view name(editorID);
                    // Combat V2-1e: note actual ATTACK actions (not Reload/
                    // Sighted) for NPCSync's damage-forward attribution gate.
                    if (name.find("Fire") != std::string_view::npos ||
                        name.find("Attack") != std::string_view::npos ||
                        name.find("Release") != std::string_view::npos ||
                        name.find("Melee") != std::string_view::npos ||
                        name.find("Throw") != std::string_view::npos ||
                        name.find("Grenade") != std::string_view::npos) {
                        s_lastLocalPlayerAttackMs.store(GetTickCount64(),
                                                        std::memory_order_relaxed);
                    }
                    if (name.find("Fire") != std::string_view::npos ||
                        name.find("Attack") != std::string_view::npos ||
                        name.find("Reload") != std::string_view::npos ||
                        name.find("Release") != std::string_view::npos ||
                        name.find("Sighted") != std::string_view::npos) {
                        // Send fire packet to other side
                        Network::FireData fire{};
                        fire.actionFormID = formID;
                        uint8_t fireBuf[16]{};
                        Network::PacketWriter fireWriter(Network::PKT_FIRE, fireBuf, sizeof(fireBuf));
                        fireWriter.Write(fire);
                        auto& net = Pulsar::Network::GetSingleton();
                        net.Send(fireWriter.Data(), fireWriter.Size(), true);
                    }
                }
            }
        }

        return s_origPerformAction(actionData);
    }

    static void InstallPerformActionHook()
    {
        // Log struct sizes once for network debugging
        static bool s_sizeLogged = false;
        if (!s_sizeLogged) {
            s_sizeLogged = true;
            PLOG_INFO("[PULSAR] sizeof(PositionData)={} sizeof(AnimVarSnapshot)={}"sv,
                sizeof(Network::PositionData), sizeof(Network::AnimVarSnapshot));
        }

        if (!s_mhInitialized) {
            MH_Initialize();
            s_mhInitialized = true;
        }
        if (s_origPerformAction) return;

        // Read PerformAction function pointer from DAT_145fb7bf0
        uintptr_t imageBase = REX::FModule::GetExecutingModule().GetBaseAddress();
        uintptr_t ptrAddr = imageBase + 0x5FAFE70;
        void* funcPtr = *reinterpret_cast<void**>(ptrAddr);

        if (!funcPtr) {
            PLOG_WARN("[PULSAR] DAT_145fb7bf0 is null - PerformAction not registered yet"sv);
            return;
        }

        PLOG_INFO("[PULSAR] PerformAction target at 0x{:X} (RVA 0x{:X})"sv,
            reinterpret_cast<uintptr_t>(funcPtr),
            reinterpret_cast<uintptr_t>(funcPtr) - imageBase);

        MH_STATUS status = MH_CreateHook(funcPtr, &HookedPerformAction,
            reinterpret_cast<LPVOID*>(&s_origPerformAction));

        if (status == MH_OK) {
            MH_EnableHook(funcPtr);
            PLOG_INFO("[PULSAR] PerformAction hook installed"sv);
        } else {
            PLOG_WARN("[PULSAR] PerformAction MinHook failed: {}"sv, static_cast<int>(status));
        }
    }

    // --- BSAnimationGraph::Update vtable hook (per-instance on clone) ---
    // SAF uses THookVFT on BSAnimationGraph vtable[4] = Update(BSAnimationUpdateData&).
    // BSAnimationUpdateData has delta-time at byte offset 0x60.
    // We hook the clone's graph to multiply delta-time → faster animation playback.

    using AnimGraphUpdateFn = void(__fastcall*)(void* thisPtr, void* updateData);
    static AnimGraphUpdateFn s_origAnimGraphUpdate = nullptr;
    static void** s_cloneGraphVtableCopy = nullptr;
    static void** s_origGraphVtable = nullptr;   // original class vtable - to restore on despawn
    static void*  s_cloneGraphChannelObj = nullptr;  // graph channel object - to restore vtable into
    static bool s_animHookInstalled = false;
    static constexpr int ANIM_GRAPH_UPDATE_VTIDX = 4;  // BSAnimationGraph::Update

    // Direct vtable call for setting int graph vars (SEH wrapper, no C++ objects on stack)
    static int ForceSetGraphIntDirectImpl(void* graphPtr, void* fnPtr, const void* namePtr, int32_t val)
    {
        __try {
            using SetIntFn = bool(__fastcall*)(void*, const void*, int32_t);
            reinterpret_cast<SetIntFn>(fnPtr)(graphPtr, namePtr, val);
            return 1;
        } __except(1) {
            return 0;
        }
    }

    static void ForceSetGraphIntDirect(void* graphPtr, void* fnPtr, const char* name, int32_t val)
    {
        RE::BSFixedString varName(name);
        ForceSetGraphIntDirectImpl(graphPtr, fnPtr, &varName, val);
    }

    // downed pose - set an int graph variable on ANY actor's active graph.
    // Used by CoopMortality to hold iSyncBleedout=1 on the LOCAL player while
    // downed (the engine life-state-8 path locks player movement input).
    bool SetActorGraphInt(RE::Actor* a_actor, const char* a_name, int32_t a_value)
    {
        if (!a_actor) return false;
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> gMgr;
        if (!SafeGetAnimGraphManager(a_actor, gMgr) || !gMgr || gMgr->graphs.size() == 0) {
            return false;
        }
        auto& g = gMgr->graphs[gMgr->activeGraph];
        if (!g) return false;
        auto** vt = *reinterpret_cast<void***>(g.get());
        ForceSetGraphIntDirect(g.get(), vt[0x20], a_name, a_value);
        return true;
    }

    // T122: bool graph-var writes. Starfield stores bool vars separately from
    // ints, so SetVariableInt (vt[0x20]) does NOT write bool vars - the graph
    // ignores the write. BGS convention places SetVariableBool at a neighbour
    // slot; vt[0x22] is the expected slot (0x20=int, 0x21=float, 0x22=bool).
    // We call both 0x22 and 0x1F as a fallback - only the real one lands,
    // the other is a harmless no-op on the wrong-typed variable.
    static int ForceSetGraphBoolDirectImpl(void* graphPtr, void* fnPtr, const void* namePtr, bool val)
    {
        __try {
            using SetBoolFn = bool(__fastcall*)(void*, const void*, bool);
            reinterpret_cast<SetBoolFn>(fnPtr)(graphPtr, namePtr, val);
            return 1;
        } __except(1) {
            return 0;
        }
    }

    static void ForceSetGraphBoolDirect(void* graphPtr, void** vt, const char* name, bool val)
    {
        RE::BSFixedString varName(name);
        // Try both candidate slots. Wrong-typed slot will either no-op on
        // nonexistent var or return false; __try catches any crash.
        ForceSetGraphBoolDirectImpl(graphPtr, vt[0x22], &varName, val);
        ForceSetGraphBoolDirectImpl(graphPtr, vt[0x1F], &varName, val);
    }

    // Unpack animFlags bitfield (matches packing in FillAnimationData)
    struct UnpackedAnimFlags {
        int32_t iSyncIdleLocomotion;   // bits 0-1
        int32_t iSyncSprintState;      // bits 2-3
        int32_t iSyncRelaxReadySneak;  // bits 4-6
        bool    isCompletelyStopped;   // bit 7
        bool    bPlayerMoveStartActive;// bit 8
        bool    weaponDrawn;           // bit 9
        bool    seated;                // bit 10 - host occupiedFurniture != 0
        bool    hostSighted;           // bit 11 - host is aiming (RMB held)
        bool    flashlight;            // bit 12 - helmet headlamp on 
    };

    static UnpackedAnimFlags UnpackAnimFlags(uint16_t flags)
    {
        return {
            static_cast<int32_t>(flags & 0x3),
            static_cast<int32_t>((flags >> 2) & 0x3),
            static_cast<int32_t>((flags >> 4) & 0x7),
            static_cast<bool>((flags >> 7) & 0x1),
            static_cast<bool>((flags >> 8) & 0x1),
            static_cast<bool>((flags >> 9) & 0x1),
            static_cast<bool>((flags >> 10) & 0x1),
            static_cast<bool>((flags >> 11) & 0x1),
            static_cast<bool>((flags >> 12) & 0x1)
        };
    }

    // SEH wrapper - graph channel object may be freed by engine refcount after anim graph re-init.
    // No C++ objects on stack (SEH constraint). Clears stale pointers on exception so DoDespawn
    // won't write to freed memory.
    static int SafeCallOrigAnimGraphUpdate(void* thisPtr, void* updateData)
    {
        __try {
            s_origAnimGraphUpdate(thisPtr, updateData);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s_cloneGraphChannelObj = nullptr;
            s_origGraphVtable     = nullptr;
            s_animHookInstalled   = false;
            return 0;
        }
    }

    static void __fastcall HookedAnimGraphUpdate(void* thisPtr, void* updateData)
    {
        auto& mgr = GetRemotePlayerManager();
        float netSpeed = mgr.GetNetSpeed();

        // dt-multiplier: only needed as fallback when DirectControl is NOT active
        // With DirectControl, the engine handles animation timing natively
        if (updateData) {
            float* deltaTimePtr = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(updateData) + 0x60);
            float origDt = *deltaTimePtr;

            if (!s_directCtrlEnabled && netSpeed > 0.5f) {
                float dtMult = (netSpeed > 6.0f) ? 3.0f : 2.0f;
                float newDt = origDt * dtMult;
                if (newDt > 0.05f) newDt = 0.05f;
                *deltaTimePtr = newDt;
            }

            static uint64_t s_animHookCount = 0;
            static uint64_t s_lastAnimLog = 0;
            s_animHookCount++;
            uint64_t now = GetTickCount64();
            if (now - s_lastAnimLog > 5000) {
                s_lastAnimLog = now;
                PLOG_INFO("[PULSAR] AnimHook: calls={} dt={:.4f} spd={:.1f}"sv,
                          s_animHookCount, origDt, netSpeed);
            }
        }

        SafeCallOrigAnimGraphUpdate(thisPtr, updateData);
    }

    // Install per-instance vtable hook on clone's BSAnimationGraph::Update
    static bool InstallAnimGraphHook(void* graphChannelObj)
    {
        if (s_animHookInstalled) return true;

        auto** originalVtable = *reinterpret_cast<void***>(graphChannelObj);
        if (!originalVtable) return false;

        // Read current vtable[4] - this is the Update function
        s_origAnimGraphUpdate = reinterpret_cast<AnimGraphUpdateFn>(originalVtable[ANIM_GRAPH_UPDATE_VTIDX]);
        s_origGraphVtable = originalVtable;      // save original vtable ptr (for restore on despawn)
        s_cloneGraphChannelObj = graphChannelObj;

        PLOG_INFO("[PULSAR] AnimHook: original Update at vtable[{}] = 0x{:X}"sv,
                  ANIM_GRAPH_UPDATE_VTIDX, reinterpret_cast<uintptr_t>(s_origAnimGraphUpdate));

        // Create vtable copy (enough entries to cover all 81+ methods)
        constexpr int VTABLE_COPY_SIZE = 96;
        s_cloneGraphVtableCopy = new void*[VTABLE_COPY_SIZE];
        for (int i = 0; i < VTABLE_COPY_SIZE; i++) {
            __try {
                s_cloneGraphVtableCopy[i] = originalVtable[i];
            } __except(1) {
                s_cloneGraphVtableCopy[i] = nullptr;
            }
        }

        // Patch our Update hook
        s_cloneGraphVtableCopy[ANIM_GRAPH_UPDATE_VTIDX] = reinterpret_cast<void*>(&HookedAnimGraphUpdate);

        // Swap vtable pointer on the clone's graph channel
        *reinterpret_cast<void***>(graphChannelObj) = s_cloneGraphVtableCopy;

        s_animHookInstalled = true;
        PLOG_INFO("[PULSAR] AnimHook: INSTALLED on clone graph channel"sv);
        return true;
    }

    // === DoPostAnimationChannelUpdate vtable hook ===
    // IPostAnimationChannelUpdateFunctor is at TESObjectREFR+0x40
    // vtable[0] = destructor, vtable[1] = DoPostAnimationChannelUpdate
    using PostAnimUpdateFn = void(__fastcall*)(void* thisPtr);
    static PostAnimUpdateFn s_origPostAnimUpdate = nullptr;
    static void** s_postAnimVtableCopy = nullptr;
    static void** s_origPostAnimVtable = nullptr;  // original vtable ptr - to restore on despawn
    static void*  s_postAnimFunctorPtr = nullptr;  // functor object - to restore vtable into
    static bool s_postAnimHookInstalled = false;

    static void __fastcall HookedPostAnimChannelUpdate(void* thisPtr)
    {
        // Call original first
        if (s_origPostAnimUpdate) s_origPostAnimUpdate(thisPtr);

        // thisPtr = IPostAnimationChannelUpdateFunctor* = actor + 0x40
        RE::Actor* cloneActor = s_clonePitchActor;
        if (!cloneActor) return;

        // Verify thisPtr belongs to our clone
        void* expectedThis = reinterpret_cast<uint8_t*>(cloneActor) + 0x40;
        if (thisPtr != expectedThis) return;

        // Lazy-resolve bone cache
        if (!s_clonePitchBoneCache.resolved) {
            RE::NiNode* root = nullptr;
            __try {
                auto* ld = *reinterpret_cast<RE::LOADED_REF_DATA**>(
                    reinterpret_cast<uint8_t*>(cloneActor) + 0xB8);
                if (ld && ld->data3D) root = ld->data3D->GetAsNiNode();
            } __except(1) { root = nullptr; }
            if (root) {
                s_clonePitchBoneCache.Resolve(root);
                PLOG_INFO("[PULSAR] PostAnimHook: cache resolved, C_Head={}"sv,
                          (void*)s_clonePitchBoneCache.bones[6]);
            }
        }

        // Sinus head nod test DISABLED - was conflicting with DirectAt
        // PostAnimHook kept for future use (e.g. post-DirectAt yaw correction)
    }

    static bool InstallPostAnimHook(RE::Actor* actor)
    {
        if (s_postAnimHookInstalled) return true;

        // IPostAnimationChannelUpdateFunctor vtable is at actor+0x40
        void* functorPtr = reinterpret_cast<uint8_t*>(actor) + 0x40;
        auto** originalVtable = *reinterpret_cast<void***>(functorPtr);
        if (!originalVtable) return false;

        s_origPostAnimUpdate = reinterpret_cast<PostAnimUpdateFn>(originalVtable[1]);
        s_origPostAnimVtable = originalVtable;  // save for restore on despawn
        s_postAnimFunctorPtr = functorPtr;

        PLOG_INFO("[PULSAR] PostAnimHook: original vtable[1] = 0x{:X}"sv,
                  reinterpret_cast<uintptr_t>(s_origPostAnimUpdate));

        // Create vtable copy (small - only needs ~4 entries)
        constexpr int VTABLE_SIZE = 8;
        s_postAnimVtableCopy = new void*[VTABLE_SIZE];
        for (int i = 0; i < VTABLE_SIZE; i++) {
            __try {
                s_postAnimVtableCopy[i] = originalVtable[i];
            } __except(1) {
                s_postAnimVtableCopy[i] = nullptr;
            }
        }

        // Patch vtable[1] with our hook
        s_postAnimVtableCopy[1] = reinterpret_cast<void*>(&HookedPostAnimChannelUpdate);

        // Swap vtable pointer on the clone's functor interface
        *reinterpret_cast<void***>(functorPtr) = s_postAnimVtableCopy;

        s_postAnimHookInstalled = true;
        PLOG_INFO("[PULSAR] PostAnimHook: INSTALLED on clone actor"sv);
        return true;
    }

    // --- SEH wrappers (plain C, no C++ destructors on stack) ---

    static int ForceSetGraphFloatImpl(void* graphPtr, void* fnPtr, const void* namePtr, float val)
    {
        __try {
            using SetVarFn = bool(*)(void*, const void*, float);
            reinterpret_cast<SetVarFn>(fnPtr)(graphPtr, namePtr, val);
            return 1;
        } __except(1) {
            return 0;
        }
    }

    static void ForceSetGraphFloat(RE::Actor* a_actor, const char* a_name, float a_val)
    {
        if (!a_actor) return;

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphMgr;
        if (!SafeGetAnimGraphManager(a_actor, graphMgr) || !graphMgr) return;
        if (graphMgr->graphs.size() == 0) return;

        auto& graph = graphMgr->graphs[graphMgr->activeGraph];
        if (!graph) return;

        auto** vtable = *reinterpret_cast<void***>(graph.get());
        RE::BSFixedString varName(a_name);
        ForceSetGraphFloatImpl(graph.get(), vtable[0x21], &varName, a_val);
    }


    // --- Bone pitch test (SEH-safe, no C++ objects) ---
    static int SafeResolveBoneCache(RE::Actor* actor)
    {
        __try {
            auto* ld = *reinterpret_cast<RE::LOADED_REF_DATA**>(
                reinterpret_cast<uint8_t*>(actor) + 0xB8);
            if (!ld || !ld->data3D) return 0;
            auto* root = ld->data3D->GetAsNiNode();
            if (!root) return 0;
            s_clonePitchBoneCache.Resolve(root);
            return 1;
        } __except(1) { return 0; }
    }

    static int SafeApplyBoneRotation(RE::NiAVObject* bone, float cosA, float sinA)
    {
        __try {
            RE::NiMatrix3 pitchRot;
            pitchRot.MakeIdentity();
            pitchRot[1][1] = cosA;  pitchRot[1][2] = -sinA;
            pitchRot[2][1] = sinA;  pitchRot[2][2] = cosA;
            // Write ONLY world transform - animation overwrites local, but
            // maybe renderer reads world independently
            bone->world.rotate = bone->world.rotate * pitchRot;
            return 1;
        } __except(1) { return 0; }
    }

    // Resolve ExtraDirectAtTarget vtable once (outside SEH)
    static void* GetExtraDirectAtTargetVtable()
    {
        static void* s_vtable = nullptr;
        if (!s_vtable) {
            static REL::Relocation<void*> vtbl{ RE::VTABLE::ExtraDirectAtTarget[0] };
            s_vtable = vtbl.get();
        }
        return s_vtable;
    }

    // Raw SEH wrapper - set or update ExtraDirectAtTarget
    static int SafeSetDirectAtTargetRaw(RE::Actor* actor, RE::TESObjectREFR* target, void* vtable)
    {
        __try {
            auto* edl = actor->extraDataList.get();
            if (!edl) return 0;

            // Check if ExtraDirectAtTarget already exists
            auto* existing = edl->GetByType(RE::ExtraDataType::kDirectAtTarget);
            if (existing) {
                // Overwrite target pointer at BSExtraData+0x18
                *reinterpret_cast<RE::TESObjectREFR**>(
                    reinterpret_cast<uint8_t*>(existing) + 0x18) = target;
                return 2;  // updated existing
            }

            // Create new ExtraDirectAtTarget: BSExtraData(0x18) + target*(0x08) = 0x20
            auto* extra = static_cast<RE::BSExtraData*>(calloc(1, 0x20));
            if (!extra) return 0;

            *reinterpret_cast<void**>(extra) = vtable;
            extra->type = RE::ExtraDataType::kDirectAtTarget;
            extra->next = nullptr;
            extra->flags = 0;
            *reinterpret_cast<RE::TESObjectREFR**>(
                reinterpret_cast<uint8_t*>(extra) + 0x18) = target;

            edl->AddExtra(extra);
            return 1;  // created new
        } __except(1) { return 0; }
    }

    static int SafeSetDirectAtTarget(RE::Actor* actor, RE::TESObjectREFR* target)
    {
        void* vt = GetExtraDirectAtTargetVtable();
        if (!vt) return 0;
        return SafeSetDirectAtTargetRaw(actor, target, vt);
    }

    // Resolve Health ActorValueInfo once - BSFixedString has a C++ dtor and
    // cannot live inside an __try (C2712), so the lookup sits outside the
    // SEH wrapper below.
    static RE::ActorValueInfo* GetHealthAV()
    {
        static RE::ActorValueInfo* s_healthAV = nullptr;
        if (!s_healthAV) {
            s_healthAV = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                RE::BSFixedString("Health"));
        }
        return s_healthAV;
    }

    // Plain-C: no C++ objects with dtors inside __try.
    static int CloneInvincibilityTick(RE::Actor* clone, RE::ActorValueInfo* healthAV)
    {
        __try {
            if (!clone) return 0;
            clone->boolFlags.set(
                RE::Actor::BOOL_FLAGS::kAttackingDisabled,
                RE::Actor::BOOL_FLAGS::kEssential,
                RE::Actor::BOOL_FLAGS::kProtected);
            if (clone->IsDead()) {
                clone->Resurrect(false, true);
            }
            if (healthAV) {
                float mx = clone->GetPermanentActorValue(*healthAV);
                float cur = clone->GetActorValue(*healthAV);
                if (mx > 0 && cur < mx) {
                    clone->SetActorValue(*healthAV, mx);
                }
            }
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // SEH wrapper for TESObjectREFR::SetPosition (vtable[0x89])
    static int SafeRefSetPosition(RE::TESObjectREFR* ref, float x, float y, float z)
    {
        __try {
            using SetPosFn = void(__fastcall*)(void*, const RE::NiPoint3&, bool);
            auto** vtable = *reinterpret_cast<void***>(ref);
            auto setPos = reinterpret_cast<SetPosFn>(vtable[0x89]);
            RE::NiPoint3 pos(x, y, z);
            setPos(ref, pos, false);
            return 1;
        } __except(1) { return 0; }
    }

    // --- Public API (network thread) ---

    void RemotePlayerManager::RequestSpawn()
    {
        m_pendingSpawn = true;
    }

    void RemotePlayerManager::RequestDespawn()
    {
        m_pendingDespawn = true;
    }

    void RemotePlayerManager::RequestForceRespawn(std::uint32_t spawnDelayMs)
    {
        m_forceRespawnDelayMs.store(spawnDelayMs);
        m_pendingForceRespawn = true;
    }

    // (2026-06-24) - smooth the proc-cell clone flicker AT THE ROOT.
    // Insight (user): the clone's 3D survives across MANY cells on its own - the
    // engine keeps it in the loaded cell-grid; it only goes invisible once its
    // home cell actually UNLOADS (data3D detaches from the render scene-graph).
    // The previous "respawn on every proc-cell SETTLE" was premature: it tore down
    // and rebuilt the clone (2s gone-gap) even while it was still perfectly visible
    // → the flicker. So: do NOTHING while the clone is visible; act ONLY when it is
    // genuinely orphaned. Visible = data3D present AND parented in a scene-graph
    // (data3D->parent != null). Orphaned = data3D present but parent==null → MoveTo
    // (RVA 0x14191EFB0, cell-streamer-aware) re-attaches the EXISTING 3D into the
    // player's cell, no despawn. Dead = no data3D → caller respawns. Position is
    // already host-driven every frame by DoSetPosition, so a reparent never needs a
    // rebuild. GAME THREAD ONLY (MoveTo posts to game-thread cell-streamer queues).
    RemotePlayerManager::CloneReparentResult RemotePlayerManager::ReparentCloneIfOrphaned()
    {
        RE::Actor* clone = m_remoteActor;
        if (!clone) return CloneReparentResult::kDead;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return CloneReparentResult::kVisible;  // can't act safely → leave as-is

        // 0 = no 3D (dead), 1 = 3D parented (visible), 2 = 3D present but orphaned.
        int state = 0;
        __try {
            auto* ld = *reinterpret_cast<RE::LOADED_REF_DATA**>(
                reinterpret_cast<std::uint8_t*>(clone) + 0xB8);
            if (ld && ld->data3D)
                state = (ld->data3D->parent != nullptr) ? 1 : 2;
        } __except (EXCEPTION_EXECUTE_HANDLER) { state = 0; }

        if (state == 1) return CloneReparentResult::kVisible;   // still visible - let it survive across cells
        if (state == 0) return CloneReparentResult::kDead;      // genuinely gone → caller respawns

        // state == 2: orphaned 3D → re-attach via MoveTo into the player's cell.
        using EngineMoveTo_t = bool (__fastcall*)(RE::TESObjectREFR*, RE::TESObjectREFR*);
        auto fn = reinterpret_cast<EngineMoveTo_t>(REL::Offset{ 0x191EFB0 }.address());
        if (!fn) return CloneReparentResult::kDead;             // no MoveTo → respawn fallback
        bool ok = false;
        __try { ok = fn(clone, player); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (!ok) return CloneReparentResult::kDead;

        // Re-arm the graph gate: re-attach can re-init the anim graph; gating
        // skips ONLY DoSetPosition's graph block for 3s (position keeps tracking),
        // avoiding the pure-virtual graph crash (not SEH-catchable).
        m_cloneSpawnedAtMs.store(::GetTickCount64(), std::memory_order_relaxed);
        // No-pop: MoveTo placed the clone AT the player; SetPosition it to the host's
        // real coords in this SAME tick (pre-render) so it never flashes in the
        // player's face. In-cell move now (safe case); SEH-wrapped.
        if (m_hasPosition)
            SafeRefSetPosition(m_remoteActor,
                               m_targetPosData.x, m_targetPosData.y, m_targetPosData.z);
        return CloneReparentResult::kReparented;
    }

    void RemotePlayerManager::RequestSetPosition(float x, float y, float z, float rotX, float rotY, float rotZ,
                                                  float speed, float direction,
                                                  float lookHeading, uint16_t animFlags,
                                                  const Network::AnimVarSnapshot& vars)
    {
        std::lock_guard lock(m_posMutex);
        m_pendingPosData = { x, y, z, rotX, rotY, rotZ, speed, direction, lookHeading, animFlags, vars };
        m_pendingPosition = true;
    }

    void RemotePlayerManager::RequestEquipWeapon(uint32_t formID)
    {
        m_pendingWeaponFormID = formID;
        m_pendingEquip = true;
    }

    void RemotePlayerManager::RequestEquipArmor(const uint32_t* formIDs, uint8_t count)
    {
        if (count > 16) count = 16;
        m_pendingArmorCount = count;
        std::memcpy(m_pendingArmorIDs, formIDs, count * sizeof(uint32_t));
        m_pendingArmor = true;
    }

    // F.2-v2: full worn set incl. mods. Routes through the existing pending-
    // equip machinery (DoEquipWeapon/DoEquipArmor) so weapon-draw handling,
    // graph kickstart and dedup keep working; the mods land in the aligned
    // m_weaponMods/m_armorMods slots and get applied before the equip call.
    void RemotePlayerManager::RequestEquipV2(std::vector<EquipV2Entry>&& a_entries)
    {
        uint32_t weaponID = 0;
        uint32_t armorIDs[16]{};
        uint8_t  armorCount = 0;
        {
            std::lock_guard lk(m_equipModsMutex);
            m_weaponMods.clear();
            m_weaponModSentinel = 0xFFFF;
            for (auto& e : a_entries) {
                if (e.isWeapon && weaponID == 0) {
                    weaponID            = e.formID;
                    m_weaponMods        = std::move(e.mods);
                    m_weaponModSentinel = e.modSentinel;
                } else if (!e.isWeapon && armorCount < 16) {
                    armorIDs[armorCount]            = e.formID;
                    m_armorMods[armorCount]         = std::move(e.mods);
                    m_armorModSentinels[armorCount] = e.modSentinel;
                    ++armorCount;
                }
            }
        }
        m_pendingWeaponFormID = weaponID;
        m_pendingEquip = true;
        m_pendingArmorCount = armorCount;
        std::memcpy(m_pendingArmorIDs, armorIDs, sizeof(armorIDs));
        m_pendingArmor = true;
    }

    void RemotePlayerManager::RequestFire(uint32_t actionFormID)
    {
        // V2-1f: queue instead of single slot - preserves every action and its
        // order (Fire/Release/Reload) for a lossless 1:1 replay on the clone.
        m_lastFireAction = actionFormID;
        std::lock_guard lk(m_fireQueueMutex);
        if (m_fireQueue.size() < 64) m_fireQueue.push_back(actionFormID);
    }

    void RemotePlayerManager::RequestSetBones(const Network::BoneData& data)
    {
        std::lock_guard lock(m_boneMutex);
        static_assert(sizeof(m_pendingBoneFrame.rotations) == sizeof(data.rotations));
        std::memcpy(&m_pendingBoneFrame.rotations, data.rotations, sizeof(data.rotations));
        m_pendingBoneFrame.hipsPosX = data.hipsPos[0];
        m_pendingBoneFrame.hipsPosY = data.hipsPos[1];
        m_pendingBoneFrame.hipsPosZ = data.hipsPos[2];
        m_pendingBones = true;
    }

    // --- Game thread processing (~1000x/sec) ---

    void RemotePlayerManager::FindLookMarker()
    {
        if (!m_globLookHi || !m_globLookLo) return;
        float hi = m_globLookHi->value;
        float lo = m_globLookLo->value;
        if (hi == 0.0f && lo == 0.0f) return;

        uint32_t formID = static_cast<uint32_t>(
            static_cast<int32_t>(hi) * 65536 + static_cast<int32_t>(lo));
        auto* form = RE::TESForm::LookupByID(formID);
        if (form) {
            m_lookMarker = form->As<RE::TESObjectREFR>();
            if (m_lookMarker) {
                PLOG_INFO("[PULSAR] LookMarker resolved: 0x{:08X}"sv, formID);
            }
        }
    }

    void RemotePlayerManager::ProcessPending()
    {
        if (!m_globsResolved) {
            ResolveGlobs();
        }

        // Skip all animation-graph ops when (a) clone is C++-spawned (CloneManager
        // proof-of-concept) OR (b) clone is in a procedural cell (0xFF-prefix
        // cellFormID - planet surface). Verified 2026-04-19: identical graph ops
        // that run for 2+ min in static cells crash ~30s in procedural cells.
        // Graph ops themselves are not the root cause - they're a trigger specific
        // to the procedural-cell environment. Skipping them here is a MVP
        // workaround until the underlying proc-cell issue is understood.
        bool skipCloneGraphOps = false;
        if (m_remoteActor) {
            if (Pulsar::CloneManager::IsCppSpawnedClone(m_remoteActor->formID)) {
                skipCloneGraphOps = true;
            }
            // Proc-cell graph-op skip REMOVED 2026-04-19. The prior crash we
            // blamed on graph ops was actually NPCSync::SetPosition on Sarah;
            // with NPCSync now filtering proc cells, clone animation ops may
            // be safe. Testing this hypothesis.
        }
        // 2026-06-11: ALSO skip clone graph ops during a transition. The
        // both-down DOUBLE reload tears down both clones at once; the per-tick
        // clone anim-sync (ForceSetGraphInt vtable calls) then pure-virtual-
        // crashes on the mid-rebuild graph (NOT SEH-catchable; client crash
        // 22:35, frame +0x377E90). IsQuiet() covers loadMenu + grace; the
        // clone-settle window covers the post-load graph rebuild.
        if (Pulsar::Cell3DAttach::IsQuiet()) {
            skipCloneGraphOps = true;
        } else {
            const uint64_t spawnedAt = m_cloneSpawnedAtMs.load(std::memory_order_relaxed);
            if (spawnedAt != 0 && (GetTickCount64() - spawnedAt) < 3000) {
                skipCloneGraphOps = true;
            }
        }

        if (m_pendingDespawn.exchange(false)) {
            DoDespawn();
        }

        if (m_pendingSpawn.exchange(false)) {
            DoSpawn();
        }

        // Clone-invincibility watchdog. kEssential alone doesn't prevent the
        // clone from dropping to bleedout / ragdolling from collision damage
        // (observed 2026-04-22: Moon-Jumper FURN collision killed the clone
        // on client, corpse then jittered along incoming PKT_POSITION). Each
        // tick: re-assert flags, resurrect if dead, top up health.
        if (m_remoteActor) {
            static uint64_t s_lastWatchdog = 0;
            uint64_t nowW = GetTickCount64();
            if (nowW - s_lastWatchdog > 500) {
                s_lastWatchdog = nowW;
                CloneInvincibilityTick(m_remoteActor, GetHealthAV());
            }
        }

        if (m_pendingPosition.exchange(false)) {
            std::lock_guard lock(m_posMutex);
            m_targetPosData = m_pendingPosData;
            m_hasPosition = true;
        }

        // Accept new bone data from network
        if (m_pendingBones.exchange(false)) {
            std::lock_guard lock(m_boneMutex);
            m_targetBoneFrame = m_pendingBoneFrame;
            m_hasBones = true;
        }

        // Equip ops: pending flag consumption must be atomic or ENet packets
        // arriving between .load() and the false-store would be dropped. Use
        // exchange, and only consume when we can actually run the op (actor
        // present, graph ops allowed) - otherwise keep the flag pending.
        //
        // F.2-v2 crash fix (2026-06-11): ALSO gate on Cell3DAttach::IsQuiet().
        // During a load (both-down game-over reload), m_remoteActor is a STALE
        // freed pointer (not null - the despawn hasn't run yet), so the null
        // check passes and DoEquipWeapon's AddObjectToContainer faulted on the
        // garbage vtable (host crash 17:56, fault in AddObjectToContainer
        // vt[0x9A]=+0x4D0 on rdx=0x7F7F... garbage). IsQuiet() is false while
        // the loadMenu is open + grace, so the flag stays pending and the
        // equip applies AFTER the post-reload respawn gives a fresh clone.
        // IsQuiet() is TRUE while a transition (loadMenu+grace) is in flight -
        // that's exactly when applies must be DROPPED. ALSO require the clone to
        // be fully 3D-attached AND to have settled for >3s since its last spawn:
        // a freshly-respawned clone (both-down reload) has a graph still being
        // rebuilt, and the equip apply's graph ops pure-virtual-crash on it
        // (NOT SEH-catchable; both crashed 21:54-22:04). The pending flag
        // survives until all gates pass, so equip applies once the clone is
        // genuinely stable.
        const uint64_t spawnedAt = m_cloneSpawnedAtMs.load(std::memory_order_relaxed);
        const bool cloneSettled = spawnedAt != 0
                                  && (GetTickCount64() - spawnedAt) > 3000;
        // (2026-06-20): don't equip-apply on a DOWNED clone - DoEquipArmor/
        // Weapon's 3D/graph update would disturb the bleedout/knee pose. (The MAIN
        // stand-up cause was the per-tick weapon-DRAW, handled separately by forcing
        // the clone holstered while downed - see the weapon-draw block below. This
        // gate is the equip-apply half of the same protection. Pending flag survives
        // → equip applies after revive.)
        const bool equipSafe = !Pulsar::Cell3DAttach::IsQuiet()
                               && SafeCloneHas3D(m_remoteActor)
                               && cloneSettled
                               && !Pulsar::Coop::Mortality::IsRemoteDowned();
        // (2026-06-19): WEAPON equip-apply stays OFF (DoEquipWeapon mutates the
        // clone anim graph → both-down pure-virtual crash). Still consume the pending
        // flag so it doesn't pile up; just skip the apply when kSyncWeapons is false.
        if (m_remoteActor && !skipCloneGraphOps && equipSafe
            && m_pendingEquip.exchange(false)) {
            if (EquipSync::SyncWeapons()) DoEquipWeapon();
        }
        // ARMOR equip-apply ON  - biped-slot visuals, graph-safe.
        if (EquipSync::kSyncArmor && m_remoteActor && !skipCloneGraphOps && equipSafe
            && m_pendingArmor.exchange(false)) {
            DoEquipArmor();
        }

        // Graph kickstart: auto draw/holster cycle after initial spawn/equip.
        // Fires the non-instant ActionInitializeGraphToBaseState so jump
        // transitions work from the first jump.
        if (m_remoteActor && !skipCloneGraphOps && !m_graphKickstartDone) {
            uint64_t kNow = GetTickCount64();
            if (m_graphKickstartDrawAt > 0 && kNow >= m_graphKickstartDrawAt) {
                m_graphKickstartDrawAt = 0;
                g_forceAnimation = true;
                m_remoteActor->DrawWeaponMagicHands(true);
                s_forceAnimUntil = kNow + 300;
                // 2026-04-20: bumped 500→2000ms. Log showed that 500ms was
                // too short - engine didn't register drawn-state before our
                // holster, so the holster became a non-transition and the
                // engine never fired ActionInitializeGraphToBaseState.
                // User's manual draw+holster that fixed it had ~2.3s gap.
                m_graphKickstartHolsterAt = kNow + 2000;
                PLOG_INFO("[PULSAR] GraphKickstart: draw fired"sv);
            }
            if (m_graphKickstartHolsterAt > 0 && kNow >= m_graphKickstartHolsterAt) {
                m_graphKickstartHolsterAt = 0;
                g_forceAnimation = true;
                m_remoteActor->DrawWeaponMagicHands(false);
                s_forceAnimUntil = kNow + 300;
                m_graphKickstartDone = true;
                PLOG_INFO("[PULSAR] GraphKickstart: holster fired (graph init complete)"sv);
            }
        }

        // Deferred weapon re-draw after a swap - 2-stage. The new weapon equips
        // HOLSTERED and the equip needs ~800ms to settle; the normal per-tick draw
        // is suppressed while a redraw is pending (an early draw poisons the graph's
        // "weapon out" intent → weapon stuck on the back). Stage 1 forces a holster
        // (unconditional - actorState2 can't be trusted on a clone), stage 2 draws a
        // few ticks later → a real holster->draw transition pulls it into the hand.
        if (m_pendingRedraw > 0 && GetTickCount64() >= m_pendingRedraw && !skipCloneGraphOps) {
            uint64_t now = GetTickCount64();
            m_pendingRedraw = 0;
            if (m_remoteActor && !Pulsar::Coop::Mortality::IsRemoteDowned()) {
                g_forceAnimation = true;
                m_remoteActor->DrawWeaponMagicHands(false);  // stage 1: holster (reset graph)
                s_forceAnimUntil = now + 500;
                m_pendingRedrawDrawAt = now + 250;           // stage 2 scheduled
                m_weaponDrawn = false;
                m_weaponDrawTarget = false;
            }
        }
        if (m_pendingRedrawDrawAt > 0 && GetTickCount64() >= m_pendingRedrawDrawAt && !skipCloneGraphOps) {
            uint64_t now = GetTickCount64();
            m_pendingRedrawDrawAt = 0;
            if (m_remoteActor && !Pulsar::Coop::Mortality::IsRemoteDowned()) {
                // force state to SHEATHED first so the draw isn't a no-op
                // (DrawWeaponMagicHands(true) early-returns if state byte already drawn).
                SafeForceWeaponSheathedState(m_remoteActor);
                g_forceAnimation = true;
                m_remoteActor->DrawWeaponMagicHands(true);   // stage 2: real 0->drawn transition
                s_forceAnimUntil = now + 500;
                m_weaponDrawn = true;
                m_weaponDrawTarget = true;
                PLOG_INFO("[PULSAR] Deferred weapon re-draw complete (holster->draw) weapon=0x{:08X}"sv,
                          m_currentWeaponFormID);
            }
        }

        // Fire sync - forward combat actions directly via PerformAction.
        // 2026-04-20: proc-cell gate REMOVED. The original 2026-04-19 crash
        // that motivated the gate was likely the same parentCell-UAF that
        // T22 diagnosed and our parentCell-pin now fixes. If it still crashes
        // in proc cells we'll need a narrower exclusion (specific action IDs)
        // or a different approach (anim-only, manual projectile spawn).
        // SafeCallPerformAction SEH wrapper is the last line of defense.
        const bool canFire = !skipCloneGraphOps && m_remoteActor && s_origPerformAction;
        if (canFire) {
            // V2-1f (2026-06-11): drain ALL queued fire actions in arrival order.
            // The old single-slot pending (m_pendingFireAction overwrite) LOST
            // actions whenever several PKT_FIRE arrived between game-thread
            // drains - ENet pumps events in batches, so auto-fire weapons
            // dropped shots / shifted reloads and the clone's visual fire ran
            // out of sync with the sender ("länger geschossen / später
            // nachgeladen"). With the correct weapon+mods from F.2-v2 AND a
            // lossless 1:1 action replay, timing now mirrors the sender.
            std::vector<uint32_t> fireWork;
            {
                std::lock_guard lk(m_fireQueueMutex);
                if (!m_fireQueue.empty()) fireWork.swap(m_fireQueue);
            }
            for (uint32_t fireFormID : fireWork) {
                auto* actionForm = RE::TESForm::LookupByID(fireFormID);
                if (!actionForm) continue;

                static REL::Relocation<void*> s_bgaVtable{ REL::ID(393260) };
                static REL::Relocation<void*> s_aoVtable{ REL::ID(393264) };

                alignas(16) uint8_t actionData[0x68]{};
                *reinterpret_cast<void**>(actionData + 0x00) = s_bgaVtable.get();
                *reinterpret_cast<RE::Actor**>(actionData + 0x08) = m_remoteActor;
                *reinterpret_cast<RE::TESForm**>(actionData + 0x18) = actionForm;
                *reinterpret_cast<void**>(actionData + 0x28) = s_aoVtable.get();

                // Skip ActionSightedRelease - it breaks the aimed pose
                // iSyncSighted graph var controls sighted state instead
                if (fireFormID == 0x00004A58) {  // ActionSightedRelease
                    continue;
                }

                // Keep g_forceAnimation open for fire actions, clear for release
                bool isRelease = (fireFormID == 0x00013454);  // ActionRightRelease
                if (!isRelease) {
                    g_forceAnimation = true;
                    // Set aimed pose BEFORE firing so first projectile aims forward
                    if ((fireFormID == 0x00004A5C || fireFormID == 0x00004A5A) && !skipCloneGraphOps) {
                        RE::BSTSmartPointer<RE::BSAnimationGraphManager> fgMgr;
                        if (SafeGetAnimGraphManager(m_remoteActor, fgMgr) && fgMgr &&
                            fgMgr->graphs.size() > 0) {
                            auto& fg = fgMgr->graphs[fgMgr->activeGraph];
                            if (fg) {
                                auto** fvt = *reinterpret_cast<void***>(fg.get());
                                ForceSetGraphIntDirect(fg.get(), fvt[0x20], "iSyncSighted", 1);
                            }
                        }
                    }
                }
                SafeCallPerformAction(s_origPerformAction, actionData);
                if (isRelease) {
                    g_forceAnimation = false;
                }

                PLOG_INFO("[PULSAR] Clone ACTION: 0x{:08X} '{}'"sv, fireFormID,
                    isRelease ? "RELEASE" : "FIRE");
            }
        }

        if (m_hasPosition && m_globsResolved) {
            DoSetPosition();
        }

        // Layer 1: keep the player-clone out of autonomous engine combat (it's
        // now weapon-synced; its AI was machine-gunning NPCs in proc cells). Belt to
        // the AI-eval block above - also covers reactive combat and the block being
        // toggled off. Not gated on skipCloneGraphOps (StopCombat is not a graph op).
        if (m_remoteActor) {
            NeutralizeCloneCombatSafe(m_remoteActor);
        }

        // g_forceAnimUntil is namespace-level (see top of namespace)
        {
            uint64_t now = GetTickCount64();
            if (s_forceAnimUntil > 0 && now >= s_forceAnimUntil) {
                g_forceAnimation = false;
                s_forceAnimUntil = 0;
            }
        }

        // Weapon draw/sheathe - PerformAction hook blocks ALL actions on clone
        // g_forceAnimation bypasses the block for our forced calls (Skyrim Together pattern)
        // Debounce: wait 500ms for stable state before acting (prevents rapid toggle)
        // Defer: don't draw until weapon is actually equipped on the clone
        if (m_remoteActor && m_hasPosition && !skipCloneGraphOps) {
            bool wantDrawn = UnpackAnimFlags(m_targetPosData.animFlags).weaponDrawn;

            // Don't attempt draw if no weapon is equipped yet
            if (wantDrawn && m_currentWeaponFormID == 0) {
                wantDrawn = false;
            }

            ///(2026-06-20): while the clone is DOWNED, force it holstered.
            // The per-tick weapon-DRAW (DrawWeaponMagicHands(true)) puts the clone in
            // a combat-ready stance that stands it straight back out of the bleedout/
            // knee pose (root cause of "downed clone keeps standing up when equip sync
            // is on"; with sync off there's no weapon to draw, so it held). Forcing
            // wantDrawn=false makes the clone holster - which is ALSO the graph kick
            // that PLAYS the bleedout anim (matches: holstering triggered it in-game).
            if (Pulsar::Coop::Mortality::IsRemoteDowned()) {
                wantDrawn = false;
            }

            // Update g_wantWeaponDrawn so PerformAction hook can block conflicting actions
            g_wantWeaponDrawn = wantDrawn;

            // Draw/holster is EDGE-triggered off our own intent (m_weaponDrawn).
            // NOTE: actorState2&0x3 is NOT a reliable "drawn" indicator on a clone -
            // it stays 0 even after a successful DrawWeaponMagicHands (verified
            // 2026-06-20: a verify-loop polling it never converged and spammed the
            // draw every 600ms). So we never poll the engine; we drive intent.
            uint64_t now = GetTickCount64();

            // Weapon-swap settle window: the 2-stage deferred re-draw owns the draw
            // state. Don't fire a normal draw while a redraw is pending - an early
            // draw before the new weapon's equip settles gets swallowed and leaves it
            // stuck holstered on the back.
            if (m_pendingRedraw == 0 && m_pendingRedrawDrawAt == 0) {
                if (wantDrawn != m_weaponDrawTarget) {
                    m_weaponDrawTarget = wantDrawn;
                    m_weaponDrawChangeTime = now;
                }
                if (m_weaponDrawn != m_weaponDrawTarget && now - m_weaponDrawChangeTime >= 50) {
                    m_weaponDrawn = m_weaponDrawTarget;
                    g_forceAnimation = true;
                    m_remoteActor->DrawWeaponMagicHands(m_weaponDrawn);
                    s_forceAnimUntil = now + 500;
                    PLOG_INFO("[PULSAR] Forced DrawWeaponMagicHands({})"sv, m_weaponDrawn);
                }
            }
        }

        // Seated sync - bit 10 of animFlags. When host occupies furniture,
        // make the local clone sit in the nearest FURN-REFR via Bethesda's
        // native activation path. VehicleSync keeps the FURN position
        // following the clone so engine-seat-anchoring carries the clone
        // visually along with the vehicle.
        if (m_remoteActor && m_hasPosition && !skipCloneGraphOps) {
            bool wantSeated = UnpackAnimFlags(m_targetPosData.animFlags).seated;
            Pulsar::VehicleSync::OnRemoteSeatedUpdate(
                m_remoteActor, wantSeated,
                m_targetPosData.x, m_targetPosData.y, m_targetPosData.z);
        }

        // flashlight - mirror the peer's helmet headlamp onto the clone by
        // driving the engine headlamp setup (creates a real BSShadowLight at
        // clone+0xEB0). ApplyToClone no-ops when already in the desired state.
        if (m_remoteActor && m_hasPosition && !skipCloneGraphOps) {
            Pulsar::FlashlightSync::ApplyToClone(
                m_remoteActor, UnpackAnimFlags(m_targetPosData.animFlags).flashlight);
        }

        // Sneak sync - force crouch/stand based on sender state
        if (m_remoteActor && m_hasPosition && !skipCloneGraphOps) {
            bool wantSneak = (UnpackAnimFlags(m_targetPosData.animFlags).iSyncRelaxReadySneak == 2);
            static bool s_cloneSneaking = false;

            if (wantSneak != s_cloneSneaking) {
                s_cloneSneaking = wantSneak;
                RE::BSFixedString eventName(wantSneak ? "SneakStart" : "SneakStop");
                g_forceAnimation = true;
                SafeNotifyAnimGraph(m_remoteActor, eventName);
                uint64_t now = GetTickCount64();
                s_forceAnimUntil = now + 200;
                PLOG_INFO("[PULSAR] Forced sneak: {}"sv, wantSneak ? "crouch" : "stand");
            }
        }

        // Jump sync - fire JumpUp/JumpFall/JumpDown events on clone
        // 2026-04-20: removed 400ms cooldown. The cooldown was swallowing
        // rapid transitions (JumpUp→JumpFall in ~50ms, or short jumps where
        // 2→0 ground-reset happened within the cooldown window). Result:
        // fall animation stuck after landing, freeze-in-air mid-jump. The
        // netJump != s_cloneJumpState check already dedupes same-state
        // packets, so no event spam without the cooldown.
        if (m_remoteActor && m_hasPosition && !skipCloneGraphOps) {
            int32_t netJump = m_targetPosData.vars.iSyncJumpState;
            static int32_t s_cloneJumpState = 0;
            static uint64_t s_syntheticFallAt = 0;  // scheduled synthetic JumpFall fire tick
            static uint64_t s_landReassertUntil = 0;   // #clone-jump landing watchdog window
            static uint64_t s_lastLandReassertMs = 0;

            uint64_t now = GetTickCount64();

            // 2026-04-20 City-vs-Planet log comparison showed:
            //   City host sends iSyncJumpState 0→1→2→0 (with weapon drawn).
            //   Planet host sends 0→1→0 (weapon holstered → graph skips state 2).
            // Without state 2 we never fire JumpFall → clone stays in JumpUp pose
            // (tucked legs) → visual "Klumpen/ball" until landing.
            // Fix: if state 2 doesn't arrive within 250ms of state 1, fire
            // JumpFall synthetically so the clone's graph transitions properly.
            if (s_syntheticFallAt > 0 && now >= s_syntheticFallAt) {
                s_syntheticFallAt = 0;
                // Only fire if we're still in "up" state - if state already
                // transitioned to 2 or 0, real path already fired JumpFall/Down.
                if (s_cloneJumpState == 1 && m_remoteActor) {
                    g_forceAnimation = true;
                    RE::BSFixedString eF("JumpFall");
                    SafeNotifyAnimGraph(m_remoteActor, eF);
                    s_forceAnimUntil = now + 700;
                    PLOG_INFO("[PULSAR] Synthetic JumpFall fired (state 2 missing)"sv);
                }
            }

            // #clone-jump landing watchdog - the single JumpDown fired on the 2→0
            // transition sometimes fails to pull the clone's graph out of JumpFall
            // (short / low-ceiling jumps: the fall→land transition needs the clone's
            // physics to settle, which lags the one-shot event). Without a retry the
            // clone hangs in fall until the player jumps again. While the host stays
            // grounded for a short window after landing, re-assert grounded a few
            // times (rate-limited) so a missed transition self-corrects. JumpDown on
            // an already-grounded graph is a no-op, so clean landings are unaffected.
            if (s_landReassertUntil > 0 && now < s_landReassertUntil &&
                netJump == 0 && s_cloneJumpState == 0 && m_remoteActor &&
                (now - s_lastLandReassertMs) >= 120) {
                s_lastLandReassertMs = now;
                g_forceAnimation = true;
                RE::BSFixedString eD("JumpDown");
                SafeNotifyAnimGraph(m_remoteActor, eD);
                s_forceAnimUntil = now + 150;
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> gMgr;
                if (SafeGetAnimGraphManager(m_remoteActor, gMgr) && gMgr &&
                    gMgr->graphs.size() > 0) {
                    auto& graph = gMgr->graphs[gMgr->activeGraph];
                    if (graph) {
                        auto** vt = *reinterpret_cast<void***>(graph.get());
                        ForceSetGraphIntDirect(graph.get(), vt[0x20], "iSyncJumpState", 0);
                    }
                }
            } else if (s_landReassertUntil > 0 &&
                       (now >= s_landReassertUntil || netJump != 0)) {
                s_landReassertUntil = 0;   // window elapsed or a new jump started
            }

            if (netJump != s_cloneJumpState) {
                int32_t prevJump = s_cloneJumpState;
                s_cloneJumpState = netJump;

                // iSyncJumpState: 0=grounded, 1=jumpup/takeoff, 2=falling,
                // 3=boostpack active (confirmed via T124 host-diag: sequence
                // 0→1→3→2→0 during boost-press, 0→1→2→0 during plain jump).
                // Fire multiple events to ensure jump animation triggers.

                if (netJump == 1) {
                    // Jump start - cover the full jump arc (~1-1.5s).
                    g_forceAnimation = true;
                    RE::BSFixedString e1("JumpUp");
                    SafeNotifyAnimGraph(m_remoteActor, e1);
                    s_forceAnimUntil = now + 1500;
                    // Schedule synthetic JumpFall in case state 2 never arrives
                    s_syntheticFallAt = now + 250;
                    PLOG_INFO("[PULSAR] Forced jump: JumpUp (state={} prev={})"sv, netJump, prevJump);
                } else if (netJump == 3) {
                    // #clone-jump (2026-06-25): planet jumps engage the boostpack, so
                    // EVERY jump went 0→1→3→2→0. The old separate BoostUp event dropped
                    // the clone into a sustained boost-hover pose (the "Kugel"/tucked
                    // ball) that the brief jump-boost never cleanly exits. Treat the
                    // boost as a CONTINUATION of the jump instead: re-affirm JumpUp (keep
                    // the jump pose, no boost pose on the clone - cosmetic), extend the
                    // window, and keep a synthetic-fall fallback so the descent triggers.
                    g_forceAnimation = true;
                    RE::BSFixedString e1("JumpUp");
                    SafeNotifyAnimGraph(m_remoteActor, e1);
                    s_forceAnimUntil = now + 1500;
                    if (s_syntheticFallAt == 0) s_syntheticFallAt = now + 400;
                    PLOG_INFO("[PULSAR] Forced jump: boost-as-JumpUp (state={} prev={})"sv, netJump, prevJump);
                } else if (netJump == 2) {
                    // Fall phase - real state 2 received, cancel synthetic.
                    // Also covers boost-descent (3→2 when boost ends).
                    s_syntheticFallAt = 0;
                    g_forceAnimation = true;
                    // #clone-jump - boost is now treated as JumpUp (no boost pose), so
                    // the descent is a plain fall; no BoostDown needed.
                    RE::BSFixedString e1("JumpFall");
                    SafeNotifyAnimGraph(m_remoteActor, e1);
                    s_forceAnimUntil = now + 700;
                    PLOG_INFO("[PULSAR] Forced jump: JumpFall (state={} prev={})"sv, netJump, prevJump);
                } else if (netJump == 0 && (prevJump == 2 || prevJump == 1 || prevJump == 3)) {
                    // Landing - cancel any pending synthetic fall.
                    s_syntheticFallAt = 0;
                    g_forceAnimation = true;
                    RE::BSFixedString e1("JumpDown");
                    SafeNotifyAnimGraph(m_remoteActor, e1);
                    s_forceAnimUntil = now + 400;
                    // #clone-jump - arm the landing watchdog so a JumpDown that didn't
                    // take (graph still in fall) is re-asserted over the next ~600ms.
                    s_landReassertUntil = now + 600;
                    PLOG_INFO("[PULSAR] Forced jump: JumpDown (state={} prev={})"sv, netJump, prevJump);
                }

                // Also set iSyncJumpState graph variable
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> gMgr;
                if (SafeGetAnimGraphManager(m_remoteActor, gMgr) && gMgr &&
                    gMgr->graphs.size() > 0) {
                    auto& graph = gMgr->graphs[gMgr->activeGraph];
                    if (graph) {
                        auto** vt = *reinterpret_cast<void***>(graph.get());
                        ForceSetGraphIntDirect(graph.get(), vt[0x20], "iSyncJumpState", netJump);
                    }
                }
            }
        }

        // ForceRespawn: despawn now, spawn after 2s delay
        // Used when stale Papyrus state (old RemotePlayer ref or GLOB=3.0 from loaded save)
        if (m_pendingForceRespawn.exchange(false)) {
            // while the remote player is seated in a vehicle, suppress the
            // despawn/respawn churn. Respawning spawns the clone in front of the
            // moving vehicle (host runs it over), and the per-cell-transition churn
            // races freed objects (null-array CTD during the ride). Keep the seated
            // clone put; respawn resumes once they exit (seated bit -> false).
            if (m_remoteActor && Pulsar::VehicleSync::IsCloneSeated(m_remoteActor)) {
                PLOG_INFO("[PULSAR] ForceRespawn: suppressed (remote seated in vehicle)"sv);
            } else {
                DoDespawn();
                const std::uint32_t delayMs = m_forceRespawnDelayMs.load();
                m_delayedSpawnAt.store(GetTickCount64() + delayMs);
                PLOG_INFO("[PULSAR] ForceRespawn: despawned, spawning in {}ms"sv, delayMs);
            }
        }
        {
            uint64_t spawnAt = m_delayedSpawnAt.load();
            if (spawnAt > 0 && GetTickCount64() >= spawnAt) {
                if (m_delayedSpawnAt.compare_exchange_strong(spawnAt, 0)) {
                    DoSpawn();
                    PLOG_INFO("[PULSAR] ForceRespawn: requesting spawn now"sv);
                }
            }
        }

        // Check if Papyrus has finished spawning
        if (m_globSpawn && m_globSpawn->value == 2.0f && !m_remoteActor) {
            FindSpawnedActor();
        }

        // Resolve LookAt marker (spawned by Papyrus after clone)
        if (m_remoteActor && !m_lookMarker) {
            FindLookMarker();
        }

        // Keep PulsarSpawn=1.0 while waiting for Papyrus spawn
        if (m_spawned && !m_remoteActor && m_globSpawn && m_delayedSpawnAt.load() == 0) {
            float spawnVal = m_globSpawn->value;
            if (spawnVal != 1.0f && spawnVal != 2.0f && spawnVal != 3.0f) {
                m_globSpawn->value = 1.0f;
                static uint64_t s_lastRewriteLog = 0;
                uint64_t now = GetTickCount64();
                if (now - s_lastRewriteLog > 3000) {
                    s_lastRewriteLog = now;
                    PLOG_INFO("[PULSAR] PulsarSpawn was {:.1f}, re-writing 1.0 (save-load reset?)"sv, spawnVal);
                }
            }
        }

        // Diagnostic: log spawn wait time
        if (m_spawned && !m_remoteActor && m_spawnRequestTime > 0) {
            uint64_t elapsed = GetTickCount64() - m_spawnRequestTime;
            if (elapsed > 15000 && !m_spawnTimeoutWarned) {
                m_spawnTimeoutWarned = true;
                float spawnVal = m_globSpawn ? m_globSpawn->value : -1.0f;
                PLOG_WARN("[PULSAR] SPAWN WAIT: Papyrus hasn't spawned after 15s. PulsarSpawn={:.1f}"sv, spawnVal);
            }
        }
    }

    // --- GLOB resolution ---

    void RemotePlayerManager::ResolveGlobs()
    {
        m_globPosX   = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarPosX"));
        m_globPosY   = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarPosY"));
        m_globPosZ   = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarPosZ"));
        m_globRotZ   = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarRotZ"));
        m_globSpeed  = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarSpeed"));
        m_globDirty  = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarDirty"));
        m_globSpawn  = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarSpawn"));
        m_globFormHi = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarFormHi"));
        m_globFormLo = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarFormLo"));
        m_globLookHi = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarLookMarkerHi"));
        m_globLookLo = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("PulsarLookMarkerLo"));

        if (m_globPosX && m_globPosY && m_globPosZ && m_globRotZ && m_globSpeed &&
            m_globDirty && m_globSpawn && m_globFormHi && m_globFormLo) {
            m_globsResolved = true;
            PLOG_INFO("[PULSAR] All GLOB forms resolved from PULSAR.esp"sv);
        } else {
            static uint64_t s_lastWarn = 0;
            uint64_t now = GetTickCount64();
            if (now - s_lastWarn > 5000) {
                s_lastWarn = now;
                PLOG_WARN("[PULSAR] PULSAR.esp not loaded - GLOB forms not found"sv);
            }
        }
    }

    // --- Spawn ---

    void RemotePlayerManager::DoSpawn()
    {
        if (!m_globsResolved || m_spawned) return;

        m_globSpawn->value = 1.0f;
        m_spawned = true;
        m_lerpInit = false;
        m_spawnRequestTime = GetTickCount64();
        m_spawnTimeoutWarned = false;
        PLOG_INFO("[PULSAR] Spawn requested"sv);
    }

    // --- Despawn ---

    void RemotePlayerManager::DoDespawn()
    {
        if (!m_globSpawn) return;

        // release our owned headlamp light on the clone WHILE it is still
        // valid (before the engine tears down its 3D node) to avoid a dangling-ref UAF.
        Pulsar::FlashlightSync::ReleaseForClone(m_remoteActor);

        m_globSpawn->value = 0.0f;
        m_remoteActor = nullptr;
        m_spawned = false;
        m_hasPosition = false;
        m_hasBones = false;
        m_lerpInit = false;
        m_boneCache.resolved = false;
        s_clonePitchActor = nullptr;
        s_clonePitchBoneCache.resolved = false;
        std::memset(s_clonePitchBoneCache.bones, 0, sizeof(s_clonePitchBoneCache.bones));
        // Restore original vtables on old clone - prevents use-after-free when engine
        // still holds animation update references to the freed clone actor
        if (s_cloneGraphChannelObj && s_origGraphVtable) {
            *reinterpret_cast<void***>(s_cloneGraphChannelObj) = s_origGraphVtable;
            s_cloneGraphChannelObj = nullptr;
            s_origGraphVtable = nullptr;
        }
        if (s_postAnimFunctorPtr && s_origPostAnimVtable) {
            *reinterpret_cast<void***>(s_postAnimFunctorPtr) = s_origPostAnimVtable;
            s_postAnimFunctorPtr = nullptr;
            s_origPostAnimVtable = nullptr;
        }
        s_postAnimHookInstalled = false;
        s_animHookInstalled = false;
        s_directCtrl = nullptr;
        s_directCtrlVtable = nullptr;
        s_directCtrlEnabled = false;
        s_movCtrlProbed = false;
        s_hookCloneActor = nullptr;

        // Clear FormHi/Lo so FindSpawnedActor won't pick up stale save-state FormIDs
        if (m_globFormHi) m_globFormHi->value = 0.0f;
        if (m_globFormLo) m_globFormLo->value = 0.0f;

        // Re-queue weapon/armor so second clone gets re-equipped automatically
        if (m_currentWeaponFormID != 0) {
            m_pendingWeaponFormID.store(m_currentWeaponFormID);
            m_pendingEquip.store(true);
        }
        m_currentWeaponFormID = 0;
        if (m_currentArmorCount > 0) {
            std::memcpy(m_pendingArmorIDs, m_currentArmorIDs, m_currentArmorCount * sizeof(uint32_t));
            m_pendingArmorCount = m_currentArmorCount;
            m_pendingArmor.store(true);
        }
        m_currentArmorCount = 0;

        m_weaponDrawn = false;
        m_weaponDrawTarget = false;
        m_weaponDrawChangeTime = 0;
        m_pendingRedraw = 0;
        m_pendingRedrawDrawAt = 0;  // reset 2-stage deferred re-draw on respawn
        m_lookMarker = nullptr;
        m_graphKickstartDrawAt = 0;
        m_graphKickstartHolsterAt = 0;
        m_graphKickstartDone = false;
        PLOG_INFO("[PULSAR] Despawn requested"sv);
    }

    // --- CharacterController creation via engine function ---
    // REL::ID(101052) = FUN_14193bed0: creates CC from Actor data
    // Resolve outside __try (C++ object not allowed in SEH function)
    using CreateCC_t = void(*)(void*);
    static REL::Relocation<CreateCC_t> s_createCC{ REL::ID(101052) };

    static int CallCreateCharControllerRaw(void* actor)
    {
        __try {
            s_createCC(actor);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // --- Find spawned actor ---

    void RemotePlayerManager::FindSpawnedActor()
    {
        if (!m_globFormHi || !m_globFormLo) return;

        float hi = m_globFormHi->value;
        float lo = m_globFormLo->value;
        if (hi == 0.0f && lo == 0.0f) return;

        int32_t hiS = static_cast<int32_t>(hi);
        int32_t loS = static_cast<int32_t>(lo);
        int32_t signedID = hiS * 65536 + loS;
        uint32_t formID = static_cast<uint32_t>(signedID);

        auto* form = RE::TESForm::LookupByID(formID);
        if (!form) {
            PLOG_WARN("[PULSAR] FormID 0x{:08X} not found"sv, formID);
            return;
        }

        m_remoteActor = form->As<RE::Actor>();
        if (m_remoteActor) {
            m_globSpawn->value = 3.0f;
            m_spawned = true;
            // F.2-v2 crash fix: the equip apply (DoEquipWeapon/Armor) touches the
            // clone's anim graph; running it on a freshly-respawned clone whose
            // graph is still rebuilding pure-virtual-crashes (both-down reload,
            // 2026-06-11). Stamp the spawn time and require a settle window
            // before equip applies (see ProcessPending equipSafe gate).
            m_cloneSpawnedAtMs.store(GetTickCount64(), std::memory_order_relaxed);

            // Combat V2-1e: the clone's replayed fire actions spawn REAL engine
            // projectiles. Damage is synced over the network (damage-forward),
            // so the clone's own bullets must be COSMETIC - otherwise its hits
            // land on top of the forwarded damage (double counting both ways).
            {
                static RE::ActorValueInfo* s_admAV = nullptr;
                if (!s_admAV) {
                    s_admAV = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                        RE::BSFixedString("AttackDamageMult"));
                }
                if (s_admAV) {
                    m_remoteActor->SetActorValue(*s_admAV, 0.0f);
                    PLOG_INFO("[PULSAR] combat: clone AttackDamageMult=0 (visual-only shots)"sv);
                } else {
                    PLOG_WARN("[PULSAR] combat: AttackDamageMult AV not found - clone shots stay live"sv);
                }
            }

            // Bisect: in procedural cells, do NOTHING to the clone - no hook
            // registration (so PerformAction hook ignores it), no AttackingDisabled
            // flag, no CC creation. Engine AI runs freely on the clone.
            bool isProcCell = (m_remoteActor->parentCell &&
                (m_remoteActor->parentCell->formID & 0xFF000000) == 0xFF000000);

            if (isProcCell) {
                PLOG_INFO("[PULSAR] Remote actor found (PROC CELL - SEH-guarded hooks): "
                          "FormID=0x{:08X}"sv, formID);
                m_remoteActor->boolFlags.set(
                    RE::Actor::BOOL_FLAGS::kAttackingDisabled,
                    RE::Actor::BOOL_FLAGS::kEssential,
                    RE::Actor::BOOL_FLAGS::kProtected);
                s_hookCloneActor = m_remoteActor;
                // Both hooks installed; pass-through branches for proc-cell clone
                // are wrapped in SEH (IsActorInProcCell) to tolerate cell-transition
                // races.
                InstallEvalPackageHook();
                InstallPerformActionHook();
            } else {
                m_remoteActor->boolFlags.set(
                    RE::Actor::BOOL_FLAGS::kAttackingDisabled,
                    RE::Actor::BOOL_FLAGS::kEssential,
                    RE::Actor::BOOL_FLAGS::kProtected);

                // Install hooks to block AI + ALL actions on clone
                s_hookCloneActor = m_remoteActor;
                InstallEvalPackageHook();
                InstallPerformActionHook();

                // Debug: check CharacterController on clone vs player
                auto* cloneProcess = m_remoteActor->currentProcess;
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* playerProcess = player ? player->currentProcess : nullptr;

                void* cloneCC = nullptr;
                void* playerCC = nullptr;
                if (cloneProcess && cloneProcess->middleHigh) {
                    cloneCC = cloneProcess->middleHigh->charController.get();
                }
                if (playerProcess && playerProcess->middleHigh) {
                    playerCC = playerProcess->middleHigh->charController.get();
                }
                PLOG_INFO("[PULSAR] Remote actor found! FormID=0x{:08X}"sv, formID);
                PLOG_INFO("[PULSAR]   Clone CC=0x{:X} Player CC=0x{:X}"sv,
                    reinterpret_cast<uintptr_t>(cloneCC), reinterpret_cast<uintptr_t>(playerCC));

                // Create CharacterController for clone if missing
                if (!cloneCC && cloneProcess && cloneProcess->middleHigh) {
                    PLOG_INFO("[PULSAR] Creating CharacterController for clone..."sv);
                    int result = CallCreateCharControllerRaw(m_remoteActor);

                    void* newCC = cloneProcess->middleHigh->charController.get();
                    if (result == 1 && newCC) {
                        PLOG_INFO("[PULSAR] CC created successfully! CC=0x{:X}"sv,
                            reinterpret_cast<uintptr_t>(newCC));
                    } else {
                        PLOG_WARN("[PULSAR] CC creation failed: result={} CC=0x{:X}"sv,
                            result, reinterpret_cast<uintptr_t>(newCC));
                    }
                }
            }

            // CC creation for proc-cell clone - REQUIRED. T9 RE found the
            // real gate: CC+0x20 starts null, gets set by engine's Havok-
            // attach pipeline asynchronously. SafeHavokBodySetPosition checks
            // that gate and skips frames until attachment completes. Once
            // attached, setPosition calls succeed and body syncs.
            if (isProcCell) {
                auto* cloneProcess = m_remoteActor->currentProcess;
                if (cloneProcess && cloneProcess->middleHigh) {
                    void* existingCC = cloneProcess->middleHigh->charController.get();
                    if (!existingCC) {
                        PLOG_INFO("[PULSAR] PROC: creating CC on clone..."sv);
                        int result = CallCreateCharControllerRaw(m_remoteActor);
                        void* newCC = cloneProcess->middleHigh->charController.get();
                        PLOG_INFO("[PULSAR] PROC: CC result={} CC=0x{:X}"sv,
                            result, reinterpret_cast<uintptr_t>(newCC));
                    }
                }
            }
        } else {
            PLOG_WARN("[PULSAR] FormID 0x{:08X} is not an Actor"sv, formID);
        }
    }

    // --- Movement Controller: IMovementDirectControl ---
    // From Ghidra RE: Actor+0x240 = MovementController, movCtrl+0x48 = dispatch table
    // vtable[1] = Enable, vtable[5] = SetTargetPosition(NiPoint3*, float speed)
    using InterfaceLookup_t = void*(*)(void* dispatchTable, void* interfaceName);
    static REL::Relocation<InterfaceLookup_t> s_interfaceLookup{ REL::ID(49356) };

    static void* SafeInterfaceLookup(void* dispatch, void* namePtr)
    {
        void* result = nullptr;
        __try {
            result = s_interfaceLookup(dispatch, namePtr);
        } __except(1) {
            PLOG_WARN("[PULSAR] SEH exception in interface lookup"sv);
        }
        return result;
    }

    static void ProbeMovementController(RE::Actor* actor)
    {
        uintptr_t actorAddr = reinterpret_cast<uintptr_t>(actor);
        uintptr_t imageBase = REX::FModule::GetExecutingModule().GetBaseAddress();

        void* movCtrl = *reinterpret_cast<void**>(actorAddr + 0x240);
        PLOG_INFO("[PULSAR] MovCtrl: Actor+0x240 = 0x{:X}"sv, reinterpret_cast<uintptr_t>(movCtrl));

        if (!movCtrl) {
            PLOG_WARN("[PULSAR] No MovementController on clone"sv);
            return;
        }

        void* dispatch = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(movCtrl) + 0x48);
        PLOG_INFO("[PULSAR] MovCtrl dispatch: 0x{:X}"sv, reinterpret_cast<uintptr_t>(dispatch));

        if (!dispatch) {
            PLOG_WARN("[PULSAR] No dispatch table"sv);
            return;
        }

        RE::BSFixedString dcName("IMovementDirectControl");
        s_directCtrl = SafeInterfaceLookup(dispatch, &dcName);
        PLOG_INFO("[PULSAR] IMovementDirectControl = 0x{:X}"sv, reinterpret_cast<uintptr_t>(s_directCtrl));

        if (s_directCtrl) {
            s_directCtrlVtable = *reinterpret_cast<void***>(s_directCtrl);

            for (int i = 0; i < 9; i++) {
                uintptr_t funcAddr = reinterpret_cast<uintptr_t>(s_directCtrlVtable[i]);
                PLOG_INFO("[PULSAR]   DC vtable[{}] RVA=0x{:X}"sv, i, funcAddr - imageBase);
            }

            // Enable direct control (vtable[1])
            if (SafeCallDCEnable(s_directCtrl, s_directCtrlVtable[1])) {
                s_directCtrlEnabled = true;
                PLOG_INFO("[PULSAR] IMovementDirectControl ENABLED"sv);
            } else {
                PLOG_WARN("[PULSAR] Failed to enable DirectControl"sv);
                s_directCtrl = nullptr;
            }
        }
    }

    // (SEH wrappers SafeCallDCEnable/SafeCallDCSetTarget defined above)

    // --- Weapon equip ---

    // Direct call to ActorEquipManager::EquipObject using verified REL::IDs
    // IDs.h has REL::ID(0) placeholders - we bypass with correct IDs for v1.16.236
    static bool DirectEquipObject(RE::Actor* a_actor, const RE::BGSObjectInstance& a_object)
    {
        // ActorEquipManager::Singleton: REL::ID(938503) - already works in IDs.h
        static REL::Relocation<RE::ActorEquipManager**> singleton{ REL::ID(938503) };
        auto* mgr = *singleton;
        if (!mgr) return false;

        // ActorEquipManager::EquipObject: REL::ID(101949) - verified for v1.16.236
        using EquipFn = bool(*)(RE::ActorEquipManager*, RE::Actor*, const RE::BGSObjectInstance&,
                                const RE::BGSEquipSlot*, bool, bool, bool, bool, bool);
        static REL::Relocation<EquipFn> equipFn{ REL::ID(101949) };
        return equipFn(mgr, a_actor, a_object, nullptr, false, true, false, true, false);
    }

    static bool DirectUnequipObject(RE::Actor* a_actor, const RE::BGSObjectInstance& a_object)
    {
        static REL::Relocation<RE::ActorEquipManager**> singleton{ REL::ID(938503) };
        auto* mgr = *singleton;
        if (!mgr) return false;

        // ActorEquipManager::UnequipObject: REL::ID(101951) - verified for v1.16.236
        using UnequipFn = bool(*)(RE::ActorEquipManager*, RE::Actor*, const RE::BGSObjectInstance&,
                                  const RE::BGSEquipSlot*, bool, bool, bool, bool, const RE::BGSEquipSlot*);
        static REL::Relocation<UnequipFn> unequipFn{ REL::ID(101951) };
        return unequipFn(mgr, a_actor, a_object, nullptr, false, true, false, true, nullptr);
    }

    // clear the clone's weapon-draw state (Actor+0xFC low 3 bits) to SHEATHED.
    // DrawWeaponMagicHands(true) early-returns ("already drawn") when those bits are
    // 2/3 - which is the case after a swap (the pose stays drawn). Clearing them makes
    // the next draw a real 0->drawn transition so the engine re-attaches the new weapon
    // mesh from the holster node (R_BackPackHolster) to the hand node (Weapon).
    // RE 2026-06-20: DrawWeaponMagicHands = FUN_1419164f0, reads (actor+0xFC)&7.
    static void SafeForceWeaponSheathedState(RE::Actor* a_actor) noexcept
    {
        __try {
            std::uint32_t* st = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(a_actor) + 0xFC);
            *st &= ~0x7u;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void RemotePlayerManager::InvalidateCloneOnSaveLoad()
    {
        // the save-load reload frees the clone + its light; drop our
        // tracking WITHOUT DecRef (touching the freed light would UAF).
        Pulsar::FlashlightSync::ForgetAll();
        // Null cached clone pointers - the engine frees the actors during the reload.
        m_remoteActor = nullptr;
        m_lookMarker  = nullptr;
        m_spawned     = false;
        // Clear the spawn GLOB FormID so FindSpawnedActor can't re-acquire the stale
        // (freed) clone after the load; the Papyrus re-spawn writes a fresh FormID.
        if (m_globFormHi) m_globFormHi->value = 0.0f;
        if (m_globFormLo) m_globFormLo->value = 0.0f;
        // Re-queue worn equipment so the re-spawned clone gets weapon + armor back.
        if (m_currentWeaponFormID != 0) {
            m_pendingWeaponFormID.store(m_currentWeaponFormID);
            m_pendingEquip.store(true);
        }
        m_currentWeaponFormID = 0;
        if (m_currentArmorCount > 0) {
            std::memcpy(m_pendingArmorIDs, m_currentArmorIDs, m_currentArmorCount * sizeof(uint32_t));
            m_pendingArmorCount = m_currentArmorCount;
            m_pendingArmor.store(true);
        }
        m_currentArmorCount = 0;
        // Reset weapon-draw + deferred-redraw state so no stale tick fires on the new clone.
        m_weaponDrawn = false;
        m_weaponDrawTarget = false;
        m_weaponDrawChangeTime = 0;
        m_pendingRedraw = 0;
        m_pendingRedrawDrawAt = 0;
        PLOG_INFO("[PULSAR] InvalidateCloneOnSaveLoad - clone ptr + spawn GLOB cleared (both-down UAF guard)"sv);
    }

    void RemotePlayerManager::DoEquipWeapon()
    {
        if (!m_remoteActor) return;

        uint32_t formID = m_pendingWeaponFormID.load();
        if (formID == m_currentWeaponFormID) return;

        const uint32_t diagOldFormID = m_currentWeaponFormID;  // for swap log

        // Unequip old weapon
        if (m_currentWeaponFormID != 0) {
            auto* oldForm = RE::TESForm::LookupByID(m_currentWeaponFormID);
            if (oldForm) {
                RE::BGSObjectInstance oldObj(oldForm, nullptr);
                DirectUnequipObject(m_remoteActor, oldObj);
            }
        }

        m_currentWeaponFormID = formID;

        if (formID == 0) {
            PLOG_INFO("[PULSAR] Clone weapon unequipped"sv);
            return;
        }

        auto* weaponForm = RE::TESForm::LookupByID(formID);
        if (!weaponForm || !weaponForm->IsWeapon()) {
            PLOG_WARN("[PULSAR] Weapon 0x{:08X} not found or not a weapon"sv, formID);
            m_currentWeaponFormID = 0;
            return;
        }

        auto* boundObj = reinterpret_cast<RE::TESBoundObject*>(weaponForm);

        // Add to inventory
        m_remoteActor->AddObjectToContainer(boundObj, nullptr, 1, nullptr,
            RE::ITEM_TRANSFER_REASON::kNone);

        // F.2-v2: write the sender's mod array + sentinel onto the freshly
        // added inventory item BEFORE the equip resolves instance data - the
        // clone then renders the weapon with the partner's actual attachments
        // and tier instead of a bare/proc-gen-rolled base item.
        {
            std::lock_guard lk(m_equipModsMutex);
            if (!m_weaponMods.empty() || m_weaponModSentinel != 0xFFFF) {
                Pulsar::WorldSync::Loot::ApplyItemMods(
                    static_cast<RE::TESObjectREFR*>(m_remoteActor), formID,
                    m_weaponMods, m_weaponModSentinel);
            }
        }

        // Find inventory instance data for proper equip
        RE::TBO_InstanceData* invInstData = nullptr;
        m_remoteActor->ForEachInventoryItem([&](const RE::BGSInventoryItem& item) -> RE::BSContainer::ForEachResult {
            if (item.object && item.object->GetFormID() == formID) {
                if (item.instanceData) {
                    invInstData = item.instanceData.get();
                }
                return RE::BSContainer::ForEachResult::kStop;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        // Equip via ActorEquipManager (verified REL::IDs for v1.16.236)
        // Weapon appears holstered on clone's back - "weapon drawn" state requires
        // deeper engine control (Havok Pose Buffer or weapon state hook)
        RE::BGSObjectInstance objInstance(weaponForm, invInstData);
        DirectEquipObject(m_remoteActor, objInstance);
        SafeSet3DUpdateFlag(m_remoteActor, 0x400);

        // On weapon swap while drawn: holster immediately, then schedule re-draw
        // Engine needs to fully process holster before drawing new weapon
        if (m_weaponDrawn) {
            g_forceAnimation = true;
            m_remoteActor->DrawWeaponMagicHands(false);  // holster old weapon NOW
            s_forceAnimUntil = GetTickCount64() + 300;
            m_weaponDrawn = false;       // mark as holstered
            m_weaponDrawTarget = false;  // prevent normal draw code from triggering early
            m_pendingRedraw = GetTickCount64() + 800;
            PLOG_INFO("[PULSAR] weapon swap-while-drawn old=0x{:08X} new=0x{:08X} -> holster + 2-stage redraw"sv,
                      diagOldFormID, formID);
        } else {
            PLOG_INFO("[PULSAR] Equipped weapon 0x{:08X} on clone (holstered)"sv, formID);
            // Kickstart graph: schedule auto draw/holster cycle. The cycle
            // triggers ActionInitializeGraphToBaseState (non-instant), which
            // transitions the graph out of the partial-init state that
            // ActionInstantInitializeGraphToBaseState alone leaves behind.
            // Without this the first jump after spawn glitches.
            if (!m_graphKickstartDone) {
                m_graphKickstartDrawAt = GetTickCount64() + 300;
            }
        }
    }

    // --- Armor equip ---

    void RemotePlayerManager::DoEquipArmor()
    {
        if (!m_remoteActor) return;

        // Check what changed: unequip removed items, equip new ones
        // First: unequip items no longer in the list
        for (uint8_t i = 0; i < m_currentArmorCount; ++i) {
            bool stillEquipped = false;
            for (uint8_t j = 0; j < m_pendingArmorCount; ++j) {
                if (m_currentArmorIDs[i] == m_pendingArmorIDs[j]) {
                    stillEquipped = true;
                    break;
                }
            }
            if (!stillEquipped) {
                auto* form = RE::TESForm::LookupByID(m_currentArmorIDs[i]);
                if (form) {
                    RE::BGSObjectInstance obj(form, nullptr);
                    DirectUnequipObject(m_remoteActor, obj);
                }
            }
        }

        // Equip new items
        for (uint8_t j = 0; j < m_pendingArmorCount; ++j) {
            bool alreadyEquipped = false;
            for (uint8_t i = 0; i < m_currentArmorCount; ++i) {
                if (m_currentArmorIDs[i] == m_pendingArmorIDs[j]) {
                    alreadyEquipped = true;
                    break;
                }
            }
            if (!alreadyEquipped) {
                auto* form = RE::TESForm::LookupByID(m_pendingArmorIDs[j]);
                if (form) {
                    auto* boundObj = reinterpret_cast<RE::TESBoundObject*>(form);
                    m_remoteActor->AddObjectToContainer(boundObj, nullptr, 1, nullptr,
                        RE::ITEM_TRANSFER_REASON::kNone);
                    // F.2-v2: partner's mod array onto the clone's inventory
                    // item before equipping (correct skin/attachments/tier).
                    {
                        std::lock_guard lk(m_equipModsMutex);
                        if (!m_armorMods[j].empty() || m_armorModSentinels[j] != 0xFFFF) {
                            Pulsar::WorldSync::Loot::ApplyItemMods(
                                static_cast<RE::TESObjectREFR*>(m_remoteActor),
                                m_pendingArmorIDs[j],
                                m_armorMods[j], m_armorModSentinels[j]);
                        }
                    }
                    RE::BGSObjectInstance obj(form, nullptr);
                    DirectEquipObject(m_remoteActor, obj);
                }
            }
        }

        // Update current state
        m_currentArmorCount = m_pendingArmorCount;
        std::memcpy(m_currentArmorIDs, m_pendingArmorIDs, m_pendingArmorCount * sizeof(uint32_t));

        SafeSet3DUpdateFlag(m_remoteActor, 0x400);  // crash site 0x1431B6 (client 20:40)
        PLOG_INFO("[PULSAR] Armor sync: {} items equipped"sv, m_currentArmorCount);
    }

    // --- Position + Bone update ---

    void RemotePlayerManager::DoSetPosition()
    {
        if (!m_globsResolved) return;

        // Phase I - drop position applies while we're mid cell-transition.
        // Stale incoming PKT_POSITION (with peer's pre-transition coords)
        // applied during our local transition causes clone cell-flip
        // pingpong (verified 2026-04-27 log 20:11:08.959). After the quiet
        // window expires, the most-recent m_targetPosData is applied
        // naturally on the next tick.
        if (Pulsar::Cell3DAttach::IsQuiet()) return;

        // 2026-06-11 / Fix A (2026-06-21): post-(re)spawn gate, now SPLIT.
        // The crash this guards is ONLY the per-tick anim-graph sync (ForceSetGraphInt
        // vtable calls + Action firings) on a clone whose graph is still rebuilding
        // (pure-virtual, NOT SEH-catchable; client crash 22:35, frame +0x377E90).
        // The raw POSITION write (data.location / Havok) is SEH-safe and must NOT be
        // gated: gating it left a respawned clone stuck at its Papyrus spawn pos (on
        // the player's head) for 3s before re-syncing . So compute the gate
        // here, run the position write below regardless, and skip ONLY the
        // graph/locomotion block while the window holds. IsQuiet still covers the
        // loadMenu window (full early-return above).
        bool postSpawnGraphGate = false;
        {
            const uint64_t spawnedAt = m_cloneSpawnedAtMs.load(std::memory_order_relaxed);
            postSpawnGraphGate = (spawnedAt != 0 && (GetTickCount64() - spawnedAt) < 3000);
        }

        // Phase I - drop position applies if remote player's parentCell
        // doesn't match local AND we are in an INTERIOR cell. Exterior
        // cells of the same worldspace handle cross-cell SetPosition
        // gracefully (engine re-grids the actor); only interior procedural
        // instances are isolated and suffer cell-flip pingpong on mismatch.
        // (Verified 2026-04-27: even outdoor FormIDs differ per player;
        // pre-Phase-J outdoor coop worked because engine handles it.)
        {
            uint32_t remoteCell = m_remoteCellID.load(std::memory_order_acquire);
            auto* localPlayer = RE::PlayerCharacter::GetSingleton();
            auto* localParent = localPlayer ? localPlayer->parentCell : nullptr;
            uint32_t localCell = localParent ? localParent->formID : 0u;
            bool isInterior = localParent && localParent->IsInterior();
            // K.6 V2-Probe (2026-04-29): proc-cells (FF-prefix) are per-
            // machine instances of identical content. Don't suppress
            // cross-cell SetPosition between two proc-instances - the
            // clone needs to be placed in OUR local instance at peer's
            // coords. Engine handles the 3D-attach via local cell content.
            const bool localIsProc  = (localCell  & 0xFF000000u) == 0xFF000000u;
            const bool remoteIsProc = (remoteCell & 0xFF000000u) == 0xFF000000u;
            const bool procEquivalent = localIsProc && remoteIsProc;

            // Option 2 (2026-06-21): on procedural POIs the host + client
            // diverge into SEPARATE instances of the SAME template (K.6 V2-Probe:
            // content identical, matched 120/120). The old code SUPPRESSED the
            // position apply on any interior cell-mismatch → the clone FROZE the
            // moment host+client weren't in the byte-identical cell FormID (every
            // proc-POI), and stayed frozen = "teammate gone". We now APPLY
            // unconditionally: the clone is pinned to OUR local cell (see parentCell
            // pin below) and just placed at the peer's coords - which map correctly
            // because the instances are content-identical. Worst case is occasional
            // misplacement in a genuinely-different *static* interior (rare in co-op,
            // both players are together) - far better than a permanently frozen clone.
            if (isInterior && remoteCell != 0 && localCell != 0 &&
                localCell != remoteCell && !procEquivalent)
            {
                static uint32_t s_lastLogged = 0;
                static uint64_t s_lastLogTick = 0;
                uint64_t now = ::GetTickCount64();
                if ((s_lastLogged != remoteCell) ||
                    (now - s_lastLogTick > 5000)) {
                    s_lastLogged = remoteCell;
                    s_lastLogTick = now;
                    PLOG_INFO(
                        "[PULSAR] DoSetPosition: cross-cell APPLY (Option 2, "
                        "content-equiv) remote=0x{:08X} local=0x{:08X}"sv,
                        remoteCell, localCell);
                }
                // Option 2 - NO suppression return; fall through and apply.
            }
        }

        // Note: Starfield's SFSE AddPermanentTask dispatches to a worker pool
        // (we observed 6+ distinct tids in one session). ALL of those threads
        // are part of the engine's synchronized task graph - they're "safe" for
        // Havok writes in the same sense the engine's own 51 setPosition
        // callers are (T19). We don't need to lock to a single tid. The race
        // between our writes and proc-gen is handled by SEH + CC-attach-gate
        // below, not by thread-locking.

        float targetX = m_targetPosData.x;
        float targetY = m_targetPosData.y;
        float targetZ = m_targetPosData.z;
        float targetRotZ = m_targetPosData.rotZ;

        // Skip only when we haven't received any position yet (origin coords
        // 0,0,0 are legal game positions on planets; old check dropped them).
        if (!m_hasPosition) return;

        if (m_remoteActor) {
            // 2026-04-20 Pin clone->parentCell to player's parentCell.
            // T22 (Ghidra) verified: proc-gen does NOT race the cell-hashmap
            // and runs on the main thread. The prior "Cell-Hashmap-Race" crash
            // hypothesis was wrong - the actual bug is **UAF on clone->parentCell**:
            // when proc-gen tears down a POI chunk, it frees the TESObjectCELL
            // object, but clone refs keep their stale parentCell pointer.
            // Stage-1 of Actor::SetPosition then derefs freed memory.
            //
            // The player is the streaming anchor - their parentCell is guaranteed
            // live every frame. Assigning clone->parentCell = player->parentCell
            // closes the UAF window and lets us run normal Actor::SetPosition on
            // planets (no more Split-Path needed → 60Hz Havok instead of 10Hz,
            // proper broadphase wake-up, Stage-2 event fires).
            {
                auto* player = RE::PlayerCharacter::GetSingleton();
                RE::TESObjectCELL* tgt = player ? player->parentCell : nullptr;
                if (tgt) SafePinParentCell(m_remoteActor, tgt);
            }

            // One-time probe of Movement Controller interfaces (reset on despawn)
            if (!s_movCtrlProbed) {
                s_movCtrlProbed = true;
                ProbeMovementController(m_remoteActor);
            }
            // Initialize lerp
            if (!m_lerpInit) {
                m_lerpX = targetX;
                m_lerpY = targetY;
                m_lerpZ = targetZ;
                m_lerpRotZ = targetRotZ;
                m_lerpInit = true;
            }

            constexpr float LERP_XY = 0.02f;
            constexpr float LERP_Z  = 0.05f;

            // 2026-04-20: during jump, keep lerp snapped to target. Previously
            // lerp accumulated too slowly (LERP_Z=0.05) to catch up with the
            // jump arc, so when the jump ended (state→0) and we switched from
            // direct targetZ to lerpZ, the clone "hung in the air" at the
            // stale lerpZ and drifted slowly toward ground over ~500ms.
            const bool isJumping = (m_targetPosData.vars.iSyncJumpState != 0);
            if (isJumping) {
                m_lerpX = targetX;
                m_lerpY = targetY;
                m_lerpZ = targetZ;
            } else {
                m_lerpX += (targetX - m_lerpX) * LERP_XY;
                m_lerpY += (targetY - m_lerpY) * LERP_XY;
                m_lerpZ += (targetZ - m_lerpZ) * LERP_Z;
            }
            m_lerpRotZ = targetRotZ;

            // Teleport on huge distance
            float dx = targetX - m_lerpX;
            float dy = targetY - m_lerpY;
            if (dx * dx + dy * dy > 500.0f * 500.0f) {
                m_lerpX = targetX;
                m_lerpY = targetY;
                m_lerpZ = targetZ;
            }

            // Position sync. During jumps the lerp smoothing is too slow for
            // the ballistic arc - the clone's Z stays near ground while the
            // anim graph already plays the jump anim (tucking legs), producing
            // the "legs-inside-body" artifact. During a jump state, feed target
            // position directly so the clone's body follows the real arc.
            const bool isJumpingForPos = (m_targetPosData.vars.iSyncJumpState != 0);
            RE::NiPoint3A pos(
                isJumpingForPos ? targetX : m_lerpX,
                isJumpingForPos ? targetY : m_lerpY,
                isJumpingForPos ? targetZ : m_lerpZ);

            // 2026-04-20 SPLIT PATH retained for proc cells. parentCell pin
            // above fixes the UAF (T22), but T22 scenario (c) - "fresh cell,
            // hashmap not yet constructed" - still triggers AVs in proc cells.
            // City/static cells are safe (parentCell is stable & fully built),
            // so Actor::SetPosition is used there. Proc cells retain
            // data.location + Havok-direct 10Hz bypass.
            // Skip clone SetPosition while seated - the engine's sit-anchor
            // binds the clone to the FURN's sit marker and moves it when
            // the FURN moves. Forcing clone.pos every tick yanks it out of
            // the anchor; ActionGetUp fires, Bethesda cancels the sit, and
            // the clone ends up floating free next to the FURN (observed
            // 2026-04-22 test at 15:59). With Schritt A working, the FURN
            // itself is the authoritative mover; VehicleSync drives it.
            const bool isSeatedNow = Pulsar::VehicleSync::IsCloneSeated(m_remoteActor);
            const bool isProcCell = (!isSeatedNow && m_remoteActor && IsActorInProcCell(m_remoteActor) == 1);
            if (isSeatedNow) {
                static uint64_t s_lastSitLog = 0;
                uint64_t now = GetTickCount64();
                if (now - s_lastSitLog > 3000) {
                    s_lastSitLog = now;
                    PLOG_INFO("[PULSAR] RemotePlayer: clone seated - position driven by FURN sit-anchor"sv);
                }
            } else if (isProcCell) {
                SafeWriteActorDataLocation(m_remoteActor, pos, m_lerpRotZ);

                // 2026-04-24 raised 33→16 (30Hz→60Hz) after CrashGuard::Init
                // (T114) gates the 0x30B350E bone-array NULL-deref. The old
                // 2026-04-20 60Hz retest that crashed "beim 2. Jump" was the
                // same bone-crash signature - that vector is now guarded.
                // 60Hz fixes the 0.5m-Jump-Bug (Gravity zog body zwischen
                // 33ms-Writes runter) und glättet Walk-Anim sichtbar.
                //
                // Skip-if-unchanged during jumps: host-packet rate is 30 Hz
                // but our Havok-write is 60 Hz - without this guard we'd
                // write the SAME pos twice per packet, which Havok
                // interprets as a teleport-to-same-spot and its stabilizer
                // induces visible jitter on the airborne body. Only write
                // when the target actually moved (> 1 cm).
                static uint64_t     s_lastHavokWrite = 0;
                static RE::NiPoint3 s_lastHavokPos(0, 0, 0);
                uint64_t now = GetTickCount64();
                if (now - s_lastHavokWrite >= 16) {
                    const float dx = pos.x - s_lastHavokPos.x;
                    const float dy = pos.y - s_lastHavokPos.y;
                    const float dz = pos.z - s_lastHavokPos.z;
                    const float moved2 = dx*dx + dy*dy + dz*dz;
                    if (moved2 > 0.01f * 0.01f) {
                        s_lastHavokWrite = now;
                        s_lastHavokPos = RE::NiPoint3(pos.x, pos.y, pos.z);
                        SafeHavokBodySetPosition(m_remoteActor, pos);
                    }
                }
            } else {
                if (!SafeSetActorPosition(m_remoteActor, pos, m_lerpRotZ)) {
                    static uint64_t s_lastSetPosWarn = 0;
                    uint64_t now = GetTickCount64();
                    if (now - s_lastSetPosWarn > 3000) {
                        s_lastSetPosWarn = now;
                        PLOG_WARN("[PULSAR] SEH in SetPosition (static cell race?)"sv);
                    }
                }
            }
            s_clonePitchActor = m_remoteActor;

            // Fix A: position is now applied (clone snaps to its synced spot
            // immediately after a respawn instead of clinging to the player's head).
            // Everything below - DirectControl SetSpeed + the anim-graph variable
            // sync + Action firings - touches the (possibly still-rebuilding) anim
            // graph and pure-virtual-crashes during the post-spawn window, so skip
            // it until the graph has settled (~3s). This matches what the old full
            // early-return did, minus the position it was wrongly blocking.
            if (postSpawnGraphGate) return;

            // Animation: use DirectControl to set speed → engine picks correct locomotion
            // During jump (iSyncJumpState != 0), set speed to 0 so locomotion doesn't override jump anim
            // 2026-04-19: Proc-cell skip removed - original crash source was NPCSync,
            // not DirectControl. DirectControl has its own SEH wrapper below.
            if (s_directCtrlEnabled && s_directCtrlVtable) {
                float netSpeed = m_targetPosData.vars.speed;
                if (m_targetPosData.vars.iSyncJumpState != 0) {
                    netSpeed = 0.0f;  // Let jump animation play without locomotion interference
                }
                // vtable[3] = SetSpeed(float) - immediate, engages locomotion system
                if (!SafeCallDCSetSpeed(s_directCtrl, s_directCtrlVtable[3], netSpeed)) {
                    PLOG_WARN("[PULSAR] SEH in DirectControl SetSpeed, falling back"sv);
                    s_directCtrlEnabled = false;
                }
            }

            // AnimGraphHook removed: dt-multiplier is dead code when DirectControl is active,
            // and per-instance vtable patching on a refcounted graph channel causes crashes
            // when ActionInitializeGraphToBaseState rebuilds the graph (channel freed ~5s post-spawn).

            // PostAnimHook removed: same per-instance vtable lifetime issue as AnimHook -
            // actor+0x40 functor vtable patch causes crashes when the engine frees/rebuilds
            // the post-anim functor independently of the actor lifetime.

            // === Bone pitch overlay (world transform, 1000hz) ===
            // Bone pitch test removed - using LookAt marker approach

            // Skip graph-variable writes only for C++-spawned clones.
            // Graph-var writes re-enabled 2026-04-19 after SetPosition-race
            // root-caused as the walking-crash trigger (not graph vars).
            bool skipAnimGraphWrites =
                Pulsar::CloneManager::IsCppSpawnedClone(m_remoteActor->formID);

            // Apply all animation graph variables from network snapshot
            // Map player values → NPC values based on Sarah Morgan reference data:
            //   Normal: iState=10, relax=0  |  Sneak: iState=11, relax=2
            //   Speed == CurrentSpeed  |  sprint=1 only at full sprint (~7.7)
            if (!skipAnimGraphWrites) {
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> gMgr;
                if (SafeGetAnimGraphManager(m_remoteActor, gMgr) && gMgr &&
                    gMgr->graphs.size() > 0) {
                    auto& graph = gMgr->graphs[gMgr->activeGraph];
                    if (graph) {
                        auto** vt = *reinterpret_cast<void***>(graph.get());
                        void* setIntFn   = vt[0x20];
                        void* setFloatFn = vt[0x21];

                        const auto& v = m_targetPosData.vars;

                        // Map player state to NPC state (Sarah pattern)
                        // iState: 10=normal, 11=sneak, 17=combat
                        // relax: 0=relaxed, 1=ready(weapon up), 2=sneak
                        bool isSneaking = (v.iSyncRelaxReadySneak == 2);
                        // When weapon drawn: ALWAYS ready (1) - host relaxes after 3s
                        // but clone needs to stay in ready pose for pitch aim to work
                        bool isWeaponReady = m_weaponDrawn || (v.iSyncRelaxReadySneak == 1);
                        int32_t npcIState = isSneaking ? 11 : (isWeaponReady ? 17 : 10);
                        int32_t npcRelax  = isSneaking ? 2 : (isWeaponReady ? 1 : 0);

                        // 0.7.5 experiment (CloneUsePlayerStates): Human_Root.agx keeps the
                        // player's locomotion states (HumanDefault 0, HumanSneak 1,
                        // HumanSighted 2, ... iState 0-8) next to the NPC ones (NPCDefault
                        // 10, ... 10-19). The mapping above forces the clone into the NPC
                        // states; this hands it the host's own player iState instead.
                        if (Pulsar::GetConfig().cloneUsePlayerStates &&
                            v.iState >= 0 && v.iState <= 8) {
                            npcIState = v.iState;
                            npcRelax  = v.iSyncRelaxReadySneak;
                        }

                        // Weapon aim pose:
                        // Ready (relax=1, iSyncSighted=0): weapon held up, combat walk, head tracks pitch
                        // Sighted (iSyncSighted=1): weapon aims, follows pitch for shooting
                        // Sighted for 8s after last shot, then back to Ready
                        {
                            // Use atomics - DoSetPosition runs on BSTaskPool threads concurrently.
                            // Non-atomic check+set causes every thread to fire ActionSightedRelease.
                            static std::atomic<uint64_t> s_lastShotTime{ 0 };
                            static std::atomic<bool> s_inSighted{ false };

                            // Host RMB-aim trigger - 2026-04-25: any tick where host
                            // has iSyncSighted=1 and weapon is drawn, keep sighted
                            // timer fresh + fire ActionSighted once if not yet in.
                            bool hostSightedTick = UnpackAnimFlags(m_targetPosData.animFlags).hostSighted;
                            if (hostSightedTick && m_weaponDrawn) {
                                s_lastShotTime.store(GetTickCount64());
                                bool wasNotSighted = false;
                                if (s_inSighted.compare_exchange_strong(wasNotSighted, true) &&
                                    m_remoteActor && s_origPerformAction) {
                                    auto* sightedForm = RE::TESForm::LookupByID(0x00004A57);
                                    if (sightedForm) {
                                        static REL::Relocation<void*> s_bgaVa{ REL::ID(393260) };
                                        static REL::Relocation<void*> s_aoVa{ REL::ID(393264) };
                                        alignas(16) uint8_t sa[0x68]{};
                                        *reinterpret_cast<void**>(sa) = s_bgaVa.get();
                                        *reinterpret_cast<RE::Actor**>(sa + 0x08) = m_remoteActor;
                                        *reinterpret_cast<RE::TESForm**>(sa + 0x18) = sightedForm;
                                        *reinterpret_cast<void**>(sa + 0x28) = s_aoVa.get();
                                        g_forceAnimation = true;
                                        SafeCallPerformAction(s_origPerformAction, sa);
                                        PLOG_INFO("[PULSAR] Fired ActionSighted (host aim)"sv);
                                    }
                                }
                            }

                            // Track fire actions - any shot resets the timer
                            uint32_t lastAction = m_lastFireAction.load();
                            if (lastAction == 0x00004A5C || lastAction == 0x00004A5A ||
                                lastAction == 0x00004A57) {  // FireAuto, FireSingle, Sighted
                                s_lastShotTime.store(GetTickCount64());
                                // Fire ActionSighted on clone - CAS ensures only one thread does it
                                bool wasNotSighted = false;
                                if (s_inSighted.compare_exchange_strong(wasNotSighted, true) &&
                                    m_remoteActor && s_origPerformAction) {
                                    auto* sightedForm = RE::TESForm::LookupByID(0x00004A57);
                                    if (sightedForm) {
                                        static REL::Relocation<void*> s_bgaVt{ REL::ID(393260) };
                                        static REL::Relocation<void*> s_aoVt{ REL::ID(393264) };
                                        alignas(16) uint8_t sd[0x68]{};
                                        *reinterpret_cast<void**>(sd) = s_bgaVt.get();
                                        *reinterpret_cast<RE::Actor**>(sd + 0x08) = m_remoteActor;
                                        *reinterpret_cast<RE::TESForm**>(sd + 0x18) = sightedForm;
                                        *reinterpret_cast<void**>(sd + 0x28) = s_aoVt.get();
                                        g_forceAnimation = true;
                                        SafeCallPerformAction(s_origPerformAction, sd);
                                    }
                                }
                            }

                            uint64_t aimNow = GetTickCount64();
                            // Sighted pose triggers (2026-04-25):
                            //   (a) host aiming with RMB (hostSighted bit 11), or
                            //   (b) recent fire within 8s (kept for post-fire hold).
                            // Either path requires m_weaponDrawn to be true.
                            bool hostSighted = UnpackAnimFlags(m_targetPosData.animFlags).hostSighted;
                            bool wantSighted = m_weaponDrawn &&
                                (hostSighted || (aimNow - s_lastShotTime.load() < 8000));

                            // Transition sighted → ready: CAS ensures exactly one thread fires release
                            bool wasSighted = true;
                            if (!wantSighted && s_inSighted.compare_exchange_strong(wasSighted, false)) {
                                auto* releaseForm = RE::TESForm::LookupByID(0x00004A58);
                                if (releaseForm && m_remoteActor && s_origPerformAction) {
                                    static REL::Relocation<void*> s_bgaVr{ REL::ID(393260) };
                                    static REL::Relocation<void*> s_aoVr{ REL::ID(393264) };
                                    alignas(16) uint8_t rd[0x68]{};
                                    *reinterpret_cast<void**>(rd) = s_bgaVr.get();
                                    *reinterpret_cast<RE::Actor**>(rd + 0x08) = m_remoteActor;
                                    *reinterpret_cast<RE::TESForm**>(rd + 0x18) = releaseForm;
                                    *reinterpret_cast<void**>(rd + 0x28) = s_aoVr.get();
                                    g_forceAnimation = true;
                                    SafeCallPerformAction(s_origPerformAction, rd);
                                    s_forceAnimUntil = GetTickCount64() + 500;
                                    PLOG_INFO("[PULSAR] Fired ActionSightedRelease (timer expired)"sv);
                                }
                            }

                            ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncSighted", wantSighted ? 1 : 0);
                            ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncGunDown", 0);
                        }

                        // Floats - Speed + CurrentSpeed must match (Sarah pattern)
                        RE::BSFixedString nSpeed("Speed"), nCurSpeed("CurrentSpeed"),
                                          nDir("Direction"), nTurn("TurnDelta"),
                                          nLook("LookHeadingOffset");
                        ForceSetGraphFloatImpl(graph.get(), setFloatFn, &nSpeed, v.speed);
                        ForceSetGraphFloatImpl(graph.get(), setFloatFn, &nCurSpeed, v.speed);
                        // Same unit on both sides (fraction of a turn, DirectionMap.h).
                        ForceSetGraphFloatImpl(graph.get(), setFloatFn, &nDir, v.direction);
                        ForceSetGraphFloatImpl(graph.get(), setFloatFn, &nTurn, v.turnDelta);
                        ForceSetGraphFloatImpl(graph.get(), setFloatFn, &nLook, v.lookHeading);
                        // T126 confirmed 2026-04-25: Direction write is NOT overwritten
                        // by AIProcess (read-back diag: wrote=0.5 → readback=0.5). But
                        // NPC locomotion blend tree still plays forward anim for
                        // Direction=0.5. NT_LOCOMOTION_BLEND in biped_readylocomotion
                        // must use an additional blend-weight var the Player-graph
                        // supplies. Accepted as limit - fluid backward requires full
                        // NPC anim-graph swap (Beta scope).

                        // Integers - mapped to NPC values
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iState", npcIState);
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncIdleLocomotion", v.iSyncIdleLocomotion);
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncSprintState", v.iSyncSprintState);
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncRelaxReadySneak", npcRelax);

                        // HeadTracking ON - needed for SetLookAt/DirectAt to work
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "bHeadTrackingActive", 1);
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "bNoHeadTrack", 0);

                        // REMOVED 2026-04-19: IsPlayer/IsNPC/iIsPlayer/IsFirstPerson
                        // writes on NPC clones. T12/T16 confirmed these variables
                        // are registered ONLY on the Player anim graph - setting
                        // them on an NPC graph is a silent no-op (GetGraphVariable
                        // returns false, setter just aborts). Wasted cycles with
                        // no effect. Clone uses NPC graph transitions regardless.

                        // T14 iSync* vars still written: weapon/stance/fire/bleed.
                        // Removed 2026-04-25 (T124 host-diag):
                        //   iSyncGravity, iSyncSwimState, FacingBackward/Boost*/
                        //   IsTurning/FullBodyTurn, TurnDeltaSmoothed - all confirmed
                        //   NOT BOUND on player graph (getter returned false for the
                        //   entire session). Writes to unbound names are silent no-ops
                        //   on the clone graph too. Removing saves cycles + noise.
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncWeaponDrawState",
                            m_weaponDrawn ? 1 : 0);
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncActorStance",
                            isWeaponReady ? 1 : 0);
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncRelaxedReadyAlert",
                            npcRelax);  // proper PlayerCharacter name (not RelaxReadySneak)
                        // REVERTED 2026-04-20: syncing iSyncGunActions/iSyncGunDown
                        // from the host triggered the same internal engine
                        // fire-path that PerformAction fire-forward crashes
                        // in proc cells (client AV on first host-weapon-draw).
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncGunActions", 0);
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncGunDown", 0);
                        // while the remote player is downed, hold the clone IN
                        // bleedout - the hard 0 aborted the pose one tick after
                        // ActionBleedoutStart played.
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncBleedout",
                            s_cloneBleedoutPose.load(std::memory_order_relaxed) ? 1 : 0);
                        ForceSetGraphIntDirect(graph.get(), setIntFn, "iSyncPartialBody", 0);

                        // iSyncTurnState - confirmed flipping 0/1/2 on host via T124.
                        // iSyncStrafeState - host reads 0 always (not bound on player),
                        // but harmless; leave for NPC-graph forward-compat.
                        ForceSetGraphIntDirect(graph.get(), setIntFn,
                            "iSyncStrafeState",  v.iSyncStrafeState);
                        ForceSetGraphIntDirect(graph.get(), setIntFn,
                            "iSyncTurnState",    v.iSyncTurnState);

                        // === PITCH AIM via DirectAt marker ===
                        // Known working config from breakthrough session:
                        // LOOK_DIST=500, targetRotZ for yaw, sin(pitch) for Z
                        // No cos(pitch) in X/Y (eliminates yaw jitter at extreme pitch)
                        // SetLookAt from Papyrus activates tracking, ExtraDirectAtTarget sets target
                        //
                        // LookMarker re-enabled 2026-04-19 after crash root-caused
                        // as SetPosition cell-hashmap race (fixed by reverting to
                        // proc-cell-specific position writes).
                        if (m_lookMarker) {
                            float pitch = -v.lookPitch;  // Negate: network value is inverted
                            float yaw   = m_lerpRotZ;

                            // Spherical coords with Bethesda forward: X=sin(yaw), Y=cos(yaw)
                            // cos(pitch) in X/Y shrinks horizontal dist at extreme pitch
                            // so the angle from clone to marker = actual pitch (not atan(sin(pitch)))
                            //
                            // 2026-04-19: reduced LOOK_DIST from 500 to 100. Head-tracking
                            // only cares about the DIRECTION from clone to marker, not the
                            // distance - a 100-unit marker gives the same angle as 500.
                            // Staying close keeps the marker guaranteed inside the clone's
                            // own proc-cell chunk (Starfield proc cells are ~4096 units;
                            // at 500 units a marker could land in a neighbor chunk that's
                            // still streaming in, creating a race on the marker's ref state).
                            constexpr float LOOK_DIST = 100.0f;
                            float cp = std::cos(pitch);
                            float markerX = m_lerpX + std::sin(yaw) * cp * LOOK_DIST;
                            float markerY = m_lerpY + std::cos(yaw) * cp * LOOK_DIST;
                            float markerZ = m_lerpZ + std::sin(pitch) * LOOK_DIST;

                            // Move marker: set BOTH data.location AND 3D position
                            SafeRefSetPosition(m_lookMarker, markerX, markerY, markerZ);
                            m_lookMarker->data.location.x = markerX;
                            m_lookMarker->data.location.y = markerY;
                            m_lookMarker->data.location.z = markerZ;

                            // Force DirectAtTarget ExtraData to our marker (1000hz)
                            int datResult = SafeSetDirectAtTarget(m_remoteActor, m_lookMarker);

                            // DirectAt interpolation - max speed for shooter responsiveness
                            static RE::BSFixedString sGain("LookAtOnGain");
                            static RE::BSFixedString sLeadIn("LookAtOnLeadIn");
                            static RE::BSFixedString sPitchSpd("fPitchSpeed");
                            static RE::BSFixedString sPitchMax("DirectAtPitchMax");
                            ForceSetGraphFloatImpl(graph.get(), setFloatFn, &sGain, 100.0f);
                            ForceSetGraphFloatImpl(graph.get(), setFloatFn, &sLeadIn, 0.0f);
                            ForceSetGraphFloatImpl(graph.get(), setFloatFn, &sPitchSpd, 100.0f);
                            ForceSetGraphFloatImpl(graph.get(), setFloatFn, &sPitchMax, 1.57f);

                            static uint64_t s_lastPitchDbg = 0;
                            if (GetTickCount64() - s_lastPitchDbg > 2000) {
                                s_lastPitchDbg = GetTickCount64();
                                PLOG_INFO("[PULSAR] PITCH: p={:.2f} yaw={:.2f} mZ={:.0f} cZ={:.0f} dat={}"sv,
                                          pitch, yaw, markerZ, m_lerpZ, datResult);
                            }
                        }
                    }
                }
            }

            // Periodic debug
            static uint64_t s_lastAnimLog = 0;
            uint64_t now = GetTickCount64();
            if (now - s_lastAnimLog > 5000) {
                s_lastAnimLog = now;
                PLOG_INFO("[PULSAR] Sync: pos=({:.0f},{:.0f},{:.0f}) speed={:.1f} bones={} weap=0x{:08X}"sv,
                          m_lerpX, m_lerpY, m_lerpZ, m_targetPosData.speed,
                          m_hasBones ? "YES" : "NO", m_currentWeaponFormID);
            }
        }

        // Write GLOBs
        m_globPosX->value  = targetX;
        m_globPosY->value  = targetY;
        m_globPosZ->value  = targetZ;
        m_globRotZ->value  = targetRotZ * 57.2957795f;
        m_globSpeed->value = m_targetPosData.speed;
        m_globDirty->value = 1.0f;
    }
}
