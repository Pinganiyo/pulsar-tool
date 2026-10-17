#include "PCH.h"
#include "WorldSync/WorldSync.h"
#include "WorldSync/StateProbe.h"
#include "VehicleSync/VehicleSync.h"
#include "SaveScanner/SaveScanner.h"
#include "Network/NetworkHost.h"
#include "NPCSync/NPCSync.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "SpaceSync/DockSync.h"
#include "SpaceSync/FastTravelSync.h"
#include "SpaceSync/FlightSync.h"
#include "HookGuard.h"
#include "InventorySync/InventorySync.h"
#include "InventorySync/DirectRestore.h"
#include "NASLog.h"

#include <RE/B/BGSSaveLoad.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/U/UI.h>
#include <Windows.h>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <chrono>

using namespace std::string_view_literals;

namespace Pulsar::WorldSync {

    // FUN_14184d320: sets [mgr+0x70] = char* filename copy, [mgr+0x78] = saveType
    // Then caller sets bit 0x4 in [mgr+0x50] to trigger load on next game tick
    using SetMostRecentSave_t = void(*)(void* mgr, const char* filename, uint32_t saveType);
    static REL::Relocation<SetMostRecentSave_t> s_SetMostRecentSave{ REL::ID(98669) };

    static std::atomic<State> g_state{ State::IDLE };
    static std::mutex g_saveMutex;
    static std::vector<uint8_t> g_saveBuffer;
    static std::chrono::steady_clock::time_point g_triggerAfter;
    static std::chrono::steady_clock::time_point g_loadedAfter;  // fallback: declare LOADED after this time
    static std::atomic<std::uint64_t> g_lastLoadCompletedTick{ 0 };
    static std::uint32_t g_loadDeferCount{ 0 };  // L.7 D2 - load-safe gate deferrals

    // L.7 V13j - rotating save filename. Engine appears to cache the parsed
    // save state per-filename across loads, so reading the SAME PULSAR_Session
    // path twice in one session ends up applying stale state and the load
    // fails with cell=0. Use a fresh filename for each bridge save so the
    // engine treats each as a new save and parses fresh.
    static std::atomic<uint32_t> g_saveSequence{ 0 };
    static std::string           g_lastSaveBaseName;

    // L.7 V13n - bridge auto-retry. The FIRST bridge after connect sometimes
    // completes without the engine ever starting the real load (load-start
    // signalled by taskBitCleared but initFired=false → no teardown → player
    // stays in the pre-load cell). The 2nd attempt always fires initFired and
    // tears down. We detect the non-switch at LOADED (cur cell == pre-load
    // cell while host is elsewhere) and re-trigger the load ONCE with a fresh
    // rotated filename (same bytes copied - reusing the old filename hits the
    // engine parse-state cache and fails, see V13j). Reset per new bridge.
    static std::uint32_t         g_bridgeRetryCount{ 0 };

    // L.7 V13f / V13f.1 / V13k load-progression gates - see LOADING-state.
    //   start          - engine begun the load (any of: probe-counter
    //                     advance, mgrTasks bit-0x4 cleared, parentCell
    //                     null, timeout)
    //   cellGone       - parentCell observed null (engine torn down).
    //                     V13k: also satisfied by loadMenu CLOSED if we
    //                     saw it OPEN earlier (engine reuses same cell
    //                     when both saves are in same interior - Jump 2's
    //                     load completes WITHOUT cell wipe).
    //   loadMenuOpened - V13k addition. Tracked via UI::IsMenuOpen.
    static std::atomic<bool>     g_loadStartObserved{ false };
    static std::atomic<bool>     g_cellGoneObserved{ false };
    static std::atomic<bool>     g_loadMenuOpenedSeen{ false };
    static std::uint32_t         g_entryHookCountAtArm{ 0 };

    // L.7 pre-load snapshot of local player. Captured at
    // OnSaveReceived; consumed once at the first post-LOADED teleport apply.
    // Single-threaded access pattern: writer = network thread (SAVE
    // packet), reader = game-thread tick. Snapshot has no smart pointers
    // and is small, so a simple guard mutex is plenty.
    static PreLoadSnapshot       g_preLoadSnap{};
    static std::mutex            g_preLoadSnapMutex;
    static std::chrono::steady_clock::time_point g_loadStartDeadline;

    State GetState()
    {
        return g_state.load();
    }

    std::uint64_t GetLastLoadCompletedTick()
    {
        return g_lastLoadCompletedTick.load();
    }

    void BeginWaitingForSave()
    {
        g_state = State::WAITING_FOR_SAVE;
        PLOG_INFO("[PULSAR] WorldSync: waiting for host save..."sv);
    }

    void Reset()
    {
        g_state = State::IDLE;
        std::lock_guard lock(g_saveMutex);
        g_saveBuffer.clear();
        // L.7 drop any stale snapshot from a previous session.
        {
            std::lock_guard snapLock(g_preLoadSnapMutex);
            g_preLoadSnap.valid = false;
        }
        PLOG_INFO("[PULSAR] WorldSync: reset"sv);
    }

    // L.7 V11 - engine native QuitToMainMenu handler. RVA 0x14200F4A0 (the
    // Papyrus *.QuitToMainMenu native - registers FadeThenMainMenuCallback,
    // calls the engine's UI-transition function FUN_141EE5D50). Calling it
    // directly schedules the fade-to-main-menu transition; engine fully
    // resets state on the way and we can re-trigger Load PULSAR_Session
    // from main-menu context (which is the "always works" path that
    // bypasses the engine 2nd-mid-game-load bug).
    using EngineQuitToMainMenu_t = void (__fastcall*)();
    static REL::Relocation<EngineQuitToMainMenu_t>
        s_EngineQuitToMainMenu{ REL::Offset(0x200F4A0ULL) };

    bool TriggerEngineQuitToMainMenu()
    {
        const auto fnAddr = REL::Offset(0x200F4A0ULL).address();
        if (!fnAddr) {
            PLOG_WARN("[PULSAR] TriggerEngineQuitToMainMenu: address unresolved"sv);
            return false;
        }
        auto fn = reinterpret_cast<EngineQuitToMainMenu_t>(fnAddr);
        bool ok = false;
        __try { fn(); ok = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] TriggerEngineQuitToMainMenu: threw"sv);
        }
        if (ok) {
            PLOG_INFO(
                "[PULSAR] [L.7 V11] TriggerEngineQuitToMainMenu: scheduled "
                "fade-to-main-menu (full engine reset incoming)"sv);
            NASLOG("L.7 V11 QuitToMainMenu scheduled");
        }
        return ok;
    }

    namespace detail_ {
        // V13k - SEH-safe LoadingMenu open check, kept in its own fn so
        // ProcessPending doesn't trip C2712 (__try with destructable
        // locals).
        bool IsLoadMenuOpen_Impl()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return false;
            RE::BSFixedString name("LoadingMenu");
            return ui->IsMenuOpen(name);
        }
        bool IsLoadMenuOpenSafe()
        {
            __try { return IsLoadMenuOpen_Impl(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // L.7 D2 - is the client in a state where force-loading the bridge
        // save is safe? Force-loading while the player is SEATED in a ship
        // (cockpit/pilot seat) or piloting / mid-takeoff crashes the engine on
        // the load path: a null BSReadWriteLock acquire (write at 0x24,
        // Ghidra 0x1422CC350), observed 2026-05-25 as a takeoff CTD when the
        // host took off, sent a bridge save, and the client force-loaded it
        // while still seated in its own ship. On-foot planet joins are safe.
        bool IsClientSafeToLoad_Impl()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return false;
            // Seated in furniture: actor+0x228 process → +0x08 middleHigh →
            // +0x428 occupiedFurniture handle (non-zero = seated). Same path
            // as NetworkHost::SafeReadPlayerFurnHandle.
            auto* a = reinterpret_cast<std::uint8_t*>(player);
            auto* proc = *reinterpret_cast<std::uint8_t**>(a + 0x228);
            if (proc) {
                auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x08);
                if (mh && *reinterpret_cast<std::uint32_t*>(mh + 0x428) != 0)
                    return false;  // seated (e.g. ship cockpit)
            }
            if (player->IsInSpace(true)) return false;  // piloting / in space
            if (IsLoadMenuOpen_Impl())  return false;    // a load already underway
            return true;
        }
        bool IsClientSafeToLoad()
        {
            __try { return IsClientSafeToLoad_Impl(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }  // fault → unsafe
        }

        // L.7 D2.2 (, 2026-06-23) - is the load-block specifically the
        // HARD case (seated in a ship / piloting / in space)? Force-loading the
        // bridge save here hits the null-lock takeoff CTD (Ghidra 0x1422CC350
        // family; reference_crash_22ce9c6.md). The merely-transient block
        // (LoadingMenu open) is NOT this case and may still force-proceed.
        bool IsClientSeatedOrInSpace_Impl()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return false;  // null = transient load, not the hard case
            auto* a = reinterpret_cast<std::uint8_t*>(player);
            auto* proc = *reinterpret_cast<std::uint8_t**>(a + 0x228);
            if (proc) {
                auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x08);
                if (mh && *reinterpret_cast<std::uint32_t*>(mh + 0x428) != 0)
                    return true;  // seated (e.g. ship cockpit)
            }
            if (player->IsInSpace(true)) return true;  // piloting / in space
            return false;
        }
        bool IsClientSeatedOrInSpace()
        {
            __try { return IsClientSeatedOrInSpace_Impl(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return true; }  // fault → assume hard, abort
        }

        // SEH-wrapped player +0x113c flag clear - kept in its own function
        // so the caller (ProcessPending) doesn't trip C2712.
        void ClearPlayerPreLoadFlag_Impl(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return;
            auto* pflag = reinterpret_cast<volatile std::uint8_t*>(
                reinterpret_cast<std::uint8_t*>(a_player) + 0x113c);
            std::uint8_t prevFlag = *pflag;
            *pflag = 0;
            PLOG_INFO(
                "[PULSAR] [L.7 V12] cleared player+0x113c flag "
                "(0x{:02X} → 0x00)"sv, prevFlag);
        }
    }

    void detail_ClearPlayerPreLoadFlag(RE::PlayerCharacter* a_player)
    {
        __try { detail_::ClearPlayerPreLoadFlag_Impl(a_player); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN(
                "[PULSAR] [L.7 V12] player +0x113c clear threw"sv);
        }
    }

    bool TriggerHostQuicksave()
    {
        auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
        if (!mgr) {
            PLOG_WARN("[PULSAR] WorldSync: TriggerHostQuicksave - BGSSaveLoadManager not available"sv);
            return false;
        }
        // queuedTasks at +0x50, bit 0x8 = kQuickSave (verified in Ghidra FUN_141852ab0)
        auto* tasks = reinterpret_cast<volatile LONG*>(
            reinterpret_cast<uintptr_t>(mgr) + 0x50);
        InterlockedOr(tasks, 0x8);
        PLOG_INFO("[PULSAR] WorldSync: host quicksave triggered (bit 0x8 set)"sv);
        return true;
    }

    // L.7 combined AutoSave + QuickSave bits. Used by Landing tracker
    // because pure bit-0x8 silent-fails in post-landing seated state (engine
    // clears the bit without writing a file). Bit 0x1 (kAutoSave) is the
    // path Bethesda's own disembark-autosave uses, which works in this state.
    // Setting both lets whichever path is unlocked actually run.
    // L.7 call FUN_141677fe0 (Papyrus `Spaceship.ExitShip` native impl).
    // Verified Ghidra 2026-05-06: 95-byte fn, 0 args. Fires
    // `TakeoffMenu_ExitShip` event via FUN_14167b1f0 → FUN_1422cb8f0 (Notify),
    // then sets DAT_145edbe75 = 1. The engine's vanilla sinks for that event
    // perform the full disembark sequence (clear pilot binding, exit chair,
    // restore HUD state).
    //
    // SEH-guarded; helper kept in its own namespace because REL::Relocation
    // has a non-trivial destructor and __try forbids object-unwinding
    // locals (C2712).
    namespace exit_ship_detail {
        using ExitShipFn = std::uint64_t (*)();
        static std::uint64_t CallExitShipImpl()
        {
            static REL::Relocation<ExitShipFn> fn{ REL::Offset{ 0x1675C60 } };
            return fn();
        }
        static std::uint64_t CallExitShipSafe()
        {
            __try { return CallExitShipImpl(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }
    }
    bool TriggerExitShip()
    {
        const std::uint64_t r = exit_ship_detail::CallExitShipSafe();
        PLOG_INFO("[PULSAR] [L.7] TriggerExitShip fired (returned {})"sv, r);
        return r != 0;
    }

    // L.7 Path A.4 - engine's full pilot-binding release.
    // FUN_141aeb6f0 ("ReleasePilotIfInVehicle") - checks actor+0x37c bit 4,
    // fetches pilot binding context via FUN_142119780, then calls the
    // 673-byte release fn FUN_14211cbf0 which performs:
    // BGSActionData fire (unbinds input action map)
    // ship-control input release (the part that lets the player
    //     keep steering the ship even after furn/seat are cleared)
    // HUD/menu close cascade
    // Verified via Ghidra _PilotReleaseFn_decomp.txt.
    //
    // CRITICAL: must be called BEFORE we clear actor+0x37c bit 4 - the
    // wrapper at FUN_141aeb6f0 gates on that bit. Once cleared, the
    // wrapper short-circuits and ship-controls stay bound.
    namespace pilot_release_detail {
        using PilotReleaseFn = std::uint64_t (*)(void* unused, void* actor);
        static std::uint64_t CallPilotReleaseImpl(void* actor)
        {
            static REL::Relocation<PilotReleaseFn> fn{ REL::Offset{ 0xAEB190 } };
            return fn(nullptr, actor);
        }
        static std::uint64_t CallPilotReleaseSafe(void* actor)
        {
            __try { return CallPilotReleaseImpl(actor); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }
    }
    bool TriggerEnginePilotRelease()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) {
            PLOG_WARN("[PULSAR] [L.7] TriggerEnginePilotRelease - PC null"sv);
            return false;
        }
        const std::uint64_t r =
            pilot_release_detail::CallPilotReleaseSafe(p);
        PLOG_INFO(
            "[PULSAR] [L.7] TriggerEnginePilotRelease (FUN_141aeb6f0) returned {}"sv,
            r);
        return r != 0;
    }

    bool TriggerHostAutoSave()
    {
        auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
        if (!mgr) {
            PLOG_WARN("[PULSAR] WorldSync: TriggerHostAutoSave - BGSSaveLoadManager not available"sv);
            return false;
        }
        auto* tasks = reinterpret_cast<volatile LONG*>(
            reinterpret_cast<uintptr_t>(mgr) + 0x50);
        // 0x1 = kAutoSave, 0x8 = kQuickSave - set both. FUN_141852ab0 will
        // dispatch to whichever path is currently allowed.
        InterlockedOr(tasks, 0x1 | 0x8);
        PLOG_INFO("[PULSAR] WorldSync: host autosave triggered (bits 0x1+0x8 set)"sv);
        return true;
    }

    // L.7 trigger a quickload via BGSSaveLoadManager queuedTasks bit
    // 0x10 (= kQuickLoad per BGSSaveLoad.h). The engine reads bit, dispatches
    // to its quickload path which loads the most-recent save (= most likely
    // the QuickSave we trigger right before this).
    //
    // Use case: post-bridge save-load on client leaves the engine UI/control
    // layer in pilot mode even after Path A clears actor bits and Path B
    // patches the save. A full save-load cycle on the client (not the
    // engine's bridge load - a fresh quicksave + quickload) resets the
    // input/HUD state machines completely.
    bool TriggerQuickLoad()
    {
        auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
        if (!mgr) {
            PLOG_WARN("[PULSAR] WorldSync: TriggerQuickLoad - BGSSaveLoadManager not available"sv);
            return false;
        }
        auto* tasks = reinterpret_cast<volatile LONG*>(
            reinterpret_cast<uintptr_t>(mgr) + 0x50);
        InterlockedOr(tasks, 0x10);
        PLOG_INFO("[PULSAR] WorldSync: quickload triggered (bit 0x10 set)"sv);
        return true;
    }
    bool TriggerQuickSave()
    {
        auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
        if (!mgr) {
            PLOG_WARN("[PULSAR] WorldSync: TriggerQuickSave - BGSSaveLoadManager not available"sv);
            return false;
        }
        auto* tasks = reinterpret_cast<volatile LONG*>(
            reinterpret_cast<uintptr_t>(mgr) + 0x50);
        InterlockedOr(tasks, 0x8);
        PLOG_INFO("[PULSAR] WorldSync: quicksave triggered (bit 0x8 set)"sv);
        return true;
    }

    // Trigger a regular (non-quick) save with engine-generated filename.
    // Bit 0x1000 in queuedTasks (verified Ghidra FUN_141852ab0):
    //   FUN_141851790(mgr, buf)            // engine generates next save name
    //   FUN_14183ed20(mgr, buf, -1, 0)     // do save with type=0
    // CRASH-DANGER: bit 0x1000 actually triggers UI list-rebuild
    // (kBuildSaveGameList in CommonLibSF) - when set right after a
    // load complete, host teleports to cell 0x0 and gets stuck.
    // Use TriggerHostCustomSaveDirect instead.
    bool TriggerHostFullSave()
    {
        auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
        if (!mgr) {
            PLOG_WARN("[PULSAR] WorldSync: TriggerHostFullSave - BGSSaveLoadManager not available"sv);
            return false;
        }
        auto* tasks = reinterpret_cast<volatile LONG*>(
            reinterpret_cast<uintptr_t>(mgr) + 0x50);
        InterlockedOr(tasks, 0x1000);
        PLOG_INFO("[PULSAR] WorldSync: host full-save triggered (bit 0x1000 set)"sv);
        return true;
    }

    // Direct call into the save/load dispatcher (REL::ID 98640).
    // Signature: void(mgr, filename, deviceID, mode)
    // mode = 0 → load
    // mode = 1+ → save (with type)
    // deviceID = 0xffffffff → no device (PC default)
    // Verified Ghidra: FUN_141843BF0 wraps FUN_1418436E0 (real impl)
    // which queues a BSService cmd then waits for completion.
    // CRITICAL: caller must ensure game is in stable state (no loading
    // menu, player in real cell, not in proc-gen mid-stream).
    using SaveLoadDispatch_t = void(*)(void* mgr, const char* filename,
                                        uint32_t deviceID, uint32_t mode);
    static REL::Relocation<SaveLoadDispatch_t> s_SaveLoadDispatch{ REL::ID(98640) };

    bool TriggerHostCustomSaveDirect(const char* name)
    {
        if (!name || !*name) return false;
        auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
        if (!mgr) {
            PLOG_WARN("[PULSAR] WorldSync: TriggerHostCustomSaveDirect - BGSSaveLoadManager not available"sv);
            return false;
        }
        PLOG_INFO("[PULSAR] WorldSync: TriggerHostCustomSaveDirect name='{}' (calling REL::ID 98640 mode=1)"sv, name);
        s_SaveLoadDispatch(mgr, name, 0xffffffff, 1);
        PLOG_INFO("[PULSAR] WorldSync: TriggerHostCustomSaveDirect returned"sv);
        return true;
    }

    // Bypasses the FUN_141843BF0 wrapper (which has a swi(3) crash trap
    // when DAT_145e794dc>1 && DAT_145f4a4a0==1 - the "-relaunch" branch).
    // Calls FUN_1418436E0 directly (REL::ID 98638) - the "real" save
    // implementation that builds a BSService cmd + waits for completion.
    // Same signature as the wrapper.
    using SaveLoadImpl_t = void(*)(void* mgr, const char* filename,
                                    uint32_t deviceID, uint32_t mode);
    static REL::Relocation<SaveLoadImpl_t> s_SaveLoadImpl{ REL::ID(98638) };

    bool TriggerHostSaveBypass(const char* name)
    {
        if (!name || !*name) return false;
        auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
        if (!mgr) {
            PLOG_WARN("[PULSAR] WorldSync: TriggerHostSaveBypass - BGSSaveLoadManager not available"sv);
            return false;
        }
        // Sample what's normally in mgr+0x948 to find a known-good mode.
        uint32_t curMode = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(mgr) + 0x948);
        PLOG_INFO("[PULSAR] WorldSync: TriggerHostSaveBypass name='{}' (REL::ID 98638) mgr+0x948={}"sv,
                  name, curMode);

        // Try multiple mode values + log return. Mode 0 = load. Try 2/3 for save.
        for (uint32_t mode : {2u, 3u, 4u, 5u}) {
            PLOG_INFO("[PULSAR] WorldSync: bypass try mode={}"sv, mode);
            s_SaveLoadImpl(mgr, name, 0xffffffff, mode);
            PLOG_INFO("[PULSAR] WorldSync: bypass mode={} returned"sv, mode);
        }
        return true;
    }

    void OnSaveReceived(const uint8_t* a_data, size_t a_length)
    {
        // Packet format: [uint8 type][uint32 totalSize][raw .sfs data]
        if (a_length < 5) {
            PLOG_WARN("[PULSAR] WorldSync: save packet too small ({} bytes)"sv, a_length);
            return;
        }

        uint32_t totalSize = 0;
        std::memcpy(&totalSize, a_data + 1, 4);

        size_t dataOffset = 5;
        size_t dataSize = a_length - dataOffset;

        if (dataSize < totalSize) {
            PLOG_WARN("[PULSAR] WorldSync: incomplete save data ({}/{} bytes)"sv, dataSize, totalSize);
        }

        PLOG_INFO("[PULSAR] WorldSync: received save ({} bytes)"sv, dataSize);
        NASLOG("WorldSync: received save (%zu bytes)", dataSize);

        {
            std::lock_guard lock(g_saveMutex);
            g_saveBuffer.assign(a_data + dataOffset, a_data + dataOffset + dataSize);
        }

        g_state = State::SAVE_RECEIVED;

        // L.7 capture local player's pre-load state BEFORE Suspend.
        // After Suspend the engine begins tearing down actors and parent
        // cell pointers, so this must run while the world is still live.
        CapturePreLoadSnapshot();

        // L.7 V3 - arm pre-bridge-snapshot flag BEFORE Suspend so the
        // engine chunk-reader hook drops player BFCB during the upcoming
        // bridge load. Fires for EVERY save receipt (initial bridge,
        // F8-from-host, planet landing, etc.) - TickPreBridgeSnapshot only
        // covers the very first bridge of an MP session.
        Pulsar::InventorySync::ArmForBridge();

        // L.7 Plan B - capture current inventory while the world is
        // still live (pre-Suspend). Same execution context as the
        // CapturePreLoadSnapshot call above. Covers F8 / GravJump / planet
        // landing bridges where TickPreBridgeSnapshot would not fire
        // (state goes LOADED → SAVE_RECEIVED without WAITING_FOR_SAVE).
        Pulsar::InventorySync::DirectRestore::CaptureClientState();

        // V3.3 - freeze the cache from this point until RestoreClientState
        // completes. The bridge load below will replace the player's live
        // inventory with host's; without the lock, the periodic capture
        // (Main.cpp game-thread tick) fires every 5s and overwrites the
        // cache with host's items long before restore runs → cache==current
        // → restore is no-op (observed V3.2 logs: second F8 had cached=23
        // curBefore=23, zeroedEntries=0).
        Pulsar::InventorySync::DirectRestore::LockCache();

        // SUSPEND all PULSAR hooks BEFORE save-load wipes engine state.
        // The actor pointers our hooks/maps hold will become stale across
        // save-load. Without suspend, hooks fire on freed memory → UAF.
        // Resume happens automatically once we reach LOADED + settle window.
        Pulsar::HookGuard::Suspend();

        // Reset NPCSync state immediately on save receipt - clears all stale
        // remote-actor pointers from the pre-save session.
        Pulsar::NPCSync::Reset();
        // L.7 V13g - same for VehicleSync. Without this, s_seatedByCloneID
        // keeps stale FURN pointers across the save-load and post-load
        // ticks call SafeSetMotionType on freed memory → pure-virtual-call
        // crash (observed 2026-05-04 V13f.1 test).
        Pulsar::VehicleSync::Reset();
        // L.7 V13g - RemotePlayerManager caches RE::Actor* m_remoteActor +
        // RE::TESObjectREFR* m_lookMarker pointing at engine objects that
        // get destroyed during the save-load. CellDiag's snapshot tick
        // and other readers go through GetRemoteActor() and read freed
        // memory → ACCESS VIOLATION post-load (V13g 2nd-jump test).
        // FindSpawnedActor will re-resolve them after the save loaded.
        Pulsar::RemotePlayer::GetRemotePlayerManager().ClearStaleClonePtrs();
        // Same for FlightSync's cached ship / NPC-ship Havok bodies, which the
        // tick body-commit writes through.
        Pulsar::FlightSync::ResetBodyCache();
    }

    // ─── L.7 Pre-load snapshot implementation ────────────────────

    void CapturePreLoadSnapshot()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            std::lock_guard lock(g_preLoadSnapMutex);
            g_preLoadSnap.valid = false;
            PLOG_INFO("[L.7] Snapshot skipped - player or parentCell null"sv);
            return;
        }

        PreLoadSnapshot snap{};
        snap.pos    = player->data.location;
        snap.rotZ   = player->data.angle.z;
        snap.cellID = player->parentCell->formID;
        snap.valid  = true;

        {
            std::lock_guard lock(g_preLoadSnapMutex);
            g_preLoadSnap = snap;
        }

        PLOG_INFO("[L.7] Pre-load snap: cell=0x{:08X} pos=({:.0f},{:.0f},{:.0f}) rotZ={:.2f}"sv,
                  snap.cellID, snap.pos.x, snap.pos.y, snap.pos.z, snap.rotZ);
        NASLOG("L7-288 SNAP cell=0x%08X pos=(%.0f,%.0f,%.0f) rotZ=%.2f",
               snap.cellID, snap.pos.x, snap.pos.y, snap.pos.z, snap.rotZ);
    }

    RestorePlan DetermineRestoreStrategy(
        RE::PlayerCharacter* a_player,
        const RE::NiPoint3&  a_hostPos,
        float                a_hostRotZ)
    {
        // Default: offset behind host. Always safe - covers first-connect
        // (snap-invalid: client at main-menu / solo-save), cell-mismatch
        // (client was on planet while host took off), and any other "we
        // don't have a same-cell match" case. Avoids chair-overlap when
        // host is in pilot seat.
        // rotZ in radians, 0 = +Y north; forward=(sin,cos), behind=-(sin,cos).
        constexpr float kBehindDist = 1.5f;
        RestorePlan plan{};
        plan.mode  = RestoreMode::kOffsetBehindHost;
        plan.pos.x = a_hostPos.x - std::sin(a_hostRotZ) * kBehindDist;
        plan.pos.y = a_hostPos.y - std::cos(a_hostRotZ) * kBehindDist;
        plan.pos.z = a_hostPos.z;
        plan.rotZ  = a_hostRotZ;  // face same direction as host

        if (!a_player) return plan;

        std::uint32_t newCellID = a_player->parentCell
            ? a_player->parentCell->formID
            : 0u;

        // L.7 first-connect-bridge fallback. Pre-load snapshot is
        // missing on the very first bridge after connect (player had no
        // parentCell when the snap-capture ran), so the legacy code path
        // fell through to kOffsetBehindHost → 1.5m teleport → engine
        // pilot-binding state corruption → "stuck behind host" symptom
        // (verified 2026-05-07 21:26:09 logs). Cure: even without a
        // pre-load snap, if the engine's load placed us in the same cell
        // that host is currently broadcasting from, skip teleport - both
        // ended up in the same ship interior, same-chair overlap risk
        // applies, user holds E to unseat just like the second-bridge
        // path. Outdoor-cell case is also benign: no pilot binding, no
        // chair → user just lands on host pos and walks.
        std::uint32_t hostCellID =
            Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteCellID();

        PreLoadSnapshot snap;
        {
            std::lock_guard lock(g_preLoadSnapMutex);
            snap = g_preLoadSnap;
        }

        // Same persistent cell (pre↔post OR post↔host-broadcast) → DON'T
        // teleport. Engine's load already placed the client on host's
        // saved pos (typically host's pilot chair). User unseats manually
        // by holding E for ~2s - engine's own press-and-hold unseat
        // handler runs the proper anim + state-machine transition
        // (verified L.7, 2026-05-07). Earlier SAME-CELL-RESTORE
        // teleport (snap.pos) corrupted the engine UI/input layer when
        // fired while the actor was still in pilot-sit pose: cockpit HUD
        // stayed visible, WASD steered the ship.
        // M.3 - when client just loaded their own solo save before the
        // MP-bridge (Handshake-gate flow), the pre-load snapshot represents
        // the solo cell. The freshSoloLoad flag tells us this state.
        const bool freshSoloLoad =
            Pulsar::InventorySync::IsPreBridgeSnapshotPending();
        const bool snapMatch = snap.valid && newCellID != 0 &&
                               newCellID == snap.cellID;
        const bool hostMatch = newCellID != 0 && hostCellID != 0 &&
                               newCellID == hostCellID;

        // L.7 both peers report the same cell. The kNone "engine load
        // placed us correctly (on host's pos / pilot chair)" assumption ONLY
        // holds if we actually ended up NEAR host. Fast-travel to a proc planet
        // drops the client at the cell's DEFAULT spawn (far from host's landing)
        // while still reporting the same (proc-equivalent) cell - skipping there
        // strands the client at the cell edge (verified 2026-06-20: ~32m apart,
        // host pos (-517,429,6) vs client (-539,405,8)). So validate by distance:
        // skip only when genuinely co-located; otherwise teleport to host.
        if (hostMatch || (snapMatch && !freshSoloLoad)) {
            const RE::NiPoint3& pp = a_player->data.location;
            const float dxc = pp.x - a_hostPos.x;
            const float dyc = pp.y - a_hostPos.y;
            const float dzc = pp.z - a_hostPos.z;
            const float dist = std::sqrt(dxc * dxc + dyc * dyc + dzc * dzc);
            constexpr float kCoLocatedMax = 15.0f;  // metres (ship interior / chair spawn)
            if (dist <= kCoLocatedMax) {
                plan.mode = RestoreMode::kNone;
                PLOG_INFO(
                    "[L.7] Same-cell + co-located (dist {:.1f}m, path={}) - "
                    "skip teleport. User holds E ~2s to unseat manually."sv,
                    dist, hostMatch ? "host-broadcast" : "snap");
                NASLOG("L7-299 SAME-CELL skip cell=0x%08X dist=%.1f", newCellID, dist);
                return plan;
            }
            // Same cell but FAR from host → engine didn't co-locate us (proc default
            // spawn / cell edge). Teleport to host via the default kOffsetBehindHost.
            PLOG_INFO(
                "[L.7] Same-cell BUT far from host (dist {:.1f}m, path={}) - "
                "engine didn't co-locate; teleport to host (kOffsetBehindHost)."sv,
                dist, hostMatch ? "host-broadcast" : "snap");
            NASLOG("L7-299 SAME-CELL-FAR teleport cell=0x%08X dist=%.1f", newCellID, dist);
            return plan;  // default plan.mode = kOffsetBehindHost (set at top)
        }

        // L.7 bridge-load-failed detector. freshSoloLoad means
        // client loaded their solo save and we then bridged host's save
        // bytes. If after the bridge the cell is STILL the solo cell
        // (snapMatch) and the host is broadcasting from a DIFFERENT cell
        // (!hostMatch), the engine save-load did not switch cells -
        // typically because Phase B 10s timeout force-completed Phase C
        // without the engine actually replaying the file (see L.7 V13k).
        // In that state the engine has no notion of host's cell loaded.
        // Doing kOffsetBehindHost would SetPosition the player to host's
        // X/Y but still inside the solo (ship-interior) cell - outside
        // its floor mesh → ActionFall and Z drifts to -infinity (verified
        // 2026-05-11 21:14 logs). Suppress teleport. Player stays in
        // solo cell, items synced via M.3 .psprof apply, host's world
        // state does not transfer.
        if (freshSoloLoad && snapMatch && !hostMatch && hostCellID != 0) {
            // L.7 engine bridge load completes but doesn't reposition
            // player (state byte 0x84 path, no PerformGameReset). Engine RE
            // pass identified: PerformGameReset (FUN_141898AB0) only runs on
            // state 0x85, which bridge load never sets. Rather than invoke
            // that function ourselves (high crash risk - destroys + recreates
            // player REFR, all PULSAR caches go UAF), do a manual TP:
            //   1. Resolve hostCellID -> TESObjectCELL via TESForm::LookupByID
            //   2. Pin hostClone->parentCell = hostCell  (existing pattern)
            //   3. EngineMoveTo(player, hostClone) - cell-streamer aware
            //   4. SetPosition(player, hostPos) - exact host coords
            // Main.cpp dispatcher executes steps 2-4. If cell lookup fails
            // (host cell not in client's plugin list), it falls back to kNone.
            //
            // Proc-cell exception (FF-prefix): per-client-instanced cells.
            // TESForm::LookupByID returns NULL because client doesn't have
            // host's instance. For these, engine bridge's GravJump path
            // naturally cell-switches into client's own proc-instance (see
            // log 22:23:08 cell-change to 0xFF07451B parallel to host's
            // 0xFF019640). Skip detector - fall through to default
            // kOffsetBehindHost on the OWN proc-instance.
            const bool isProcCell =
                (hostCellID & 0xFF000000u) == 0xFF000000u;
            if (isProcCell) {
                PLOG_INFO(
                    "[L.7] Host cell 0x{:08X} is proc - skip manual TP "
                    "(engine bridge cross-cells via GravJump path on its own; "
                    "client lands in own proc-instance, see OFFSET-BEHIND-HOST)."sv,
                    hostCellID);
                NASLOG("L7-302 SKIP proc-cell host=0x%08X", hostCellID);
                // Fall through - keep computed kOffsetBehindHost default.
                return plan;
            }

            plan.mode         = RestoreMode::kBridgeFailedManualTP;
            plan.pos          = a_hostPos;
            plan.rotZ         = a_hostRotZ;
            plan.targetCellID = hostCellID;
            PLOG_INFO(
                "[L.7] Bridge load did not switch cells "
                "(solo=0x{:08X} host=0x{:08X}) - manual TP via "
                "EngineMoveTo+SetPosition queued."sv,
                newCellID, hostCellID);
            NASLOG("L7-302 BRIDGE-FAILED manual-TP (local=0x%08X host=0x%08X)",
                   newCellID, hostCellID);
            return plan;
        }

        // FUTURE (dual-ship) - when host+client own different ships and the
        // non-pilot's pre-load cell is their OWN ship interior, MoveTo that
        // cell and restore pos. Requires:
        // module that registers per-player ship cells
        // engine MoveTo helper (RVA 0x14191EFB0, cell-streamer aware)
        // Today the predicate is unreachable; cell-mismatch keeps the
        // already-computed kOffsetBehindHost default.
        //
        // if (OwnShip::IsOwnShipInterior(snap.cellID) &&
        //     OwnShip::IsForeignShipInterior(newCellID)) {
        //     plan.mode = RestoreMode::kOwnShipMoveTo;
        //     plan.pos  = snap.pos;
        //     plan.rotZ = snap.rotZ;
        //     // plan.targetCell = OwnShip::ResolveCell(snap.cellID);
        //     return plan;
        // }

        // Snap valid but cell differs → keep kOffsetBehindHost default
        // computed at function entry. Typical: client on planet, host
        // pilots into space.
        return plan;
    }

    void ConsumePreLoadSnapshot()
    {
        std::lock_guard lock(g_preLoadSnapMutex);
        g_preLoadSnap.valid = false;
    }

    void ProcessPending()
    {
        State state = g_state.load();

        if (state == State::SAVE_RECEIVED) {
            // Write save to disk
            std::string savesDir = SaveScanner::GetSavesDirectory();
            if (savesDir.empty()) {
                PLOG_WARN("[PULSAR] WorldSync: can't find saves directory"sv);
                g_state = State::IDLE;
                return;
            }

            // L.7 V13j - rotating filename. Each save gets PULSAR_Session_NNN
            // so the engine doesn't reuse cached parse state from the
            // previous load (counter clear in V13i alone wasn't enough -
            // FUN_1418436E0 still failed on 2nd load with same filename).
            g_bridgeRetryCount = 0;  // V13n - fresh bridge, reset retry budget
            g_loadDeferCount = 0;    // L.7 D2 - fresh bridge, reset load-safe deferrals

            // L.7 V13o - robust save write. The Saves dir can live under
            // OneDrive (observed %USERPROFILE%\OneDrive\...\Saves), which locks
            // recently-synced files. The per-session sequence resets to 0, so
            // PULSAR_Session_001.sfs from a prior session is often still
            // OneDrive-locked → ofstream open fails → the WHOLE bridge aborts
            // (state IDLE, HookGuard stuck suspended, no LOAD_COMPLETE, host
            // never spawns the client clone - observed 2026-05-24 23:14:53
            // "can't write ...PULSAR_Session_001.sfs", host didn't see client).
            // Fix: skip locked names - try successive sequence numbers until
            // one opens. A not-yet-existing name can't be OneDrive-locked.
            std::ofstream file;
            std::string   savePath;
            std::string   baseName;
            {
                std::lock_guard lock(g_saveMutex);
                for (int attempt = 0; attempt < 24 && !file.is_open(); ++attempt) {
                    file.clear();  // reset failbit from a prior failed open
                    const uint32_t seq = ++g_saveSequence;
                    char nameBuf[64];
                    ::sprintf_s(nameBuf, sizeof(nameBuf),
                        "PULSAR_Session_%03u", seq);
                    std::string tryPath = savesDir + "\\" + nameBuf + ".sfs";
                    file.open(tryPath,
                        std::ios::binary | std::ios::out | std::ios::trunc);
                    if (file.is_open()) {
                        baseName = nameBuf;
                        savePath = tryPath;
                    } else if (attempt < 4) {
                        PLOG_INFO(
                            "[PULSAR] [L.7 V13o] save name {} locked "
                            "(OneDrive sync?), trying next"sv, tryPath);
                    }
                }
                if (!file.is_open()) {
                    PLOG_WARN(
                        "[PULSAR] WorldSync: can't write any PULSAR_Session_NNN "
                        "in {} (all locked?) - aborting bridge + resuming hooks"sv,
                        savesDir);
                    Pulsar::HookGuard::Resume();  // don't leave hooks frozen
                    g_state = State::IDLE;
                    return;
                }
                g_lastSaveBaseName = baseName;
                file.write(reinterpret_cast<const char*>(g_saveBuffer.data()),
                    g_saveBuffer.size());
                file.close();
                g_saveBuffer.clear();
            }

            PLOG_INFO("[PULSAR] WorldSync: wrote {} ({} bytes) [V13o]"sv,
                savePath, std::filesystem::file_size(savePath));
            NASLOG("WorldSync: wrote save %s", baseName.c_str());

            // Trigger load via Path C:
            // 1. SetMostRecentSave sets [mgr+0x70] = "PULSAR_Session" and [mgr+0x78] = 0
            // 2. Setting bit 0x4 in queuedTasks[+0x50] tells the queue processor to load it
            // (verified from Starfield.exe decompilation: bit 0x4 reads [mgr+0x70] as filename)
            auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
            if (!mgr) {
                PLOG_WARN("[PULSAR] WorldSync: BGSSaveLoadManager not available"sv);
                g_state = State::IDLE;
                return;
            }

            // Extend ENet timeout to 120s so the connection survives the loading screen
            Pulsar::Network::GetSingleton().SetPeerTimeout(120000);
            PLOG_INFO("[PULSAR] WorldSync: ENet timeout extended to 120s for loading"sv);

            // Short delay to let ENet settle, then queue the load.
            // IMPORTANT: keep this short - any longer and the user might manually
            // load their own save, which puts the game in-world and triggers the
            // E-key "cancel load" confirmation dialog that blocks our auto-load.
            g_triggerAfter = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            g_state = State::PENDING_TRIGGER;
            PLOG_INFO("[PULSAR] WorldSync: save written, queuing load in 2s..."sv);
            NASLOG("WorldSync: save written, queuing load in 2s");
        }

        // L.7 V11.1 - V11 routed 2nd+ loads through TriggerEngineQuitToMainMenu,
        // but the post-main-menu bit-0x4 load trigger doesn't fire at main
        // menu state (queue processor inactive or different code path needed
        // for "Continue" button). 1st jump regressed to cell=0x00000000.
        // Reverted: all loads go direct, save-bridge limited via 1-fire-per-
        // session (V8d) until a working post-main-menu load trigger is found.
        static int s_loadCount = 0;

        // Wait until game is at main menu, then trigger load
        if (state == State::PENDING_TRIGGER) {
            if (std::chrono::steady_clock::now() >= g_triggerAfter) {
                // L.7 D2 - don't force the engine-load while the client is
                // seated in a ship / piloting / a load is already underway. That
                // crashes the engine on the load path (null lock-acquire - the
                // 2026-05-25 takeoff CTD). On-foot planet joins are safe and
                // pass first try. If the player is flying it won't become
                // load-safe until it lands, so defer only briefly (~8s, hooks
                // are suspended this whole window) then abort the bridge rather
                // than hang. A later F8 / re-event re-bridges once on foot.
                if (!detail_::IsClientSafeToLoad()) {
                    if (++g_loadDeferCount <= 16) {
                        if (g_loadDeferCount == 1 || (g_loadDeferCount % 4) == 0) {
                            PLOG_INFO(
                                "[PULSAR] [L.7 D2] client not load-safe "
                                "(seated/in-space/loadMenu) - deferring bridge-load "
                                "(defer #{}/16)"sv, g_loadDeferCount);
                        }
                        g_triggerAfter = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(500);
                        return;  // stay in PENDING_TRIGGER, re-check next tick
                    }
                    // L.7 D2.3 (, 2026-06-24) - D2.2 ABORT REVERTED.
                    // D2.2 aborted the load when the client was seated/in-space, to
                    // dodge the null-lock takeoff CTD (Ghidra 0x1422CC350). But that
                    // STRANDED an in-space client: every bridge aborted, so it could
                    // never co-locate to the host (observed 2026-06-24: host sent a
                    // fresh 2.1 MB save, client in-space → never loaded). The CTD is
                    // now neutralised at the ENGINE level by CrashGuard Guard #5
                    // (shields the slim-RW lock FUN_1422ce9b0 against null/dangling
                    // lock pointers - the actual fault site), so it is safe to
                    // FORCE-PROCEED again from any unsafe state. Force-loading in
                    // space no longer crashes; the load co-locates the client.
                    PLOG_WARN(
                        "[PULSAR] [L.7 D2.3] client not load-safe after {} "
                        "deferrals - FORCE-PROCEEDING (in-space CTD shielded by "
                        "CrashGuard Guard #5)"sv,
                        g_loadDeferCount);
                    NASLOG("D2.3 force-proceed bridge after defer window");
                }
                g_loadDeferCount = 0;

                auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
                if (!mgr) {
                    PLOG_WARN("[PULSAR] WorldSync: BGSSaveLoadManager not available"sv);
                    g_state = State::IDLE;
                    return;
                }

                // Reset glob-resolved flag so we can detect re-resolution after the load finishes
                RemotePlayer::GetRemotePlayerManager().ResetGlobsResolved();

                // L.7 V13i - counter-only clear (V12 minus guard+flag).
                //
                // V13e Ghidra found the smoking gun: FUN_141843BF0 (called
                // when bit-0x4 fires) does this:
                //
                //   iVar3 = DAT_145e794dc;     // the counter
                //   if (1 < iVar3 && DAT_145f4a4a0 == 1) {
                //       // BUILT-IN PROCESS-RESTART path -
                //       FUN_141858fa0(local_1008);    // relaunch with -relaunch
                //       swi(3);                        // int3 breakpoint
                //       return;
                //   }
                //   FUN_1418436E0(param_1, param_2, param_3, param_4); // safe load
                //
                // Bethesda's engine intentionally bails out in-process load
                // on the 2nd attempt and tries to relaunch the .exe. In our
                // hooked context that path either int3's silently or falls
                // through to a no-op leaving cell=0.
                //
                // V13a proved engine handles "stuck state" (guard=0x01,
                // slots, p113c=0x01) fine - these don't matter. So we ONLY
                // need to keep the counter ≤ 1 to force the safe-load
                // branch. Don't touch guard byte or player flag.
                {
                    auto counterAddr = REL::Offset(0x5E6D6DCULL).address();
                    if (counterAddr) {
                        std::uint32_t prevCnt =
                            *reinterpret_cast<volatile std::uint32_t*>(counterAddr);
                        *reinterpret_cast<volatile std::uint32_t*>(counterAddr) = 0;
                        PLOG_INFO(
                            "[PULSAR] [L.7 V13i] cleared FUN_141843BF0 "
                            "relaunch-counter (RVA 0x5E6D6DC: {} → 0) - "
                            "forces engine into safe in-process load branch"sv,
                            prevCnt);
                        NASLOG("L.7 V13i counter cleared (relaunch trigger)");
                    } else {
                        PLOG_WARN(
                            "[PULSAR] [L.7 V13i] counter addr unresolved "
                            "- relaunch-trigger may fire"sv);
                    }
                }

                // L.7 V13j - load with the rotating filename written above.
                const std::string& loadName = g_lastSaveBaseName.empty()
                    ? std::string("PULSAR_Session")
                    : g_lastSaveBaseName;
                s_SetMostRecentSave(mgr, loadName.c_str(), 0);
                PLOG_INFO(
                    "[PULSAR] [L.7 V13j] SetMostRecentSave('{}', 0)"sv,
                    loadName);

                // L.7 V13b: snapshot engine state immediately AFTER V12
                // clear and BEFORE bit-0x4. That snapshot vs the one the
                // FUN_14189A8E0 hook captures on entry tells us if the
                // queue processor mutated anything between us and the
                // pre-load init handler.
                Pulsar::WorldSync::StateProbe::DumpManual("PRE-BIT4");

                // L.7 V13f - arm the load-start gate. We must NOT declare
                // LOADED until the engine has actually started loading.
                // Earlier versions hit a race: globsReady && parentCell were
                // both true 1ms after we set bit-0x4 (engine queue
                // processor hadn't run yet), so we declared LOADED, tore
                // down clones, sent LOAD_COMPLETE, and the engine's load
                // then failed in cell=0 fallback. Snapshot the entry-hook
                // count NOW; in LOADING we'll wait for it to advance.
                g_loadStartObserved.store(false);
                g_cellGoneObserved.store(false);
                g_loadMenuOpenedSeen.store(false);
                g_entryHookCountAtArm = StateProbe::GetEntryHookCount();
                g_loadStartDeadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(15);

                auto* tasks = reinterpret_cast<volatile uint32_t*>(
                    reinterpret_cast<uintptr_t>(mgr) + 0x50);
                *tasks |= 0x4;

                // Safety timeout: if globs never resolve in 60s, give up and declare LOADED anyway
                g_loadedAfter = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                g_state = State::LOADING;
                PLOG_INFO("[PULSAR] WorldSync: queued load of PULSAR_Session, waiting for GLOBs..."sv);
                NASLOG("WorldSync: queued load of PULSAR_Session");
            }
        }

        // Detect when loading is done:
        //  1. PULSAR.esp globs resolved → esp is active in memory
        //  2. Player has a parentCell → game is actually in-world (not main-menu cached forms)
        // Globs alone give false positives: the main-menu background scene or a stale
        // klomann save can keep PULSAR.esp forms resolved even when our load was cancelled
        // by the E-key confirmation dialog.
        if (state == State::LOADING) {
            // L.7 V13f - Phase A: gate completion check on a load-start
            // signal. Without this gate, globsReady && inWorld (which were
            // BOTH true the instant we set bit-0x4) caused premature LOADED
            // declaration → race torched the engine load. We observe ANY of:
            // StateProbe entry-hook count advanced past arm point
            //     (FUN_14189A8E0 fired = engine pre-load init ran)
            // mgrTasks bit 0x4 cleared (queue processor consumed task)
            // parentCell == nullptr (engine destroyed player or cell)
            // 15s start-timeout (safety fallback)
            if (!g_loadStartObserved.load()) {
                auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
                auto curTaskBits = mgr ? *reinterpret_cast<volatile uint32_t*>(
                    reinterpret_cast<uintptr_t>(mgr) + 0x50) : 0u;
                bool taskBitCleared = mgr && !(curTaskBits & 0x4u);
                auto* pPre = RE::PlayerCharacter::GetSingleton();
                bool cellGone = (!pPre || pPre->parentCell == nullptr);
                bool initFired =
                    StateProbe::GetEntryHookCount() > g_entryHookCountAtArm;
                bool startTimeout =
                    std::chrono::steady_clock::now() >= g_loadStartDeadline;
                if (initFired || taskBitCleared || cellGone || startTimeout) {
                    PLOG_INFO(
                        "[PULSAR] [L.7 V13f] load-start observed: "
                        "initFired={} taskBitCleared={} cellGone={} "
                        "startTimeout={}"sv,
                        initFired, taskBitCleared, cellGone, startTimeout);
                    NASLOG("L.7 V13f load-start observed");
                    g_loadStartObserved.store(true);
                    if (cellGone) g_cellGoneObserved.store(true);
                }
                // Phase A still active - do NOT fall through to completion
                // check this tick. globsReady/inWorld may both be true
                // because the engine hasn't actually started loading yet.
                return;
            }

            // L.7 V13f.1 / V13k - Phase B: gate completion until the
            // engine has actually committed to the load. Two valid
            // signals:
            //   (a) parentCell observed null at least once (cross-cell
            //       load - engine destroys + recreates the world)
            //   (b) loadMenu OPENED then CLOSED (same-cell load -
            //       engine refreshes save state without tearing down
            //       the cell, e.g. Jump 2 from ship interior to ship
            //       interior in different system)
            //
            // V13j proved (b): Jump 2 had loadMenu OPENED 0x05100136
            // → CLOSED 0x05100136 (cell never went null). Without (b),
            // V13f.1 waited the full 60s timeout before Phase C.
            //
            // 10s timeout for Phase B (was 60s - way too long for UX).
            if (!g_cellGoneObserved.load()) {
                auto* pPhB = RE::PlayerCharacter::GetSingleton();
                bool cellGoneNow = (!pPhB || pPhB->parentCell == nullptr);

                // Track loadMenu OPENED → CLOSED transition.
                bool loadMenuOpenNow = detail_::IsLoadMenuOpenSafe();
                if (loadMenuOpenNow) g_loadMenuOpenedSeen.store(true);
                bool loadMenuClosedAfterOpen =
                    g_loadMenuOpenedSeen.load() && !loadMenuOpenNow;

                static auto s_phaseBStart =
                    std::chrono::steady_clock::time_point::min();
                static bool s_v13mFlashLogged = false;
                if (s_phaseBStart == std::chrono::steady_clock::time_point::min()) {
                    s_phaseBStart = std::chrono::steady_clock::now();
                    s_v13mFlashLogged = false;
                }
                bool phaseBTimeout =
                    std::chrono::steady_clock::now() - s_phaseBStart >
                    std::chrono::seconds(10);

                // L.7 V13m - only accept the loadMenu OPENED→CLOSED shortcut
                // for a TRUE same-cell load (host target cell == our pre-load
                // cell). For a cross-cell load - different cells, or host in a
                // proc cell whose per-instance FF-ID can never equal ours - a
                // brief loadMenu flash can fire BEFORE the engine's real
                // teardown. Accepting it completed Phase B prematurely, the
                // engine never tore down, and the player stayed in their own
                // cell (observed 2026-05-24 F8#1: client 0x1DEA46, host
                // 0x05100136 → V13k fired → "did not switch cells"). For a
                // cross-cell load require cellGone (real teardown); the 10s
                // timeout is the final fallback.
                std::uint32_t hostCellPhB =
                    Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteCellID();
                std::uint32_t preCellPhB = 0;
                {
                    std::lock_guard lock(g_preLoadSnapMutex);
                    if (g_preLoadSnap.valid) preCellPhB = g_preLoadSnap.cellID;
                }
                const bool crossCellExpected =
                    hostCellPhB != 0 && preCellPhB != 0 &&
                    hostCellPhB != preCellPhB;

                if (cellGoneNow) {
                    PLOG_INFO(
                        "[PULSAR] [L.7 V13f.1] Phase B done: cell-gone "
                        "observed (engine torn down world)"sv);
                    g_cellGoneObserved.store(true);
                    s_phaseBStart =
                        std::chrono::steady_clock::time_point::min();
                } else if (loadMenuClosedAfterOpen && !crossCellExpected) {
                    PLOG_INFO(
                        "[PULSAR] [L.7 V13k] Phase B done: loadMenu "
                        "OPENED→CLOSED transition (same-cell load path; "
                        "host=0x{:08X} pre=0x{:08X})"sv,
                        hostCellPhB, preCellPhB);
                    NASLOG("L.7 V13k Phase B loadMenu transition");
                    g_cellGoneObserved.store(true);
                    s_phaseBStart =
                        std::chrono::steady_clock::time_point::min();
                } else if (phaseBTimeout) {
                    PLOG_WARN(
                        "[PULSAR] [L.7 V13k] Phase B 10s timeout - "
                        "forcing Phase C (load may be stuck)"sv);
                    g_cellGoneObserved.store(true);
                    s_phaseBStart =
                        std::chrono::steady_clock::time_point::min();
                } else {
                    // Still waiting. For a cross-cell load this is where we
                    // sit through the menu flash (deliberately ignored) until
                    // the real cellGone teardown or the 10s timeout. Log once.
                    if (loadMenuClosedAfterOpen && crossCellExpected &&
                        !s_v13mFlashLogged) {
                        s_v13mFlashLogged = true;
                        PLOG_INFO(
                            "[PULSAR] [L.7 V13m] loadMenu flash IGNORED - "
                            "cross-cell load expected (host=0x{:08X} != "
                            "pre=0x{:08X}); waiting for cellGone teardown"sv,
                            hostCellPhB, preCellPhB);
                    }
                    return;
                }
            }

            bool globsReady = RemotePlayer::GetRemotePlayerManager().IsGlobsResolved();
            auto* p = RE::PlayerCharacter::GetSingleton();
            bool inWorld = (p && p->parentCell != nullptr);
            bool timeoutHit = std::chrono::steady_clock::now() >= g_loadedAfter;
            if ((globsReady && inWorld) || timeoutHit) {
                if (timeoutHit && !(globsReady && inWorld)) {
                    PLOG_WARN("[PULSAR] WorldSync: 60s load timeout hit - forcing LOADED (globs={} inWorld={})"sv,
                        globsReady, inWorld);
                }

                // L.7 V13n - auto-retry on bridge non-switch (first-bridge
                // failure). If the player is still in the pre-load cell while
                // the host broadcasts from a different cell, the engine load
                // never relocated us (the initFired=false path observed
                // 2026-05-24: F8#1 fails, F8#2 works). Re-trigger the load
                // ONCE with a fresh rotated filename - reusing the old name
                // hits the engine parse-state cache and fails again (V13j).
                {
                    // V13n.1 - detect non-switch via initFired, NOT the host
                    // cell. The host cell (GetRemoteCellID) is frequently still
                    // 0 over the network at this instant - that made the
                    // original host-cell check miss the exact case it targets
                    // (the initial bridge: observed 2026-05-24 18:35:09
                    // "host=0x00000000" → no retry → "did not switch" 5s later).
                    // If the engine's pre-load init never fired (entry-hook
                    // count didn't advance past arm) the load was a no-op;
                    // combined with still being in the pre-load cell, the bridge
                    // didn't relocate us → retry. Fewer failed bridges also
                    // means fewer ForceRespawn churn cycles, which is the
                    // trigger for the refcount 3D-UAF crash 0x23686DA.
                    const bool initFiredThisLoad =
                        StateProbe::GetEntryHookCount() > g_entryHookCountAtArm;
                    std::uint32_t curCell =
                        (p && p->parentCell) ? p->parentCell->formID : 0u;
                    std::uint32_t preCell = 0u;
                    {
                        std::lock_guard lock(g_preLoadSnapMutex);
                        if (g_preLoadSnap.valid) preCell = g_preLoadSnap.cellID;
                    }
                    const bool didNotSwitch =
                        !initFiredThisLoad && curCell != 0u &&
                        curCell == preCell;
                    if (didNotSwitch && g_bridgeRetryCount < 1) {
                        ++g_bridgeRetryCount;
                        bool copied = false;
                        std::string savesDir = SaveScanner::GetSavesDirectory();
                        if (!savesDir.empty() && !g_lastSaveBaseName.empty()) {
                            std::string oldPath =
                                savesDir + "\\" + g_lastSaveBaseName + ".sfs";
                            const uint32_t rseq = ++g_saveSequence;
                            char rname[64];
                            ::sprintf_s(rname, sizeof(rname),
                                "PULSAR_Session_%03u", rseq);
                            std::string newPath = savesDir + "\\" + rname + ".sfs";
                            std::error_code ec;
                            std::filesystem::copy_file(oldPath, newPath,
                                std::filesystem::copy_options::overwrite_existing,
                                ec);
                            if (!ec) {
                                g_lastSaveBaseName = rname;
                                copied = true;
                            } else {
                                PLOG_WARN(
                                    "[PULSAR] [L.7 V13n] retry copy failed: {}"sv,
                                    ec.message());
                            }
                        }
                        if (copied) {
                            // Keep the long loading ENet timeout, re-arm
                            // PENDING_TRIGGER (re-does counter clear +
                            // SetMostRecentSave + bit-0x4 + Phase gates).
                            Pulsar::Network::GetSingleton().SetPeerTimeout(120000);
                            g_triggerAfter = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
                            g_state = State::PENDING_TRIGGER;
                            PLOG_WARN(
                                "[PULSAR] [L.7 V13n.1] bridge did NOT switch "
                                "(initFired=false, cur=0x{:08X} == pre) - "
                                "auto-retry #1 with fresh save '{}'"sv,
                                curCell, g_lastSaveBaseName);
                            NASLOG("L.7 V13n.1 auto-retry (cur=0x%08X)", curCell);
                            return;
                        }
                    }
                }

                // Restore normal ENet timeout now that we're in the world.
                // 2026-06-11: 5000 → 8000 (×2 max inside SetPeerTimeout = 16s),
                // matching the relaxed host-side OnConnect timeout - transient
                // hiccups no longer drop the link (clone-flicker fix).
                Pulsar::Network::GetSingleton().SetPeerTimeout(8000);
                // Reset teleport flag so client teleports to host's current position
                PLOG_INFO(
                    "[PULSAR] [TELE-DIAG] WorldSync LOADED → calling "
                    "ResetTeleport (post-save-load arming)"sv);
                NASLOG("TELE-DIAG WorldSync LOADED -> ResetTeleport call");
                Pulsar::Network::GetSingleton().ResetTeleport();
                // ForceRespawn: save load resets PulsarSpawn GLOB and Papyrus VM state.
                // DoDespawn clears stale state, DoSpawn re-triggers Papyrus clone spawn.
                RemotePlayer::GetRemotePlayerManager().RequestForceRespawn();

                g_state = State::LOADED;
                g_lastLoadCompletedTick.store(::GetTickCount64());
                // L.7 V11 - count completed loads. The 1st (initial connect)
                // is the "fresh" mid-game load that always works; the 2nd+
                // need QuitToMainMenu pre-step to avoid the engine bug.
                ++s_loadCount;
                PLOG_INFO("[PULSAR] [L.7 V11] load-count is now {}"sv,
                          s_loadCount);
                if (p) {
                    PLOG_INFO("[PULSAR] WorldSync: host save loaded! Player at ({:.0f},{:.0f},{:.0f})"sv,
                        p->data.location.x, p->data.location.y, p->data.location.z);
                } else {
                    PLOG_INFO("[PULSAR] WorldSync: host save loaded!"sv);
                }
                NASLOG("WorldSync: host save loaded");

                // L.6 V3 Phase 3i v17 - clear DockSync forward-bridge wait flag
                // so client's cockpit-hatch activates resume normal flow.
                Pulsar::DockSync::OnLocalSaveLoaded();

                // L.7 V8 - tell host the client is now in a stable in-world
                // state. Host gates further save-bridge fires on this ack so
                // we can't double-trigger during an in-flight load.
                Pulsar::SpaceSync::FastTravel::NotifyClientLoadComplete(
                    timeoutHit ? 2 : 1);
            }
        }
    }

}  // namespace Pulsar::WorldSync
