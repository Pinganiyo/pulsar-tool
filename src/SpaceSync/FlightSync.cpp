// Phase L.3 V2 V1 - Coop pilot flight-state sync via ShipHudDataModel.
//
// Architecture (from RE - see memory/reference_space_flight_architecture.md):
// ShipHudDataModel is constructed once when the Spaceship HUD opens
//     (FUN_141556a50, RVA 0x1556300). It's a multi-event-sink that aggregates
//     ship state into a property bag exposed to the SWF UI layer.
// Inside ShipHudDataModel, +0x1BB0 (= +0x376*8 qwords) is the
//     StickDataProvider sub-object: 24 TUIValue<T> entries, each 32 bytes,
//     value at +0x18 within the entry (u32 for float, u8 for bool).
// The engine fires Spaceship::PlayerMovementOutputEvent per pilot-frame;
//     the model's sink writes throttle/yaw-input/pitch-input/boost/reverse/
//     cruise-level into the Properties; the HUD reads from them.
//
// We hook the constructor so we get the instance pointer on every model
// allocation. While the pointer is live AND we're piloting, host reads the
// values and broadcasts them; client writes received values back into its
// own model's properties (cockpit gauges + control state mirror host).
//
// Thread model:
// Hook callback runs on the game thread (model construction is engine-thread).
// Tick() runs on game thread (called from Main.cpp tick loop).
// OnPacketReceived() runs on the network thread; it only stages the latest
//     state into an atomic-armed struct. Tick() drains and applies it.
// The instance pointer is `std::atomic<uintptr_t>`; reads/writes are lock-free.

#include "PCH.h"
#include "SpaceSync/FlightSync.h"
#include "Config.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "SpaceSync/ShipCombatSync.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "WorldSync/WorldSync.h"
#include "NASLog.h"

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/U/UIMessageQueue.h>
#include <RE/T/TESObjectREFR.h>

#include <REL/Relocation.h>
#include <MinHook.h>

// L.7 declared in Main.cpp; arms the periodic 30s [t+Xs] dump so
// we can see the post-trigger state evolve.
namespace Pulsar::Diag293 { void OnHostCloneUnseated(); }

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

using namespace std::string_view_literals;

namespace Pulsar::FlightSync {
namespace {

    // --- Layout constants (verified via Ghidra RE) ----------------------------

    // V1 (PKT_FLIGHT_STATE) - DISABLED. The StickDataProvider direct-write
    // approach was a race condition: client's local engine writes the same
    // memory at 60Hz with its own input, fighting our 20Hz applies (saw
    // throttle drift to 2.0). The fundamental fix is V2 below - sync at the
    // input layer, not the HUD-state layer.
    //
    // We keep the constructor hook + diagnostic dump for ongoing visibility
    // (the dump reveals which engine fields actually move during piloting).
    static constexpr bool kV1SendEnabled  = false;
    static constexpr bool kV1ApplyEnabled = false;

    // ShipHudDataModel constructor - RE'd from xref to "throttle"/"shipHealth"
    // strings (registers all sub-providers including StickData).
    static constexpr std::uintptr_t kRVA_ShipHudDataModelCtor = 0x1556300;

    // V2 hook target - MovementHandlerAgentPlayerFlightControls::vt[7]
    // (FUN_14068f440 RVA 0x68E6D0 size 157). VERIFIED 2026-04-29: this is
    // ONLY called once on flight-mode attach, NOT per-frame. Diagnostic hook
    // counted exactly 1 invocation per pilot-seat entry.
    // Kept for diagnostic; not the actual sync path.
    static constexpr std::uintptr_t kRVA_FlightInputProcess = 0x68E6D0;

    // V2.1 - actual per-frame PMO fire site. From RE_PMOFire+FireFull:
    //   FUN_142115260 (RVA 0x2112320, size 2673)
    // Builds a ~60-byte event payload from DAT_1461ea1f0+0x20 manager
    // Calls FUN_141570520 to get PMO EventSource
    // Builds a lambda-dispatcher (LocalFunctionImpl<...>) on stack
    // Calls FUN_1422cb8f0(eventSource, &dispatcher) - BSTEventSource::Notify
    //   This is the function the engine calls every pilot-frame to fire
    //   PlayerMovementOutputEvent. Hooking it lets us:
    //     Host:   capture state from DAT_1461ea1f0 + adjacent globals → broadcast
    //     Client: pre-hook overrides those globals with received host values
    //             then lets original fire normally → all sinks (HUD/FlightCtrl/
    //             physics) receive host's input → ship moves identically.
    static constexpr std::uintptr_t kRVA_PMOFire = 0x2112320;

    // V2.2 - BSTEventSource::Notify dispatcher helper, called from many event
    // types (filter by event source). For PMO this is the choke point that
    // delivers the per-frame event payload to all sinks. The dispatcher arg
    // is a stack-allocated lambda struct:
    //   dispatcher+0x00: LocalFunctionImpl<...>::vftable
    //   dispatcher+0x08: &local_308  where  local_308 = &payload
    // So payload bytes = *(*(dispatcher+0x08)).
    static constexpr std::uintptr_t kRVA_NotifyDispatch = 0x22C9300;

    // PMO EventSource singleton VA - DAT_1461F72D8 (RVA 0x61EF448). Set at
    // first call of FUN_141570520; we resolve it lazily at first hook fire.
    static constexpr std::uintptr_t kRVA_PMOEventSource = 0x61EF448;
    static std::atomic<std::uintptr_t> g_pmoSourceVA{ 0 };

    // PMO event payload size. From decomp: local_288..local_234 spans ~0x54
    // bytes. We buffer 96 bytes for safety (includes any trailing fields).
    static constexpr std::size_t kPMOPayloadSize = 96;

    // Offset of the StickData TUIValue<T> ARRAY within ShipHudDataModel.
    // From the ctor decomp:
    //   puVar3 = param_1 + 0x376;             // = +0x1BB0, the Provider object
    //   uVar19 = FUN_14156ce80(param_1 + 0x37F);  // = +0x1BF8, value-array start
    //   FUN_141544730(uVar19, param_1[0x3df]); // registers (values, provider)
    // The TUIValue<T> array starts at +0x37F qwords = +0x1BF8 bytes - the
    // values that FUN_141544730 references at +0x00, +0x20, +0x40 etc.
    static constexpr std::uintptr_t kStickDataOffset = 0x37F * 8;  // = 0x1BF8

    // Each TUIValue<T> entry is 32 bytes; value lives at +0x18 within the entry.
    static constexpr std::uintptr_t kEntryStride    = 0x20;
    static constexpr std::uintptr_t kValueOffsetInEntry = 0x18;
    // The "init/dirty" flag byte at +0x10 is 1 once the engine has written.
    static constexpr std::uintptr_t kInitFlagOffsetInEntry = 0x10;

    // Field-name → entry-index in StickDataProvider (each entry 32 bytes).
    // Types verified via FUN_14156ce80 + diagnostic dump 2026-04-29:
    //   floats (TUIValue<float>): entries 0..11, 17..19, 23
    //   bools  (TUIValue<bool>):  entries 12..16, 20, 21
    //   ints   (TUIValue<int>):   entries 13, 22 (likely)
    enum FieldIdx : int {
        kF_Boost                = 0,   // +0x000 float - fuel meter 0..1
        kF_Throttle             = 1,   // +0x020 float
        kF_AdjustedThrottle     = 2,   // +0x040 float
        kF_Speed                = 3,   // +0x060 float
        kF_ThrusterX            = 4,   // +0x080 float (lateral, mostly 0)
        kF_ThrusterY            = 5,   // +0x0A0 float (lateral, mostly 0)
        kF_ReticleX             = 6,   // +0x0C0 float (mostly 0 in normal flight)
        kF_ReticleY             = 7,   // +0x0E0 float
        kF_DirCrosshairX        = 8,   // +0x100 float - yaw output ⭐
        kF_DirCrosshairY        = 9,   // +0x120 float - pitch output ⭐
        kF_AimAssistX           = 10,
        kF_AimAssistY           = 11,
        kF_BoostActive          = 12,  // +0x180 bool
        kF_ThrottleMode         = 13,  // +0x1A0 int
        kF_BReverse             = 14,  // +0x1C0 bool
        kF_BShowAimAssist       = 15,  // +0x1E0 bool
        kF_BTargetFriendly      = 16,  // +0x200 bool
        kF_AdjustedRoll         = 17,  // +0x220 float
        kF_TurnRateSweetSpot    = 18,  // +0x240 float
        kF_TurnRateSweetSpotRng = 19,  // +0x260 float
        kF_BStickSwap           = 20,  // +0x280 bool
        kF_BShowCruiseButton    = 21,  // +0x2A0 bool
        kF_UICruiseLevel        = 22,  // +0x2C0 int
        kF_FMaxCruiseSpeed      = 23,  // +0x2E0 float
    };

    static std::uintptr_t EntryAddr(std::uintptr_t a_modelBase, int a_idx)
    {
        return a_modelBase + kStickDataOffset + (kEntryStride * a_idx);
    }

    // Network rate.
    static constexpr std::uint64_t kSendIntervalMs = 50;   // 20Hz - enough for cockpit feel.

    // --- State ----------------------------------------------------------------

    // Atomic pointer to most-recent ShipHudDataModel instance. Set by ctor hook,
    // implicitly invalidated when the model is destroyed (we'd need to hook the
    // dtor for a stronger guarantee, but reads are SEH-guarded so a stale
    // pointer simply yields zeros / fault that's swallowed).
    static std::atomic<std::uintptr_t> g_modelPtr{ 0 };
    static std::atomic<std::uint64_t>  g_lastSendMs{ 0 };
    static std::atomic<std::uint64_t>  g_diagSampleCount{ 0 };
    static std::atomic<std::uint64_t>  g_lastDumpMs{ 0 };  // V1.0.1 diag dump

    // Pending received state for client-side apply (game-thread drain).
    struct Pending {
        std::atomic<bool>          armed{ false };
        Pulsar::Network::FlightStateData data{};
        std::uint64_t              receivedAtMs{ 0 };
    };
    static Pending g_pending;

    // --- Hook ----------------------------------------------------------------

    using CtorFn = void* (*)(void* a_self);
    static CtorFn g_origCtor = nullptr;

    static void* __fastcall HookedCtor(void* a_self)
    {
        // Run engine ctor first so the model is fully initialized before we
        // record the pointer.
        void* result = g_origCtor ? g_origCtor(a_self) : nullptr;
        if (a_self) {
            auto p = reinterpret_cast<std::uintptr_t>(a_self);
            g_modelPtr.store(p, std::memory_order_release);
            PLOG_INFO("[PULSAR] FlightSync: ShipHudDataModel ctor - instance=0x{:X}"sv,
                      p);
            NASLOG("FlightSync: ShipHudDataModel ctor instance=0x%llX",
                   (unsigned long long)p);
        }
        return result;
    }

    // --- Read / write helpers -------------------------------------------------

    static float ReadFloat(std::uintptr_t a_entry)
    {
        __try {
            return *reinterpret_cast<float*>(a_entry + kValueOffsetInEntry);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0.0f;
        }
    }
    static std::uint8_t ReadByte(std::uintptr_t a_entry)
    {
        __try {
            return *reinterpret_cast<std::uint8_t*>(a_entry + kValueOffsetInEntry);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
    static std::uint32_t ReadU32(std::uintptr_t a_entry)
    {
        __try {
            return *reinterpret_cast<std::uint32_t*>(a_entry + kValueOffsetInEntry);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
    static void WriteFloat(std::uintptr_t a_entry, float a_v)
    {
        __try {
            *reinterpret_cast<float*>(a_entry + kValueOffsetInEntry) = a_v;
            // Mark dirty so the HUD re-reads the value.
            *reinterpret_cast<std::uint8_t*>(a_entry + kInitFlagOffsetInEntry) = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    static void WriteByte(std::uintptr_t a_entry, std::uint8_t a_v)
    {
        __try {
            *reinterpret_cast<std::uint8_t*>(a_entry + kValueOffsetInEntry) = a_v;
            *reinterpret_cast<std::uint8_t*>(a_entry + kInitFlagOffsetInEntry) = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    static void WriteU32(std::uintptr_t a_entry, std::uint32_t a_v)
    {
        __try {
            *reinterpret_cast<std::uint32_t*>(a_entry + kValueOffsetInEntry) = a_v;
            *reinterpret_cast<std::uint8_t*>(a_entry + kInitFlagOffsetInEntry) = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Read full FlightStateData snapshot from a model instance. Returns false
    // if the model pointer is null. SEH-guarded internally - bad pointer yields
    // zeros, which is harmless (engine just won't see meaningful changes).
    static bool ReadSnapshot(std::uintptr_t a_model, Pulsar::Network::FlightStateData& a_out)
    {
        if (!a_model) return false;
        a_out.throttle         = ReadFloat(EntryAddr(a_model, kF_Throttle));
        a_out.adjustedThrottle = ReadFloat(EntryAddr(a_model, kF_AdjustedThrottle));
        a_out.speed            = ReadFloat(EntryAddr(a_model, kF_Speed));
        a_out.boost            = ReadFloat(EntryAddr(a_model, kF_Boost));
        a_out.dirCrosshairX    = ReadFloat(EntryAddr(a_model, kF_DirCrosshairX));
        a_out.dirCrosshairY    = ReadFloat(EntryAddr(a_model, kF_DirCrosshairY));
        a_out.maxCruiseSpeed   = ReadFloat(EntryAddr(a_model, kF_FMaxCruiseSpeed));
        a_out.boostActive      = ReadByte(EntryAddr(a_model, kF_BoostActive));
        a_out.bReverse         = ReadByte(EntryAddr(a_model, kF_BReverse));
        a_out.uiCruiseLevel    = static_cast<std::uint8_t>(
            ReadU32(EntryAddr(a_model, kF_UICruiseLevel)) & 0xFFu);
        a_out._pad             = 0;
        return true;
    }

    static void ApplySnapshot(std::uintptr_t a_model,
                              const Pulsar::Network::FlightStateData& a_in)
    {
        if (!a_model) return;
        WriteFloat(EntryAddr(a_model, kF_Throttle),         a_in.throttle);
        WriteFloat(EntryAddr(a_model, kF_AdjustedThrottle), a_in.adjustedThrottle);
        WriteFloat(EntryAddr(a_model, kF_Speed),            a_in.speed);
        WriteFloat(EntryAddr(a_model, kF_Boost),            a_in.boost);
        WriteFloat(EntryAddr(a_model, kF_DirCrosshairX),    a_in.dirCrosshairX);
        WriteFloat(EntryAddr(a_model, kF_DirCrosshairY),    a_in.dirCrosshairY);
        WriteFloat(EntryAddr(a_model, kF_FMaxCruiseSpeed),  a_in.maxCruiseSpeed);
        WriteByte(EntryAddr(a_model, kF_BoostActive),       a_in.boostActive);
        WriteByte(EntryAddr(a_model, kF_BReverse),          a_in.bReverse);
        WriteU32(EntryAddr(a_model, kF_UICruiseLevel),      a_in.uiCruiseLevel);
    }

    // V1.0.1 diagnostic dump: read every entry as a float and log it.
    // Lets us identify which entries actually change during steering vs idle -
    // thruster-mode-input vs reticle-transform-input vs other.
    static void DumpAllEntries(std::uintptr_t a_model)
    {
        if (!a_model) return;
        float v[24]{};
        for (int i = 0; i < 24; ++i) {
            v[i] = ReadFloat(EntryAddr(a_model, i));
        }
        PLOG_INFO(
            "[PULSAR] FlightSync DUMP "
            "[0]boost={:.3f} [1]thr={:.3f} [2]adjThr={:.3f} [3]spd={:.3f} "
            "[4]thrustX={:.3f} [5]thrustY={:.3f} [6]reticleX={:.3f} [7]reticleY={:.3f} "
            "[8]dirCrossX={:.3f} [9]dirCrossY={:.3f} [10]aimX={:.3f} [11]aimY={:.3f} "
            "[12]boostAct={:.3f} [13]throtMode={:.3f} [14]reverse={:.3f} [15]showAim={:.3f} "
            "[16]targFr={:.3f} [17]adjRoll={:.3f} [18]turnSS={:.3f} [19]turnSSR={:.3f} "
            "[20]stickSwap={:.3f} [21]showCruise={:.3f} [22]cruiseLvl={:.3f} [23]maxCruise={:.3f}"sv,
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
            v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15],
            v[16], v[17], v[18], v[19], v[20], v[21], v[22], v[23]);
        NASLOG(
            "FlightSync DUMP [0]b=%.3f [1]thr=%.3f [2]adj=%.3f [3]spd=%.3f "
            "[4]tX=%.3f [5]tY=%.3f [6]rX=%.3f [7]rY=%.3f "
            "[8]dX=%.3f [9]dY=%.3f [10]aX=%.3f [11]aY=%.3f "
            "[12]bA=%.3f [13]tM=%.3f [14]rev=%.3f [15]sA=%.3f "
            "[16]tF=%.3f [17]aR=%.3f [18]tSS=%.3f [19]tSSR=%.3f "
            "[20]sSw=%.3f [21]sCr=%.3f [22]cL=%.3f [23]mCr=%.3f",
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
            v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15],
            v[16], v[17], v[18], v[19], v[20], v[21], v[22], v[23]);
    }

    // -----------------------------------------------------------------------
    // V2 - Flight input hook (PKT_FLIGHT_INPUT)
    // -----------------------------------------------------------------------

    // Last host input received on client (network thread writes, hook reads).
    struct PendingFlightInput {
        std::atomic<bool>            armed{ false };
        Pulsar::Network::FlightInputData data{};
        std::atomic<std::uint64_t>   receivedAtMs{ 0 };
    };
    static PendingFlightInput g_pendingInput;

    // Host send rate-limit (engine hook fires ~60Hz; we send 30Hz).
    static constexpr std::uint64_t kInputSendIntervalMs = 33;
    static std::atomic<std::uint64_t> g_lastInputSendMs{ 0 };
    static std::atomic<std::uint64_t> g_inputDiagCount{ 0 };

    // Stale-input timeout: if we haven't heard from host in this long, stop
    // overriding client's local input (so client can pilot again on disconnect).
    static constexpr std::uint64_t kInputStaleMs = 1500;

    using FlightInputProcessFn = void(__fastcall*)(void*, void*);
    static FlightInputProcessFn g_origFlightInputProcess = nullptr;

    // V2.1 - per-frame PMO fire hook (FUN_142115260).
    using PMOFireFn = void(__fastcall*)(void*, void*);
    static PMOFireFn g_origPMOFire = nullptr;
    static std::atomic<std::uint64_t> g_pmoFireCount{ 0 };
    static std::atomic<std::uint64_t> g_lastPMOLogMs{ 0 };

    // V2.2 - Notify-dispatch hook on FUN_1422CB8F0.
    using NotifyDispatchFn = void(__fastcall*)(void*, void*);
    static NotifyDispatchFn g_origNotifyDispatch = nullptr;
    static std::atomic<std::uint64_t> g_notifyTotalCount{ 0 };
    static std::atomic<std::uint64_t> g_notifyPMOCount{ 0 };
    static std::atomic<std::uint64_t> g_lastNotifyLogMs{ 0 };
    static std::atomic<std::uint64_t> g_lastPMOSendMs{ 0 };
    static std::atomic<std::uint64_t> g_lastPMOApplyLogMs{ 0 };
    static constexpr std::uint64_t kPMOSendIntervalMs = 33;  // 30 Hz broadcast
    static constexpr std::uint64_t kPMOStaleMs = 1500;       // client revert if no host

    // Latest received host PMO payload (network thread writes; hook reads).
    struct PendingPMOPayload {
        std::atomic<bool>           armed{ false };
        Pulsar::Network::PMOPayloadData data{};
        std::atomic<std::uint64_t>  receivedAtMs{ 0 };
    };
    static PendingPMOPayload g_pendingPMOPayload;

    // ----------------------------------------------------------------------
    // V3 - Ship NiNode world-transform sync
    // ----------------------------------------------------------------------
    //
    // Bypass event/input layers - sync the FINAL render-transform directly.
    // Ship NiAVObject world transform: rotate@+0x80 (3x3), translate@+0xB0
    // (vec3). Path: PlayerCharacter → GetSpaceship → REFR+0xB8 (loadedData)
    // → +0x08 (data3D) → NiAVObject. CLib LOADED_REF_DATA verified by
    // CellDiag/Cell3DAttach which read live heap pointers via this path.
    //
    // Host: read NiAVObject world bytes per-frame, broadcast PKT_SHIP_TRANSFORM.
    // Client: write received bytes to local NiAVObject before next render.
    static constexpr bool kV3SendEnabled  = true;
    static constexpr bool kV3ApplyEnabled = true;

    static constexpr std::uintptr_t kRefr_LoadedDataOffset = 0xB8;
    static constexpr std::uintptr_t kLoaded_Data3DOffset   = 0x08;
    // NiAVObject layout (verified CLib NiTransform.h + NiAVObject.h):
    //   local  @ +0x40 - rotate@+0x40 (NiMatrix3, 36B), translate@+0x70 (12B)
    //   world  @ +0x80 - rotate@+0x80 (36B),            translate@+0xB0 (12B)
    // First V3 attempt wrote ONLY world - wire was byte-identical but client
    // saw no visual change. Hypothesis: engine's UpdateWorldData() pass each
    // frame recomputes world = parent.world * local, overwriting our world
    // write before render. Fix: write LOCAL too so the recompute reproduces
    // host's transform (assumes ship's parent NiNode is identity, which holds
    // for the ship REFR sitting at static (0,0,0) anchor in space).
    static constexpr std::uintptr_t kNiAv_LocalRotateOffset    = 0x40;
    static constexpr std::uintptr_t kNiAv_LocalTranslateOffset = 0x70;
    static constexpr std::uintptr_t kNiAv_WorldRotateOffset    = 0x80;
    static constexpr std::uintptr_t kNiAv_WorldTranslateOffset = 0xB0;

    static constexpr std::uint64_t kV3SendIntervalMs = 16;   // 60 Hz (was 30 Hz; user reported stutter)
    static constexpr std::uint64_t kV3StaleMs        = 1500;

    static std::atomic<std::uint64_t> g_v3SendCount{ 0 };
    static std::atomic<std::uint64_t> g_v3ApplyCount{ 0 };
    static std::atomic<std::uint64_t> g_lastV3SendMs{ 0 };
    static std::atomic<std::uint64_t> g_lastV3ApplyMs{ 0 };
    static std::atomic<std::uint64_t> g_lastV3LogMs{ 0 };

    struct PendingShipTransform {
        std::atomic<bool>           armed{ false };
        Pulsar::Network::ShipTransformData data{};
        std::atomic<std::uint64_t>  receivedAtMs{ 0 };
    };
    static PendingShipTransform g_pendingShipTransform;

    // Resolve the ship's NiAVObject pointer (data3D) via the verified path.
    // Returns nullptr if any hop is null or fails. SEH-guarded.
    // L.3 V5 - last shipFormID observed in an inbound PKT_SHIP_TRANSFORM. Used
    // by the apply-side fallback when our local player->GetSpaceship(true)
    // returns null. Typical post-WorldSync save load: client materializes in
    // the cockpit cell but the engine doesn't restore the spaceship link
    // (BSComponentDB2 cell→ship registry has no entry on this side), so the
    // standard lookup misses. We then resolve the ship REFR by FormID and walk
    // its loadedData→data3D directly.
    static std::atomic<std::uint32_t> g_lastReceivedShipFormID{ 0 };

    static void* SafeRefrToShipNiAV(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref) return nullptr;
        __try {
            auto* base = reinterpret_cast<std::uint8_t*>(a_ref);
            auto loaded = *reinterpret_cast<std::uintptr_t*>(base + kRefr_LoadedDataOffset);
            if (!loaded) return nullptr;
            auto data3D = *reinterpret_cast<std::uintptr_t*>(
                reinterpret_cast<std::uint8_t*>(loaded) + kLoaded_Data3DOffset);
            if (!data3D) return nullptr;
            return reinterpret_cast<void*>(data3D);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // L.3 V5 - log only the first time the fallback resolves so the live log
    // doesn't fill with one-line entries.
    static std::atomic<bool> g_loggedFallbackResolve{ false };

    static void* SafeGetShipNiAVObject()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return nullptr;
        if (auto* ship = player->GetSpaceship(true)) {
            return SafeRefrToShipNiAV(ship);
        }

        // Fallback: GetSpaceship returned null. On the receiver side after a
        // WorldSync save load this is normal - try resolving by the formID
        // the active pilot just sent us.
        const std::uint32_t fmt =
            g_lastReceivedShipFormID.load(std::memory_order_acquire);
        if (fmt == 0) return nullptr;
        auto* form = RE::TESForm::LookupByID(fmt);
        if (!form) return nullptr;
        auto* ref = form->As<RE::TESObjectREFR>();
        void* niAv = SafeRefrToShipNiAV(ref);
        if (niAv && !g_loggedFallbackResolve.exchange(true,
                                                     std::memory_order_acq_rel)) {
            PLOG_INFO(
                "[PULSAR] L.3 V5 ship-niAv fallback: shipFormID=0x{:08X} ref=0x{:X} niAv=0x{:X}"sv,
                fmt, reinterpret_cast<std::uintptr_t>(ref),
                reinterpret_cast<std::uintptr_t>(niAv));
        }
        return niAv;
    }

    static bool SafeReadShipTransform(void* a_niAv,
                                      Pulsar::Network::ShipTransformData& a_out)
    {
        if (!a_niAv) return false;
        __try {
            auto* base = reinterpret_cast<std::uint8_t*>(a_niAv);
            std::memcpy(a_out.rotate,
                        base + kNiAv_WorldRotateOffset, sizeof(a_out.rotate));
            std::memcpy(a_out.translate,
                        base + kNiAv_WorldTranslateOffset, sizeof(a_out.translate));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static void SafeWriteShipTransform(void* a_niAv,
                                       const Pulsar::Network::ShipTransformData& a_in)
    {
        if (!a_niAv) return;
        __try {
            auto* base = reinterpret_cast<std::uint8_t*>(a_niAv);
            // Write LOCAL first - survives engine's UpdateWorldData pass.
            std::memcpy(base + kNiAv_LocalRotateOffset,
                        a_in.rotate, sizeof(a_in.rotate));
            std::memcpy(base + kNiAv_LocalTranslateOffset,
                        a_in.translate, sizeof(a_in.translate));
            // Write WORLD too - covers the case where renderer reads world
            // before next UpdateWorldData. Idempotent if engine recomputes.
            std::memcpy(base + kNiAv_WorldRotateOffset,
                        a_in.rotate, sizeof(a_in.rotate));
            std::memcpy(base + kNiAv_WorldTranslateOffset,
                        a_in.translate, sizeof(a_in.translate));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // True when local rotation is actively changing (= we are the pilot).
    // Set in MaybeSyncShipTransform, read in HookedBhkSpaceshipVt8 to gate
    // the havok cache override.
    static std::atomic<bool> g_isActivePilot{ false };

    // L.7/post-bridge input lockout window. Hoisted up so
    // IsLocalDeterministicPilot can read it (defined originally near
    // TickPostLoadUnseat). While now < this timestamp, the FlightInput
    // hook zeroes the client's local input AND the local pilot detection
    // refuses to claim pilot regardless of saved state. Set in
    // TickPostLoadUnseat to GetTickCount64()+30000.
    static std::atomic<std::uint64_t> g_postBridgeInputLockoutUntilMs{ 0 };

    // L.4 V1.1 - visual notification attempt removed.
    //
    // Tried REL::ID(138728) "DebugNotification" (CommonLibSF Misc.h placeholder
    // with comment-noted ID). Function resolves to RVA 0x2878C50 and calls
    // return without SEH crash, BUT no visible pop-up appears on screen in
    // SF 1.16.236. Likely a different function (Console_Log or stub) or
    // different signature.
    //
    // Visual feedback deferred to L.4 V2 (with NPCSync-Space + projectile
    // spawn) - at that point we'll do a proper Ghidra RE pass for Bethesda's
    // HUD message API or use the BGSEffectShader path.

    // V3 iter10 - separate send buffer for outgoing havok state (declared
    // here so MaybeSyncShipTransform can read it).
    static std::atomic<bool> g_outgoingHavokStateValid{ false };
    static float             g_outgoingHavokState[16]{};
    static std::mutex        g_outgoingHavokStateMutex;

    // V3 iter12 - PMO fire timestamp for active-pilot detection.
    // (Set in HookedPMOFire - defined later in this file.)
    static std::atomic<std::uint64_t> g_lastPMOFireMs{ 0 };
    // Forward decls - defined in iter8 hook section below.
    static void PublishOutgoingHavokState(const float a_in[16]);
    static bool ConsumeOutgoingHavokState(float a_out[16]);

    // Track the last rotate row0 we OBSERVED on our local NiNode, to detect
    // whether physics is actively writing it (i.e. we are the active pilot).
    static std::atomic<std::uint32_t> g_lastObservedRotR0X{ 0 };  // float bits
    static std::atomic<std::uint32_t> g_lastObservedRotR0Y{ 0 };
    static std::atomic<std::uint32_t> g_lastObservedRotR0Z{ 0 };
    // Track the last rotate row0 we WROTE, to detect engine overwrites.
    static std::atomic<std::uint32_t> g_lastWroteRotR0X{ 0 };
    static std::atomic<std::uint32_t> g_lastWroteRotR0Y{ 0 };
    static std::atomic<std::uint32_t> g_lastWroteRotR0Z{ 0 };

    static float Bits2Float(std::uint32_t a) {
        float f; std::memcpy(&f, &a, 4); return f;
    }
    static std::uint32_t Float2Bits(float a) {
        std::uint32_t u; std::memcpy(&u, &a, 4); return u;
    }

    // One-time diag: dump data3D vftable, NiNode children info, first child's
    // world rotate. Validates whether data3D is a leaf or a container.
    static std::atomic<bool> g_v3DiagDone{ false };
    static void V3DumpScene(void* a_niAv)
    {
        if (!a_niAv) return;
        if (g_v3DiagDone.exchange(true)) return;
        __try {
            auto* base = reinterpret_cast<std::uint8_t*>(a_niAv);
            std::uintptr_t vftbl = *reinterpret_cast<std::uintptr_t*>(base + 0x00);
            // NiNode adds members AFTER NiAVObject. NiAVObject ends at ~0x130
            // (worldBound=NiBound ends @+0x110, plus a few more fields).
            // children BSTArray @ +0x130 (NiNode first member after vftable).
            // BSTArray layout: ptr@+0x00, size@+0x10, capacity@+0x14 (CLib).
            std::uintptr_t childArrPtr = *reinterpret_cast<std::uintptr_t*>(base + 0x130);
            std::uint32_t childCount = *reinterpret_cast<std::uint32_t*>(base + 0x140);
            std::uintptr_t child0Vf = 0;
            float c0r0[3] = { 0, 0, 0 };
            float c0t[3]  = { 0, 0, 0 };
            if (childArrPtr && childCount > 0) {
                std::uintptr_t child0 = *reinterpret_cast<std::uintptr_t*>(childArrPtr);
                if (child0) {
                    auto* cb = reinterpret_cast<std::uint8_t*>(child0);
                    child0Vf = *reinterpret_cast<std::uintptr_t*>(cb + 0x00);
                    c0r0[0] = *reinterpret_cast<float*>(cb + 0x80);
                    c0r0[1] = *reinterpret_cast<float*>(cb + 0x84);
                    c0r0[2] = *reinterpret_cast<float*>(cb + 0x88);
                    c0t[0]  = *reinterpret_cast<float*>(cb + 0xB0);
                    c0t[1]  = *reinterpret_cast<float*>(cb + 0xB4);
                    c0t[2]  = *reinterpret_cast<float*>(cb + 0xB8);
                }
            }
            PLOG_INFO(
                "[PULSAR] V3 scene-dump: niAv=0x{:X} vftbl=0x{:X} childArr=0x{:X} "
                "childCount={} child0Vf=0x{:X} c0r0=({:.3f},{:.3f},{:.3f}) "
                "c0t=({:.2f},{:.2f},{:.2f})"sv,
                reinterpret_cast<std::uintptr_t>(a_niAv), vftbl, childArrPtr,
                childCount, child0Vf,
                c0r0[0], c0r0[1], c0r0[2],
                c0t[0], c0t[1], c0t[2]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // V3 iter13 - Active-pilot detection via per-body translation-delta EMA.
    //
    // Iter11 (NiNode rotation change): both sides physics-integrate → both
    // flag active → no apply → 2-3s stutter.
    // Iter12 (PMO-fire-rate): PMO fires per-physics-tick on BOTH sides
    // independent of input → useless signal.
    // Iter13: track havok body row3 (translation) delta inside the vt[8]
    // hook. The side actually receiving flight input integrates real motion
    // (~m/s); a passive body in zero-G with no input drifts essentially zero
    // (per iter12 logs row3=(0,0,0,0) constant on client).
    // Apply-feedback bias on the passive side (its row3 echoes host's
    // motion right after a write) is suppressed via a short cooldown after
    // each successful passive write.
    //
    // Implementation: see Iter13DetectAndSetActive() below near the vt[8]
    // hook (uses kHavokBodyDataPtrOffset / kHavokStateOffsetInData defined
    // further down in this anonymous namespace).

    // Per-frame sync - called from the PMO branch of HookedNotifyDispatch on
    // both host and client. Decides per-frame:
    // If WE are the active pilot (physics writing local NiNode): broadcast.
    // Else: apply latest received transform (if armed and fresh).
    static bool IsCruiseActive();  // forward decl - defined after PendingCruise

    static void MaybeSyncShipTransform()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        // 3v7 - L.3 V3 cruise suspend REMOVED here on host side. User
        // confirmed cruise sync did work historically; blanket suspend
        // killed it. Magnitude clamp on receiver side filters out the
        // chaotic-state spikes (>1e7) that caused freeze 2026-05-01 14:08
        // while keeping normal cruise positions flowing.

        const std::uint64_t now = ::GetTickCount64();

        void* niAv = SafeGetShipNiAVObject();
        if (!niAv) return;

        // One-time scene dump for diagnosis.
        V3DumpScene(niAv);

        // Read current local rotate to determine activity.
        Pulsar::Network::ShipTransformData live{};
        if (!SafeReadShipTransform(niAv, live)) return;

        // Iter13: g_isActivePilot is now set in HookedBhkSpaceshipVt8 via
        // Iter13DetectAndSetActive() - vt[8] runs at higher freq than PMO and
        // owns the motion-based detection. We just read the latest decision.
        const bool active = g_isActivePilot.load(std::memory_order_acquire);

        if (active && kV3SendEnabled) {
            const std::uint64_t last = g_lastV3SendMs.load(std::memory_order_acquire);
            if ((now - last) >= kV3SendIntervalMs) {
                g_lastV3SendMs.store(now, std::memory_order_release);
                // Pack havok state from the dedicated send buffer (filled by
                // iter8 active_read in vt[8] hook).
                ConsumeOutgoingHavokState(live.havokState);
                // L.3 V5 - stamp shipFormID so the receiver can resolve its
                // local same-FormID ship REFR when its own GetSpaceship lookup
                // fails (post-save-load on the passive side).
                if (auto* localPlayer = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* localShip = localPlayer->GetSpaceship(true)) {
                        live.shipFormID = localShip->formID;
                    }
                }
                std::uint8_t buf[160]{};  // ShipTransformData=116B + 3B header
                Pulsar::Network::PacketWriter w(
                    Pulsar::Network::PKT_SHIP_TRANSFORM, buf, sizeof(buf));
                if (w.Write(live)) {
                    net.Send(w.Data(), w.Size(), false);
                    std::uint64_t cnt = g_v3SendCount.fetch_add(1) + 1;
                    std::uint64_t lastLog = g_lastV3LogMs.load(std::memory_order_acquire);
                    if ((now - lastLog) >= 1000) {
                        g_lastV3LogMs.store(now, std::memory_order_release);
                        PLOG_INFO(
                            "[PULSAR] V3 send (active pilot): cnt={} niAv=0x{:X} "
                            "t=({:.2f},{:.2f},{:.2f}) row0=({:.3f},{:.3f},{:.3f})"sv,
                            cnt, reinterpret_cast<std::uintptr_t>(niAv),
                            live.translate[0], live.translate[1], live.translate[2],
                            live.rotate[0], live.rotate[1], live.rotate[2]);
                    }
                }
            }
            return;
        }

        // Passive - apply received if fresh.
        if (!kV3ApplyEnabled) return;
        const bool armed = g_pendingShipTransform.armed.load(std::memory_order_acquire);
        if (!armed) return;
        const std::uint64_t recvAt = g_pendingShipTransform.receivedAtMs.load(std::memory_order_acquire);
        if (recvAt == 0 || (now - recvAt) > kV3StaleMs) return;

        // BEFORE-write diagnostic: log live (pre-write) values vs what we
        // wrote last frame. If live ≠ last-write → engine overwrote.
        const auto& d = g_pendingShipTransform.data;
        std::uint64_t lastLog = g_lastV3LogMs.load(std::memory_order_acquire);
        const bool willLog = (now - lastLog) >= 1000;

        SafeWriteShipTransform(niAv, d);
        // Record what we wrote so IsActivePilot can suppress self-detection.
        g_lastWroteRotR0X.store(Float2Bits(d.rotate[0]), std::memory_order_release);
        g_lastWroteRotR0Y.store(Float2Bits(d.rotate[1]), std::memory_order_release);
        g_lastWroteRotR0Z.store(Float2Bits(d.rotate[2]), std::memory_order_release);
        // Update observed so IsActivePilot doesn't flag our write as activity.
        g_lastObservedRotR0X.store(Float2Bits(d.rotate[0]), std::memory_order_release);
        g_lastObservedRotR0Y.store(Float2Bits(d.rotate[1]), std::memory_order_release);
        g_lastObservedRotR0Z.store(Float2Bits(d.rotate[2]), std::memory_order_release);

        std::uint64_t cnt = g_v3ApplyCount.fetch_add(1) + 1;
        if (willLog) {
            g_lastV3LogMs.store(now, std::memory_order_release);
            // Read back to verify write took.
            Pulsar::Network::ShipTransformData verify{};
            SafeReadShipTransform(niAv, verify);
            PLOG_INFO(
                "[PULSAR] V3 apply: cnt={} ageMs={} pre-write=({:.3f},{:.3f},{:.3f}) "
                "wrote=({:.3f},{:.3f},{:.3f}) post-read=({:.3f},{:.3f},{:.3f})"sv,
                cnt, (now - recvAt),
                live.rotate[0], live.rotate[1], live.rotate[2],
                d.rotate[0], d.rotate[1], d.rotate[2],
                verify.rotate[0], verify.rotate[1], verify.rotate[2]);
        }
    }

    // Dump 24 floats from a pointer; SEH-guarded.
    static void DumpParamFloats(const char* a_label, void* a_p)
    {
        if (!a_p) return;
        float v[24]{};
        __try {
            auto* base = reinterpret_cast<std::uint8_t*>(a_p);
            for (int i = 0; i < 24; ++i) {
                v[i] = *reinterpret_cast<float*>(base + i * 4);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        PLOG_INFO(
            "[PULSAR] PMOFire {} f[0..23]: "
            "{:.3f} {:.3f} {:.3f} {:.3f} | "
            "{:.3f} {:.3f} {:.3f} {:.3f} | "
            "{:.3f} {:.3f} {:.3f} {:.3f} | "
            "{:.3f} {:.3f} {:.3f} {:.3f} | "
            "{:.3f} {:.3f} {:.3f} {:.3f} | "
            "{:.3f} {:.3f} {:.3f} {:.3f}"sv,
            a_label,
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
            v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15],
            v[16], v[17], v[18], v[19], v[20], v[21], v[22], v[23]);
    }

    // V3 iter12 - track timestamp of latest PMO fire for active-pilot
    // detection. PMO fires only on the side that's actively piloting
    // (per RE: "~1250 Hz on host pilot"). Passive side's engine doesn't
    // fire PMO. Reliable signal for "I am the pilot locally".
    // (g_lastPMOFireMs declared earlier so IsActivePilot can access it.)

    static void __fastcall HookedPMOFire(void* a_param1, void* a_param2)
    {
        g_lastPMOFireMs.store(::GetTickCount64(), std::memory_order_release);
        // Diagnostic - log every 1s to verify per-frame timing.
        std::uint64_t fc = g_pmoFireCount.fetch_add(1);
        std::uint64_t now0 = ::GetTickCount64();
        std::uint64_t last0 = g_lastPMOLogMs.load(std::memory_order_acquire);
        if ((now0 - last0) >= 500) {  // 2 Hz dump
            g_lastPMOLogMs.store(now0, std::memory_order_release);
            // V2.2 diagnostic: dump param_1's first 96 bytes as floats - we
            // need to identify which fields hold the live flight input/state
            // so we can pick the right sync target.
            PLOG_INFO("[PULSAR] PMOFire fired: count={} param1=0x{:X}"sv,
                      fc, reinterpret_cast<std::uintptr_t>(a_param1));
            DumpParamFloats("p1", a_param1);
        }

        // Always run engine logic - diagnostic only at this stage.
        if (g_origPMOFire) {
            g_origPMOFire(a_param1, a_param2);
        }
    }

    // ----------------------------------------------------------------------
    // V2.2 - Notify-dispatch hook
    // ----------------------------------------------------------------------
    //
    // FUN_1422CB8F0 is a generic BSTEventSource::Notify dispatcher. It's
    // called for many event types per frame; we filter to PMO via the
    // singleton-pointer match. When matched:
    //   Host:   read payload bytes via 2x dereference from dispatcher,
    //           rate-limited broadcast PKT_PMO_PAYLOAD.
    //   Client: if recent host payload pending, overwrite payload bytes
    //           in-place BEFORE original Notify dispatches them. All sinks
    //           (HUD, FlightController, ship physics) then process host's
    //           exact state - both engines compute identical visuals.

    static std::uintptr_t ResolvePMOEventSourceVA()
    {
        std::uintptr_t cached = g_pmoSourceVA.load(std::memory_order_acquire);
        if (cached) return cached;
        std::uintptr_t resolved = REL::Offset{ kRVA_PMOEventSource }.address();
        g_pmoSourceVA.store(resolved, std::memory_order_release);
        return resolved;
    }

    // V4 Fire-Probe - resolve all candidate fire/projectile vtables.
    // 413845 = BSTEventSource<BGSOnPlayerFireWeaponEvent> (foot weapons mostly)
    // 412442 = BSTEventSource<WeaponFiredEvent>           (generic - likely ship)
    // 454462 = BSTEventSource<ProjectileBeginUpdateEvent> (proj spawn)
    static std::atomic<std::uintptr_t> g_fireVt_OnPlayer{ 0 };
    static std::atomic<std::uintptr_t> g_fireVt_Generic { 0 };
    static std::atomic<std::uintptr_t> g_fireVt_ProjBeg { 0 };
    static std::uintptr_t ResolveFireVt(std::atomic<std::uintptr_t>& a_cache,
                                        std::uint64_t a_id)
    {
        std::uintptr_t cached = a_cache.load(std::memory_order_acquire);
        if (cached) return cached;
        std::uintptr_t va = REL::ID(static_cast<std::uint32_t>(a_id)).address();
        a_cache.store(va, std::memory_order_release);
        return va;
    }
    static std::uintptr_t ResolveFireEventVtableVA()  // legacy name kept
    {
        return ResolveFireVt(g_fireVt_OnPlayer, 413845);
    }

    // L.5 Phase 1 - cruise event vtable cache. CommonLibSF already provides
    // the BSTGlobalEvent::EventSource<...> vtable IDs; we re-use the existing
    // ResolveFireVt cache helper. IDs from IDs_VTABLE.h:
    //   1036687 = BSTGlobalEvent::EventSource<Reticle::OnCruiseActivate>
    //   1036685 = BSTGlobalEvent::EventSource<Reticle::OnCruiseExitAnimFinished>
    //   1036683 = BSTGlobalEvent::EventSource<Reticle::OnCruiseLockCourse>
    //   1037134 = BSTGlobalEvent::EventSource<Spaceship::SpaceCruiseArrival>
    static std::atomic<std::uintptr_t> g_cruiseVt_Activate { 0 };
    static std::atomic<std::uintptr_t> g_cruiseVt_ExitAnim { 0 };
    static std::atomic<std::uintptr_t> g_cruiseVt_LockCourse{ 0 };
    static std::atomic<std::uintptr_t> g_cruiseVt_Arrival  { 0 };
    static std::atomic<std::uint64_t>  g_cruiseProbeHitCount{ 0 };

    // L.5 Phase 2 - poll the cruise-active global byte from Tick. Two prior
    // hook strategies (Papyrus EnableCruiseMode @ 0x2024C20 and AV-set
    // dispatcher @ 0xC1BD80 with edx=0x40000) didn't fire on user-keypress
    // cruise toggle. Static binary scan revealed:
    // IsCruiseModeActive @ 0x2014500 = `movzx eax, byte [0x145EAA5AE]; ret`
    // Cruise enter writes 1 to 0x145EAA5AE @ 0x1413FDAF2 (in fn 0x1413FDAC0)
    // Cruise exit writes 0 to 0x145EAA5AE @ 0x14140003D
    // The cruise-active state is a SINGLE GLOBAL BYTE. Polling at 60Hz is the
    // simplest way to detect transitions without instrumenting the complex
    // enter/exit functions. Latency ~16ms is fine for a state-toggle.
    static constexpr std::uintptr_t kRVA_CruiseActiveByte = 0x5EAA5AE;
    static std::atomic<std::uintptr_t> g_cruiseActiveBytePtr{ 0 };
    static std::atomic<std::uint8_t>   g_lastCruiseActive{ 0xFF };  // 0xFF = uninitialized
    // Phase 4 Step 6 - "arrival initiated" byte at DAT_145EB2856. Set to 1 by
    // FUN_1413FA140 when per-tick math detects close-enough proximity to
    // current cruise destination POI. Gates dur progression in FUN_1413FEBA0
    // (read at line 0x1413ffd11 of arrival sender). Polled at same 60Hz as
    // cruise-active byte.
    static constexpr std::uintptr_t kRVA_CruiseArrivalByte = 0x5EAAA96;
    static std::atomic<std::uintptr_t> g_cruiseArrivalBytePtr{ 0 };
    static std::atomic<std::uint8_t>   g_lastCruiseArrival{ 0xFF };
    // Track whether arrival init fired during current cruise session - lets us
    // distinguish auto-arrival exit (post-arrivalInit) from manual exit
    // (player pressed cruise key mid-flight). Reset on cruise byte 0->1.
    static std::atomic<bool> g_arrivalSeenThisCruise{ false };

    // L.5 Phase 2 - cruise enter/exit functions. Two separate functions:
    //
    //   ENTER fn @ RVA 0x13FDAC0 - `void __fastcall(bool force)`
    // movzx esi, cl    ; cl = bool arg (force flag)
    // call predicate
    // if predicate || arg → mov byte [0x145EAA5AE], 1 (cruise=on)
    // else → just return (NO byte=0 write - this is enter-only)
    //
    //   EXIT fn @ RVA 0x13FFD00 - `void()` (size 523)
    // cleans up cruise event handles, cell-streaming counters
    // mov byte [0x145EAA5AE], 0 + 8 other cruise-state vars
    // this IS the proper exit-cleanup; the bare toggle fn doesn't exit.
    static constexpr std::uintptr_t kRVA_CruiseEnter = 0x13FDAC0;
    static constexpr std::uintptr_t kRVA_CruiseExit  = 0x13FFD00;
    // Phase 4 Step 6 - "Cruise Mode Arrival Initiated" trigger function.
    // Sets the 3 gate bits (DAT_145EB2856, DAT_145EB29EE via FUN_1413F9400,
    // DAT_14590E27F) that unlock dur progression in FUN_1413FEBA0, plus
    // schedules the cell-stream task (FUN_140B6EB20) so client loads same
    // POI content as host. Called on client when host's DAT_145EB2856 0->1.
    static constexpr std::uintptr_t kRVA_CruiseArrivalInitFn = 0x13F9F70;
    // Phase 4 deep - cell-stream task scheduler. Fires the lambda that loads
    // POI/planet/NPC content based on ship's current world position. Called
    // engine-natural at end of dur countdown (FUN_1413FEBA0). We call it
    // again on client AFTER our orient/state writes settled so cell-streaming
    // operates on host's full world context instead of stale client state.
    static constexpr std::uintptr_t kRVA_CellStreamTaskFn  = 0x140B6E400;
    static constexpr std::uintptr_t kRVA_PlayerWorldState  = 0x5F43230;  // DAT_145F4AFD0 singleton (ship+world manager)
    // Phase 4 deep - LocationMarkerArrayUpdate::Event Notify wrapper.
    // 64-byte fn that gets BSTEventSource singleton @ 0x14597A220 and fires
    // the event with a single-byte payload. Subscribers re-query the marker
    // manager → recompute bIsCelestialParentBody (the field that drives
    // planet rendering) → refresh sky backdrop. Engine fires this from
    // FUN_141A61D00 with byte=1 ("full refresh"). We fire on client post-
    // EXIT to force re-evaluation with synced player position.
    static constexpr std::uintptr_t kRVA_MarkerNotifyFn    = 0x141A1C2F0;
    using CruiseEnterFn = void(__fastcall*)(bool);
    using CruiseExitFn  = void(__fastcall*)();
    using CruiseArrivalInitFn = void(__fastcall*)();
    using CellStreamTaskFn    = void(__fastcall*)(void*);
    using MarkerNotifyFn      = void(__fastcall*)(std::uint8_t);
    static std::atomic<std::uintptr_t> g_cruiseEnterFn{ 0 };
    static std::atomic<std::uintptr_t> g_cruiseExitFn { 0 };
    static std::atomic<std::uintptr_t> g_cruiseArrivalInitFn{ 0 };
    static std::atomic<std::uintptr_t> g_cellStreamTaskFn   { 0 };
    static std::atomic<std::uintptr_t> g_markerNotifyFn     { 0 };

    // Pending cruise apply - staged from network thread, applied from Tick on
    // game thread. armed=true means "an apply is queued"; once applied, armed
    // is cleared. Receiver-side suppression flag prevents the apply from
    // re-broadcasting (host already broadcasted, client must not echo).
    struct PendingCruise {
        std::atomic<bool>          armed{ false };
        std::atomic<std::uint8_t>  enable{ 0 };
        std::atomic<std::uint64_t> receivedAtMs{ 0 };
        // Phase 3v3 - host's world-position doubles at EXIT moment. Client
        // writes them to its own globals BEFORE calling FUN_1413FFED0() so
        // exit-cleanup reads host's coordinates → final ship position match.
        std::atomic<std::uint64_t> shipPosX{ 0 };  // raw bits of double (atomic-safe)
        std::atomic<std::uint64_t> shipPosY{ 0 };
        std::atomic<std::uint64_t> shipPosZ{ 0 };
        // Phase 4 Step 4 - destination formID @ DAT_145EB8284 (low 32b). Set
        // by host's resolver at ENTER, sent here, written before client's
        // force-ENTER so per-tick arrival check has a target.
        std::atomic<std::uint32_t> destFormId{ 0 };
        // Phase 4 Step 6 - "arrival initiated" event. Independent trigger
        // from main armed flag so ENTER and arrival-init can both be pending
        // simultaneously without overwriting each other.
        std::atomic<bool>          armedArrivalInit{ false };
        // Phase 4 Step 7 - extra state fields synced with arrival init.
        std::atomic<std::uint32_t> arrivalState{ 0 };  // DAT_145EB8274 value
        std::atomic<std::uint8_t>  uiSelector{ 0 };    // DAT_145EB2857 value
    };
    static PendingCruise g_pendingCruise;

    // L.5 Phase 3v5 - fast helper for L.3 V3 ship-sync to detect cruise active
    // state. Reads the same byte L.5 polls. During cruise, the engine writes
    // wildly oscillating row3 values to the ship's havok body (intermediate
    // cruise math states up to 100M+ in magnitude, see logs 2026-05-01 +
    // 14:08 freeze). Replaying these on the client breaks the engine. We
    // suspend L.3 V3 send/apply during cruise so client's body stays stable
    // until the cruise EXIT settles. Resume on byte 1→0.
    static bool IsCruiseActive()
    {
        std::uintptr_t ptr = g_cruiseActiveBytePtr.load(std::memory_order_acquire);
        if (!ptr) return false;
        std::uint8_t v = 0;
        __try { v = *reinterpret_cast<std::uint8_t*>(ptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return v != 0;
    }
    // Suppression: when we apply a remote cruise transition by calling the
    // toggle function, our own poll will see the byte change. We set this
    // window to suppress re-broadcasting the resulting transition.
    static std::atomic<std::uint64_t> g_cruiseApplySuppressUntilMs{ 0 };
    // L.5 Phase 4 Step 16 - bidirectional cruise. Track which side ORIGINATED
    // the current cruise session: true when our local poll detects a 0→1
    // transition outside the suppression window (= we caused it). Drives
    // continuous orient broadcast (only the originator's local engine math
    // produces meaningful orient values; the receiver's local engine has
    // force=true skipped resolver). Cleared on cruise EXIT (1→0).
    static std::atomic<bool> g_cruiseSelfInitiated{ false };

    // L.5 Phase 3 - post-cruise-EXIT cell resync. When host detects 1→0
    // cruise transition and broadcasts it, we ALSO schedule a SaveBuffer-
    // resync after kPostCruiseResyncDelayMs so the client lands in the
    // post-cruise destination cell. The cruise EXIT cleanup function clears
    // the byte and decrements cell-streaming counters but the engine needs
    // time to fully settle into the destination cell before we snapshot.
    // 5s is a conservative latch - empirically cruise-arrival cell streaming
    // completes within ~2-3s on a modest SSD.
    static constexpr std::uint64_t kPostCruiseResyncDelayMs = 5000;
    static std::atomic<std::uint64_t> g_postCruiseResyncDueMs{ 0 };

    // L.5 Phase 3 v2 - gated resync. Capture host's shipCell formID at the
    // 0→1 transition (cruise start). Only fire SaveBuffer-resync at EXIT if
    // post-cruise shipCell actually differs (= cruise crossed cells, e.g.
    // cross-system far-travel). Same-cell cruise is the common case (intra-
    // system 400ls movement) and L.3 V3 ship pos sync handles it naturally -
    // resync is overkill there AND transfers the host's "is pilot" state
    // to client (regression observed 2026-05-01).
    static std::atomic<std::uint32_t> g_preCruiseShipCellID{ 0 };

    // L.5 Phase 3 debug - cruise-state globals discovered via Ghidra RE
    // (2026-05-01). Dumped to log at every cruise state transition + 1Hz
    // during cruise so we can compare host vs client state and decide if
    // 3a (SaveBuffer-resync) is sufficient or 3c (state-replay) is needed.
    static constexpr std::uintptr_t kRVA_Cruise2ndByte    = 0x5EAA4D7;  // secondary cruise flag
    static constexpr std::uintptr_t kRVA_CruiseDistance   = 0x5EB04C4;  // int - cruise duration scaler
    static constexpr std::uintptr_t kRVA_CruiseOrient0    = 0x5F29FF0;  // qword 1/4 of orientation/cruise vec
    static constexpr std::uintptr_t kRVA_CruiseOrient1    = 0x5F29FF8;
    static constexpr std::uintptr_t kRVA_CruiseOrient2    = 0x5F2A000;
    static constexpr std::uintptr_t kRVA_CruiseOrient3    = 0x5F2A008;
    static constexpr std::uintptr_t kRVA_CruiseContextPtr = 0x5FB4B70;  // qword - DAT_145FBC8F0 cruise sink-context smart-ptr
    static constexpr std::uintptr_t kRVA_CruiseContext2   = 0x5FB4B68;  // 2nd cruise smart-ptr (alt-exit only)
    static constexpr std::uintptr_t kRVA_CruiseExitAnimVA = 0x61DD738;  // DAT_1461E51F0+0x2D8 - exit-anim active flag
    static constexpr std::uintptr_t kRVA_CruiseDuration2  = 0x5EB07DC;  // float - cruise duration counter (FUN_1413FEBA0)
    static constexpr std::uintptr_t kRVA_CruiseTimeSubVal = 0x5FBF9A8;  // float - per-tick decrement
    // Phase 4 Step 7 - additional state fields for arrival UI sync.
    static constexpr std::uintptr_t kRVA_CruiseArrivalState = 0x5EB04B4;  // uint32 - state machine value, source of DAT_145EB84D4 at FUN_1413FA140
    static constexpr std::uintptr_t kRVA_CruiseUiSelector   = 0x5EAAA97;  // uint8 - selects ShipHud_OnCruiseArrival vs ShipHud_OnEndCruiseMode
    // Phase 4 diag - cruise assoc-form globals (kASTP, formType 0x8B). Set by
    // host's resolver during cruise; client's force-ENTER skips the resolver
    // so these likely stay 0/stale on client. Read in FUN_1413FEBA0's arrival
    // final block + passed to FUN_141A95550 (script event fire).
    static constexpr std::uintptr_t kRVA_CruiseAssocForm0 = 0x5EA2B00;  // DAT_145EAA8C0
    static constexpr std::uintptr_t kRVA_CruiseAssocForm1 = 0x5EAA8C8;  // DAT_145EAA8C8
    static constexpr std::uintptr_t kRVA_CruiseAssocForm2 = 0x5EA2A50;  // DAT_145EAA810
    static constexpr std::uintptr_t kRVA_CruiseAssocForm3 = 0x5EA2AA8;  // DAT_145EAA868
    // Periodic snapshot during cruise (when byte=1), 1Hz.
    static std::atomic<std::uint64_t> g_lastCruiseSnapshotMs{ 0 };
    static constexpr std::uint64_t kCruiseSnapshotIntervalMs = 1000;
    // L.5 Phase 4 Step 11 - continuous orient broadcast. Memory-diff (Step 10)
    // showed DAT_145F31DA0/DA8/DB0 (3 doubles) evolve on host but stagnate on
    // client between arrivalInit/EXIT events. Likely drives planet rendering.
    // Host broadcasts current values @5Hz during active cruise; client writes
    // them directly to the 3 globals (per-tick math doesn't write here so the
    // write sticks).
    static std::atomic<std::uint64_t> g_lastCruiseOrientSendMs{ 0 };
    static constexpr std::uint64_t kCruiseOrientIntervalMs = 200;  // 5Hz
    // L.5 Phase 4 Step 12 - client-side marker re-eval. Memory diff (Step 10)
    // showed marker mgr's "current parent body" tracking @ 0x1461E2350-168
    // doesn't update on client during cruise (lazy-init singleton at +0x160
    // never triggered). After arrivalInit applies on client we wait 500ms
    // then call FUN_141A1ED40(1) to force a marker array re-eval. We also
    // fire it periodically @1Hz mid-cruise on client.
    // Earlier attempt called this post-EXIT and SEH'd because EXIT cleanup
    // had already cleared marker mgr state. Mid-cruise the state is intact.
    static std::atomic<std::uint64_t> g_markerNotifyDueMs   { 0 };
    static std::atomic<std::uint64_t> g_lastMarkerNotifyMs  { 0 };
    static constexpr std::uint64_t kMarkerNotifyDeferMs    = 500;
    static constexpr std::uint64_t kMarkerNotifyPeriodMs   = 1000;
    // Step 13 - forward-declared here so PollCruiseActiveByte can reset on
    // cruise ENTER. Definition stays inside the namespace block lower down.
    static std::atomic<bool> g_step13FiredThisCruise{ false };
    static std::atomic<bool> g_step13Fatal{ false };

    // L.5 Phase 4 Step 14 - manual resolver call on client. The cruise ENTER
    // fn (FUN_1413FDC90) skips the resolver (FUN_1413FDFD0) when force=true.
    // The resolver populates engine state read by planet rendering subsystem
    // (POI tracking + nearest-target lookup). On client we use force=true so
    // this state stays uninitialized → no planet rendering. Step 14 calls the
    // resolver manually after force-enter to run the skipped setup.
    static constexpr std::uintptr_t kRVA_CruiseResolverFn = 0x13FDE00;
    static std::atomic<bool> g_step14Fatal{ false };

    // L.5 Phase 4 Step 15 - bracket cruise toggle FUN_1413FDC90 with pre/post
    // memory dumps to capture EXACTLY what the engine writes during the cruise
    // enter/exit transition. Hooks both force=true (client) and force=false
    // (host natural pipeline). Diff PRE→POST delta on each side to find:
    // what the natural enter populates that force=true skips
    // the bytes we miss on client → next sync targets
    using CruiseToggleHookFn = void(__fastcall*)(bool);
    static CruiseToggleHookFn g_origCruiseToggle{ nullptr };
    struct PendingCruiseOrient {
        std::atomic<bool>          armed{ false };
        std::atomic<std::uint64_t> bitsX{ 0 };
        std::atomic<std::uint64_t> bitsY{ 0 };
        std::atomic<std::uint64_t> bitsZ{ 0 };
        std::atomic<std::uint64_t> receivedAtMs{ 0 };
    };
    static PendingCruiseOrient g_pendingCruiseOrient;
    // L.5 Phase 4 Step 10 - memory-diff dump. Higher-rate (10Hz) hex dump of
    // ~10 cruise-related memory regions, so we can diff host vs client and
    // identify globals that evolve on host but stagnate on client during
    // cruise. Drives the search for the missing planet-rendering /
    // celestial-body / NPC-ship sync state.
    static std::atomic<std::uint64_t> g_lastCruiseMemDumpMs{ 0 };
    static constexpr std::uint64_t kCruiseMemDumpIntervalMs = 100;

    // L.5 Phase 1.5 - generic event-source vtable first-seen logger. The
    // pre-existing CommonLibSF cruise vtables didn't match (Phase 1 result),
    // so the post-patch cruise system likely uses different vtables. Log
    // EVERY unique event-source vtable on first occurrence with the module
    // RVA + hit count. User toggles cruise during a tagged time window and
    // we diff against pre-cruise baseline to identify cruise-specific sources.
    static constexpr std::size_t          kEvtDiscoMaxUnique = 256;
    static std::atomic<std::size_t>       g_evtDiscoUniqueCnt{ 0 };
    static std::uintptr_t                 g_evtDiscoUniqueVtbls[kEvtDiscoMaxUnique]{};
    static std::atomic<std::uint64_t>     g_evtDiscoTotalLogged{ 0 };
    static std::atomic<bool>              g_evtDiscoEnabled{ true };

    static bool EvtDiscoIsKnown(std::uintptr_t a_vt)
    {
        const std::size_t n = g_evtDiscoUniqueCnt.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n && i < kEvtDiscoMaxUnique; ++i) {
            if (g_evtDiscoUniqueVtbls[i] == a_vt) return true;
        }
        return false;
    }
    static bool EvtDiscoRemember(std::uintptr_t a_vt)
    {
        const std::size_t idx = g_evtDiscoUniqueCnt.fetch_add(1);
        if (idx < kEvtDiscoMaxUnique) {
            g_evtDiscoUniqueVtbls[idx] = a_vt;
            return true;
        }
        return false;
    }

    // V4 Fire-Probe state - dedup unique event-source-vtable per session, log
    // payload + context up to N times. User test scenario: 5x left-click +
    // 5x right-click in pilot seat, then 1-2x on-foot fire for control.
    static constexpr std::size_t   kFireProbeMaxUnique     = 64;
    static constexpr std::uint64_t kFireProbeMaxLogs       = 200;
    static std::atomic<std::size_t>    g_fireProbeUniqueCnt{ 0 };
    static std::uintptr_t              g_fireProbeUniqueVtbls[kFireProbeMaxUnique]{};
    static std::atomic<std::uint64_t>  g_fireProbeMatchCount{ 0 };
    static std::atomic<std::uint64_t>  g_fireProbeLogCount{ 0 };

    static bool FireProbeIsKnownVtable(std::uintptr_t a_vt)
    {
        const std::size_t n = g_fireProbeUniqueCnt.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n && i < kFireProbeMaxUnique; ++i) {
            if (g_fireProbeUniqueVtbls[i] == a_vt) return true;
        }
        return false;
    }
    static void FireProbeRememberVtable(std::uintptr_t a_vt)
    {
        const std::size_t idx = g_fireProbeUniqueCnt.fetch_add(1);
        if (idx < kFireProbeMaxUnique) {
            g_fireProbeUniqueVtbls[idx] = a_vt;
        }
    }

    // Dispatcher layout: +0x00 vtable, +0x08 ptr-to-payload-ptr.
    // Returns the payload byte pointer, or nullptr on access failure.
    static std::uint8_t* SafeGetPayloadFromDispatcher(void* a_dispatcher)
    {
        if (!a_dispatcher) return nullptr;
        __try {
            std::uintptr_t ptrToPayloadPtr =
                *reinterpret_cast<std::uintptr_t*>(
                    reinterpret_cast<std::uint8_t*>(a_dispatcher) + 8);
            if (!ptrToPayloadPtr) return nullptr;
            std::uintptr_t payloadPtr =
                *reinterpret_cast<std::uintptr_t*>(ptrToPayloadPtr);
            if (!payloadPtr) return nullptr;
            return reinterpret_cast<std::uint8_t*>(payloadPtr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    static void SafeReadPayload(std::uint8_t* a_dst, std::uint8_t* a_src,
                                std::size_t a_size)
    {
        __try {
            std::memcpy(a_dst, a_src, a_size);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    static void SafeWritePayload(std::uint8_t* a_dst, const std::uint8_t* a_src,
                                 std::size_t a_size)
    {
        __try {
            std::memcpy(a_dst, a_src, a_size);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ----------------------------------------------------------------------
    // V3 iter5 - Havok cache hook (bhkCharacterStateSpaceship vt[8])
    // ----------------------------------------------------------------------
    //
    // RE finding (RE_HavokVt8 + RE_HavokPropagator):
    //   FUN_1423ba880 (RVA 0x23B8270) is bhkCharacterStateSpaceship vt[8],
    //   called per-physics-tick by the engine's rigid-body iterator. It:
    //     1. Reads havok rotation/translation from param_2 (havok body) via
    //        param_2->vt[+0x270] and param_2->vt[+0x258].
    //     2. Caches them at param_1+0xa0..+0xc0 (3× vec4 = 3x3 rotation, 4th
    //        component masked) and +0xd0 (translation).
    //     3. Calls FUN_1423ba320 which performs further matrix math and
    //        writes back to +0xd0/+0xe0.
    //   The cache is consumed by a downstream "physics-to-NiNode sync" pass
    //   that writes ship.data3D->world.rotate from this cache.
    //
    // Strategy: hook FUN_1423ba880, run original, then override the rotation
    // cache (+0xa0/+0xb0/+0xc0) with host's transmitted values. Engine's
    // downstream sync then propagates our values to the ship NiNode → render
    // sees them. We DO NOT touch translation (+0xd0) - ship REFR is anchor at
    // (0,0,0); writing translation here could break collision/skybox systems.
    //
    // Bidirectional: only override on the PASSIVE side (g_isActivePilot=false).
    // Active side runs normal havok flow → its ship physics outputs to network.

    static constexpr std::uintptr_t kRVA_BhkSpaceshipVt8 = 0x23B8270;
    using BhkSpaceshipVt8Fn = void(__fastcall*)(void*, void*);
    static BhkSpaceshipVt8Fn g_origBhkSpaceshipVt8 = nullptr;
    static std::atomic<std::uint64_t> g_havokOverrideCount{ 0 };
    static std::atomic<std::uint64_t> g_lastHavokLogMs{ 0 };

    // V3 iter8 - direct rotation/state read/write on havok body's nested data.
    //
    // RE finding (RE_HavokRotation on vftbl 0x4D65D60):
    //   vt[+0x258] getter (FUN_1423bfba0) reads 64 bytes (8 qwords) from
    //     body[+0x4D0]+0x50..+0x8F  (4x vec4 = 4x4 matrix or hknpMotionState).
    //   vt[+0x260] setter calls FUN_1423bae50 which memcpy's 64 bytes back.
    //   This is what vt[8] passes `local_b8` (48 bytes) to via vt[+0x260] -
    //   so this 64-byte block IS the ship's transform/motion state that drives
    //   the physics body downstream.
    //
    // iter7 mistake: tried vt[+0x270] which is POSITION (16B), seen as
    //   "quat=(10.6, 31.5, 0.99, 0)" - XYZ coords, not a quat.

    static constexpr std::uintptr_t kHavokBodyDataPtrOffset    = 0x4D0;
    static constexpr std::uintptr_t kHavokStateOffsetInData    = 0x50;
    static constexpr std::size_t    kHavokStateSize            = 64;

    static bool SafeReadHavokState(void* a_havokBody, float a_out[16])
    {
        if (!a_havokBody) return false;
        __try {
            auto* p = reinterpret_cast<std::uint8_t*>(a_havokBody);
            auto data = *reinterpret_cast<std::uintptr_t*>(p + kHavokBodyDataPtrOffset);
            if (!data) return false;
            std::memcpy(a_out, reinterpret_cast<void*>(data + kHavokStateOffsetInData),
                        kHavokStateSize);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    static void SafeWriteHavokState(void* a_havokBody, const float a_in[16])
    {
        if (!a_havokBody) return;
        __try {
            auto* p = reinterpret_cast<std::uint8_t*>(a_havokBody);
            auto data = *reinterpret_cast<std::uintptr_t*>(p + kHavokBodyDataPtrOffset);
            if (!data) return;
            std::memcpy(reinterpret_cast<void*>(data + kHavokStateOffsetInData),
                        a_in, kHavokStateSize);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Type-guard for SafeCommitHavokTransform: vtable of the body vt[8] processes
    // (the known-good ship-flight controller). Recorded LIVE in HookedBhkSpaceshipVt8
    // so no hardcoded vtable RVA. The Tick body-commit only fires the proxy-controller
    // SetTransform when the target body is this SAME type - otherwise it would call
    // the wrong slot-0x260 function (the rigidbody controller's has a different
    // signature) on a mismatched body (e.g. g_playerBhkBody snapping to a station).
    static std::atomic<std::uintptr_t> g_shipBodyVtbl{ 0 };

    // 1.16.244 carry-fix. On 244 the cache-only memcpy (SafeWriteHavokState)
    // no longer makes the physics body move, so a STANDING/unseated client is not
    // carried by the host's ship (symptom: "only the quest-marker moves"). The
    // engine's body-data SetTransform writes the SAME +0x50 block AND commits it into
    // the live hknp world (calls FUN_14235dce0 under the Havok world write-lock) -
    // that world-commit is exactly what carries anything standing on the body.
    // We call it directly on the data block (*(body+0x4D0)), the same block
    // SafeWriteHavokState already targets, so no vtable-type ambiguity.
    // RE (Ghidra Starfield244): FUN_1423b8840 = data-block SetTransform, REL::ID 125839
    // (236 RVA 0x23BAE50); signature (dataBlock, ptr-to-8-qwords / 16 floats). Writes
    // dataBlock+0x50..+0x88 then commits if dataBlock+0x160 != 0. SEH-guarded; no-op on
    // null. Rotation only - host translate stays the (0,0,0) cockpit anchor.
    static constexpr std::uintptr_t kRVA_HavokBodySetTransform = 0x23B8840;  // REL::ID 125839
    using HavokBodySetTransformFn = void(__fastcall*)(void* /*dataBlock*/, const void* /*xform16*/);
    static void SafeCommitHavokTransform(void* a_havokBody, const float a_in[16])
    {
        if (!a_havokBody) return;
        __try {
            auto* p = reinterpret_cast<std::uint8_t*>(a_havokBody);
            auto data = *reinterpret_cast<std::uintptr_t*>(p + kHavokBodyDataPtrOffset);
            if (!data) return;
            // Type-guard: FUN_1423b8840 is the bhkCharProxyController data-block
            // SetTransform. A different controller type has a different slot-0x260
            // function + signature, so only fire the commit when this body is the
            // SAME type as the known-good vt[8] ship body. Otherwise fall back to the
            // cache-only memcpy (old behavior - never worse than before).
            const std::uintptr_t vt    = *reinterpret_cast<std::uintptr_t*>(p);
            const std::uintptr_t known = g_shipBodyVtbl.load(std::memory_order_acquire);
            if (known != 0 && vt == known) {
                auto fn = reinterpret_cast<HavokBodySetTransformFn>(
                    REL::Offset{ kRVA_HavokBodySetTransform }.address());
                fn(reinterpret_cast<void*>(data), a_in);   // memcpy + hknp world-commit
            } else {
                std::memcpy(reinterpret_cast<void*>(data + kHavokStateOffsetInData),
                            a_in, kHavokStateSize);          // cache-only fallback
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static std::atomic<std::uint64_t> g_v3iter7SendCount{ 0 };
    static std::atomic<std::uint64_t> g_v3iter7ApplyCount{ 0 };
    static std::atomic<std::uint64_t> g_lastIter7LogMs{ 0 };

    // V3 iter9 - param_1 (bhkState) layout dump for player-ship identification.
    static std::atomic<bool> g_param1Dumped{ false };

    // V3 iter10 - outgoing havok state buffer is declared earlier in the
    // anonymous namespace so MaybeSyncShipTransform can use it.
    // Helpers to publish/consume - kept in non-__try functions because
    // std::lock_guard introduces unwinding which conflicts with __try.
    static void PublishOutgoingHavokState(const float a_in[16])
    {
        std::lock_guard<std::mutex> lk(g_outgoingHavokStateMutex);
        std::memcpy(g_outgoingHavokState, a_in, sizeof(g_outgoingHavokState));
        g_outgoingHavokStateValid.store(true, std::memory_order_release);
    }
    static bool ConsumeOutgoingHavokState(float a_out[16])
    {
        if (!g_outgoingHavokStateValid.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lk(g_outgoingHavokStateMutex);
        std::memcpy(a_out, g_outgoingHavokState, sizeof(g_outgoingHavokState));
        return true;
    }

    // === V3 iter13 - Active-pilot detection via per-body motion EMA ===
    //
    // For each unique havok-body pointer seen as `a_param2`, track row3
    // (translation) across calls and accumulate a per-frame motion EMA. The
    // pilot side's body integrates real flight input → high EMA. Passive
    // side's body in zero-G drifts ~0 → low EMA. The dual-pilot bug iter11/12
    // hit (both sides flagging active) is avoided because passive's local
    // engine produces no acceleration without input, so its EMA stays low.
    //
    // Apply-feedback (passive sees host's motion echoed via our own writes)
    // is suppressed by a short cooldown after each successful write - during
    // cooldown we force passive and reset the EMA baselines.
    static constexpr int            kIter13SlotCount       = 4;
    static constexpr float          kIter13EmaAlpha        = 0.15f;
    // Squared-distance threshold (avoids std::sqrt include). 0.0025 = (0.05)^2.
    static constexpr float          kIter13ActiveThreshSq  = 0.0025f;
    static constexpr std::uint64_t  kIter13ActiveHoldMs    = 3000;
    static constexpr std::uint64_t  kIter13ApplyCooldownMs = 200;
    // Max age of cached havok body pointer - used by Tick to keep applying
    // received transforms when vt[8] stops firing (e.g. local player not
    // seated → engine stops physics-stepping the ship → vt[8] silent).
    static constexpr std::uint64_t  kIter13TickBodyMaxAgeMs = 30000;

    // M.1 V2 - player-ship body identification.
    // The vt[8] hook fires for ALL ships in the simulation, including NPCs.
    // Without this gate, the passive apply path writes the host's player-
    // ship transform to *every* body on the client → all NPC ships cluster
    // at the host's player position. We identify the player's body by
    // position-matching against player.GetSpaceship().data.location every
    // ~200ms (cached to keep the hot path cheap).
    static std::atomic<void*>          g_playerBhkBody{ nullptr };
    static std::atomic<std::uint64_t>  g_psPosCacheMs{ 0 };
    static std::atomic<bool>           g_psPosValid{ false };
    static std::atomic<float>          g_psPosX{ 0.0f };
    static std::atomic<float>          g_psPosY{ 0.0f };
    static std::atomic<float>          g_psPosZ{ 0.0f };
    // Squared-distance threshold for "close enough to be the player ship".
    // Ships are large (50-100m); 200m^2 = 40000 covers any plausible body
    // origin within the ship volume.
    static constexpr float kPSMatchThreshSq = 40000.0f;
    // near a station/POI several ship bodies cluster within the match
    // threshold. Pick the CLOSEST (min d2) per 200ms window instead of last-wins,
    // else g_playerBhkBody flickers onto an NPC/station body and the flight apply
    // drives the STATION → ship fuses with it + physics stutter (unplayable).
    static std::atomic<float> g_psMatchBestD2{ 3.4e38f };
    static std::atomic<void*> g_psMatchBestBody{ nullptr };
    // v2 - sticky lock. Logs proved two bodies both report d2=0 and
    // FLICKER every 200ms window: once the flight apply writes the host
    // transform to the wrong body, that body sits exactly at g_psPos too, so
    // min-d2 can no longer tell them apart (apply-feedback contamination).
    // Fix: once a body is locked, KEEP it as long as it keeps matching; never
    // switch to another body that merely also matches. The wrong body then
    // stops receiving the apply, drifts back to its true (far) position, and
    // stops competing. Release only after the held body is absent for several
    // windows (ship change / cell reload / pilot handover).
    static std::atomic<bool> g_psStickyHeld{ false };   // held body matched this window
    static std::atomic<int>  g_psStickyMiss{ 0 };       // consecutive windows held body absent
    static constexpr int     kPSStickyReleaseWindows = 3;

    // M.1 V2 iter2 - NPC ship state cache.
    // We track up to 16 unique non-player bodies seen via vt[8]. Each slot
    // caches the body's last havok state. A 1Hz Tick broadcasts active slots
    // (lastUpdate < 5s ago) as PKT_NPC_SHIP_BATCH.
    //
    // Receiver caches incoming batches and matches each remote entry to a
    // local body via translation distance, then writes the state.
    struct NpcShipSlot {
        void*         body          = nullptr;
        float         lastState[16] = {};
        std::uint64_t lastUpdateMs  = 0;
    };
    static constexpr int  kNpcShipSlotCount = 16;
    static NpcShipSlot    g_npcShipSlots[kNpcShipSlotCount];
    static std::mutex     g_npcShipSlotsMtx;
    static std::atomic<std::uint64_t> g_lastNpcBatchSendMs{ 0 };
    static constexpr std::uint64_t    kNpcBatchIntervalMs = 100;   // 10Hz
    static constexpr std::uint64_t    kNpcSlotStaleMs     = 5000;

    // Client-side: cache of recently-seen local NPC bodies + their last-
    // observed translation. Used to match incoming remote entries.
    struct NpcLocalBody {
        void*         body              = nullptr;
        float         pos[3]            = { 0, 0, 0 };
        std::uint64_t seenMs            = 0;
        // M.1 V2 iter2d - cached "to-write" state from latest match.
        // The vt[8] hook writes this every physics step so engine physics
        // can't drift the body away (1Hz batch overwrites would otherwise
        // be steamrolled by 60Hz local sim).
        bool          hasPendingApply   = false;
        float         pendingState[16]  = {};
        std::uint64_t pendingMs         = 0;
        // M.1 V2 iter2g - snapshot interpolation. We keep prev + current
        // target snapshots and lerp translation between them based on
        // wall-clock time. Eliminates the 10Hz teleport oscillation that
        // pure target-snap produced.
        float         prevState[16]     = {};
        std::uint64_t prevMs            = 0;
        bool          hasPrev           = false;
    };
    static NpcLocalBody  g_npcLocalBodies[kNpcShipSlotCount];
    static std::mutex    g_npcLocalBodiesMtx;
    // Pending received batch - drained by Tick to apply transforms.
    struct PendingNpcEntry {
        std::uint32_t handleHash;
        float         havokState[16];
    };
    static std::vector<PendingNpcEntry> g_pendingNpcBatch;
    static std::mutex                   g_pendingNpcBatchMtx;
    static std::atomic<std::uint64_t>   g_pendingNpcBatchMs{ 0 };
    static constexpr std::uint64_t      kPendingNpcBatchStaleMs = 3000;
    // Match radius (squared) - remote ↔ local body. Encounter ships drift
    // independently per-machine; 5km radius captures more proc-gen variance
    // without false-matching unrelated ships (typical inter-ship spacing in
    // encounters is much larger than 5km).
    static constexpr float kNpcMatchRadiusSq = 25'000'000.0f;  // 5km^2

    struct Iter13Slot {
        void* body         = nullptr;
        float lastPos[3]   = { 0, 0, 0 };
        bool  lastPosValid = false;
        float motionEmaSq  = 0.0f;
    };
    static Iter13Slot                  g_iter13Slots[kIter13SlotCount];
    static std::atomic<std::uint64_t>  g_iter13LastApplyMs{ 0 };
    static std::atomic<std::uint64_t>  g_iter13LastActiveMs{ 0 };
    static std::atomic<std::uint64_t>  g_iter13DetectCount{ 0 };
    static std::atomic<std::uint64_t>  g_iter13LastDetectLogMs{ 0 };
    // Cached body pointer (and last-seen timestamp) from passive vt[8]
    // applies - used by Tick fallback when vt[8] stops firing.
    static std::atomic<void*>          g_iter13CachedApplyBody{ nullptr };
    static std::atomic<std::uint64_t>  g_iter13CachedApplyBodyMs{ 0 };
    static std::atomic<std::uint64_t>  g_iter13TickApplyCount{ 0 };
    static std::atomic<std::uint64_t>  g_iter13LastTickApplyLogMs{ 0 };

    // V3 iter14 - deterministic pilot signal. Iter13 motion-only detection
    // can't recognize a parked-in-space pilot (no motion → no EMA → active=0).
    // Combine network role + IsInSpace + GetSpaceship + occupiedFurniture
    // handle to definitively identify the local pilot, independent of motion.
    //
    // L.3 V4 - pilot handover. Either side can pilot when seated. If both
    // are seated simultaneously: both broadcast, both stay in active mode
    // (passive apply doesn't run when active=true), each sees own engine
    // sim. When one stands → starts applying other's transform → other's
    // input drives both ships. Asymmetric ownership emerges naturally from
    // the seated-state alone.
    //
    // occupiedFurniture handle layout (verified in VehicleSync.cpp:54-67):
    //   Actor + 0x228 = AIProcess*
    //   AIProcess + 0x08 = MiddleHighProcessData*
    //   MiddleHighProcessData + 0x428 = handle (uint32, 0 = not seated)
    static bool IsLocalDeterministicPilot()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return false;
        // L.3 V4 - IsHost gate removed. Both peers can pilot.

        // L.7 post-bridge guard. Engine load places client into
        // host's saved pilot chair (full furn binding intact), which
        // would normally trigger detPilot=1 below → client broadcasts
        // ship state in parallel with host → sync conflict. While the
        // post-bridge input lockout is active, refuse to claim pilot
        // regardless of seated state. The lockout auto-expires after
        // 30s; if the user has unseated by then (furn handle becomes 0
        // naturally), detPilot will return false anyway.
        const std::uint64_t lockoutUntil =
            g_postBridgeInputLockoutUntilMs.load(std::memory_order_acquire);
        if (lockoutUntil != 0 && ::GetTickCount64() < lockoutUntil) {
            return false;
        }

        // L.3 V4.1 - host-priority tiebreaker for the DUAL-PILOT case. When BOTH
        // peers sit in their (same-FormID) cockpit chair, the seated-only test
        // below makes BOTH claim pilot → both go active → an active pilot never
        // runs the passive apply, so the client stops applying the host's flight
        // and its own ship body sits frozen = "no ship sync while the client is
        // in the pilot seat; stand up and it syncs again" (verified 2026-06-21
        // logs: host row0 rotating live, client row0 frozen, both active=1).
        // Resolve in favour of the HOST: if we are the client AND the host is
        // actively piloting (a fresh PKT_SHIP_TRANSFORM is arriving), YIELD -
        // return false so we stay passive and apply the host's flight even while
        // seated. If the host is NOT sending (not piloting), we don't yield.
        if (!net.IsHost()) {
            const bool armed =
                g_pendingShipTransform.armed.load(std::memory_order_acquire);
            const std::uint64_t recvAt =
                g_pendingShipTransform.receivedAtMs.load(std::memory_order_acquire);
            if (armed && recvAt != 0 && (::GetTickCount64() - recvAt) <= kV3StaleMs) {
                return false;  // host is the active pilot → client yields
            }
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        bool inSpace = false;
        bool hasShip = false;
        bool seated  = false;
        __try {
            inSpace = player->IsInSpace(true);
            hasShip = (player->GetSpaceship(true) != nullptr);
            auto* a = reinterpret_cast<std::uint8_t*>(player);
            auto* proc = *reinterpret_cast<std::uint8_t**>(a + 0x228);
            if (proc) {
                auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x08);
                if (mh) {
                    seated = (*reinterpret_cast<std::uint32_t*>(mh + 0x428) != 0);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        // L.3 V4 - HUD fallback removed (it persisted after standing up,
        // keeping host marked as pilot indefinitely → broadcast continued →
        // client's seat seemed occupied → client couldn't sit). The engine's
        // seated check (furnHandle != 0) is correct on host AND on client
        // when client actually sits in the seat - the earlier "client sits
        // but seated=0" observation was likely because host's clone still
        // occupied the seat in client's world.
        return inSpace && hasShip && seated;
    }

    // L.3 V4 diagnostic - log piloting flags from Tick (game thread, not
    // throttled by iter13 cooldown). 1Hz rate limit.
    static std::atomic<std::uint64_t> g_v4DiagLastLogMs{ 0 };
    static void V4PilotDiagTick(std::uint64_t a_now)
    {
        const std::uint64_t lastLog = g_v4DiagLastLogMs.load(std::memory_order_acquire);
        if ((a_now - lastLog) < 1000) return;
        g_v4DiagLastLogMs.store(a_now, std::memory_order_release);

        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        bool          inSpace      = false;
        bool          hasShip      = false;
        bool          seated       = false;
        std::uint32_t furnHandle   = 0;
        std::uintptr_t shipPtr     = 0;
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                inSpace = player->IsInSpace(true);
                auto* ship = player->GetSpaceship(true);
                hasShip = (ship != nullptr);
                shipPtr = reinterpret_cast<std::uintptr_t>(ship);
                auto* a = reinterpret_cast<std::uint8_t*>(player);
                auto* proc = *reinterpret_cast<std::uint8_t**>(a + 0x228);
                if (proc) {
                    auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x08);
                    if (mh) {
                        furnHandle = *reinterpret_cast<std::uint32_t*>(mh + 0x428);
                        seated = (furnHandle != 0);
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        const auto modelPtr = g_modelPtr.load(std::memory_order_acquire);
        PLOG_INFO(
            "[PULSAR] L.3 V4 pilotDiag: isHost={} inSpace={} hasShip={} seated={} "
            "shipPtr=0x{:X} furn=0x{:08X} hud=0x{:X} detPilot={} active={}"sv,
            net.IsHost() ? 1 : 0, inSpace ? 1 : 0, hasShip ? 1 : 0, seated ? 1 : 0,
            shipPtr, furnHandle, modelPtr,
            (inSpace && hasShip && seated) ? 1 : 0,
            g_isActivePilot.load(std::memory_order_acquire) ? 1 : 0);
    }

    static Iter13Slot* Iter13FindOrAllocSlot(void* a_body)
    {
        for (auto& s : g_iter13Slots) if (s.body == a_body) return &s;
        for (auto& s : g_iter13Slots) if (s.body == nullptr) {
            s.body = a_body;
            return &s;
        }
        return nullptr;
    }

    static void Iter13ResetTracking()
    {
        for (auto& s : g_iter13Slots) {
            s.motionEmaSq  = 0.0f;
            s.lastPosValid = false;
        }
    }

    static bool Iter13SafeReadBodyPos(void* a_havokBody, float a_out[3])
    {
        if (!a_havokBody) return false;
        __try {
            auto* p    = reinterpret_cast<std::uint8_t*>(a_havokBody);
            auto  data = *reinterpret_cast<std::uintptr_t*>(p + kHavokBodyDataPtrOffset);
            if (!data) return false;
            // row3 lives at data + 0x50 + 0x30 (+0x80) - translation vec4.
            auto* pos = reinterpret_cast<float*>(
                data + kHavokStateOffsetInData + 0x30);
            a_out[0] = pos[0];
            a_out[1] = pos[1];
            a_out[2] = pos[2];
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static void Iter13DetectAndSetActive(void* a_body, std::uint64_t a_now)
    {
        // Apply-cooldown: we just wrote received state → we're passive. Skip
        // motion detection (the body row3 we'd read next contains host's
        // deltas, which would falsely register as our own motion).
        const std::uint64_t lastApply = g_iter13LastApplyMs.load(std::memory_order_acquire);
        if (lastApply != 0 && (a_now - lastApply) < kIter13ApplyCooldownMs) {
            Iter13ResetTracking();
            g_isActivePilot.store(false, std::memory_order_release);
            return;
        }

        // V3 iter14 - deterministic pilot signal. Captured here, applied at
        // the end as an OR-override so motion EMA still updates for diag.
        const bool detPilot = IsLocalDeterministicPilot();
        if (detPilot) {
            g_iter13LastActiveMs.store(a_now, std::memory_order_release);
        }

        if (!a_body) {
            // No body this call - apply detPilot decision and exit.
            if (detPilot) g_isActivePilot.store(true, std::memory_order_release);
            return;
        }
        Iter13Slot* slot = Iter13FindOrAllocSlot(a_body);
        if (!slot) return;

        float pos[3];
        if (!Iter13SafeReadBodyPos(a_body, pos)) return;

        if (slot->lastPosValid) {
            const float dx = pos[0] - slot->lastPos[0];
            const float dy = pos[1] - slot->lastPos[1];
            const float dz = pos[2] - slot->lastPos[2];
            const float dSq = dx * dx + dy * dy + dz * dz;
            slot->motionEmaSq = (1.0f - kIter13EmaAlpha) * slot->motionEmaSq
                              + kIter13EmaAlpha * dSq;
        }
        slot->lastPos[0]   = pos[0];
        slot->lastPos[1]   = pos[1];
        slot->lastPos[2]   = pos[2];
        slot->lastPosValid = true;

        // Activity = max EMA across slots > threshold (player ship usually
        // has higher EMA than NPC drift).
        float maxEmaSq = 0.0f;
        for (auto& s : g_iter13Slots) {
            if (s.motionEmaSq > maxEmaSq) maxEmaSq = s.motionEmaSq;
        }

        // M.1 V2 iter2e - only trust detPilot for active determination.
        // Motion EMA was the primary signal pre-iter14, but in space encounters
        // NPC ships drive maxEmaSq > threshold continuously (engine physics
        // simulates pirates at 60Hz), causing active=true on client even
        // when client isn't piloting. detPilot already encodes the right
        // signal (IsHost + IsInSpace + GetSpaceship + seated) - trust it.
        // Keep maxEmaSq logged only for diagnostics.
        if (maxEmaSq > kIter13ActiveThreshSq) {
            g_iter13LastActiveMs.store(a_now, std::memory_order_release);
        }
        const bool active = detPilot;
        g_isActivePilot.store(active, std::memory_order_release);

        // Diagnostic log (1 Hz).
        const std::uint64_t cnt     = g_iter13DetectCount.fetch_add(1) + 1;
        const std::uint64_t lastLog = g_iter13LastDetectLogMs.load(std::memory_order_acquire);
        if ((a_now - lastLog) >= 1000) {
            g_iter13LastDetectLogMs.store(a_now, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] V3 iter13 detect: cnt={} maxEmaSq={:.5f} active={} "
                "detPilot={} applyCooldownMs={}"sv,
                cnt, maxEmaSq, active ? 1 : 0, detPilot ? 1 : 0,
                lastApply ? (a_now - lastApply) : 0);
        }
    }

    // M.1 V2 iter2 helpers - keep std::lock_guard out of __try-using functions
    // (C2712 forbids stack-unwinding objects in functions with structured EH).
    static void UpdateLocalNpcBody(void* a_body, const float a_pos[3],
                                    std::uint64_t a_now)
    {
        std::lock_guard<std::mutex> lock(g_npcLocalBodiesMtx);
        int slotIdx   = -1;
        int oldestIdx = 0;
        std::uint64_t oldestMs = g_npcLocalBodies[0].seenMs;
        for (int i = 0; i < kNpcShipSlotCount; ++i) {
            if (g_npcLocalBodies[i].body == a_body) { slotIdx = i; break; }
            if (g_npcLocalBodies[i].body == nullptr && slotIdx < 0) slotIdx = i;
            if (g_npcLocalBodies[i].seenMs < oldestMs) {
                oldestMs  = g_npcLocalBodies[i].seenMs;
                oldestIdx = i;
            }
        }
        if (slotIdx < 0) slotIdx = oldestIdx;
        // If we evict an old slot for a NEW body, clear its pending state
        // (otherwise we'd write the previous body's host-pos to the new body).
        if (g_npcLocalBodies[slotIdx].body != a_body) {
            g_npcLocalBodies[slotIdx].hasPendingApply = false;
            g_npcLocalBodies[slotIdx].pendingMs       = 0;
        }
        g_npcLocalBodies[slotIdx].body   = a_body;
        g_npcLocalBodies[slotIdx].pos[0] = a_pos[0];
        g_npcLocalBodies[slotIdx].pos[1] = a_pos[1];
        g_npcLocalBodies[slotIdx].pos[2] = a_pos[2];
        g_npcLocalBodies[slotIdx].seenMs = a_now;
    }

    // M.1 V2 iter2g - read interpolated NPC apply state for given body.
    // Snapshot interpolation: lerp translation between prev and current
    // pending target, snap rotation to current (rotation matrix lerp would
    // need re-orthonormalization, snap is fine for 10Hz batches).
    static bool TakeNpcPendingApply(void* a_body, std::uint64_t a_now,
                                     float out_state[16])
    {
        std::lock_guard<std::mutex> lock(g_npcLocalBodiesMtx);
        for (int i = 0; i < kNpcShipSlotCount; ++i) {
            if (g_npcLocalBodies[i].body != a_body) continue;
            const auto& s = g_npcLocalBodies[i];
            if (!s.hasPendingApply) return false;
            if ((a_now - s.pendingMs) > 500) return false;

            // Compute alpha for translation lerp.
            float alpha = 1.0f;
            if (s.hasPrev && s.pendingMs > s.prevMs) {
                const std::uint64_t span = s.pendingMs - s.prevMs;
                if (a_now <= s.prevMs) {
                    alpha = 0.0f;
                } else {
                    const std::uint64_t age = a_now - s.prevMs;
                    alpha = static_cast<float>(age) / static_cast<float>(span);
                    // Clamp extrapolation to 1.5 batches - smooth motion
                    // when packets are late, but don't fly off into space.
                    if (alpha > 1.5f) alpha = 1.5f;
                }
            }

            // M.1 V2 iter2h - rotation matrix lerp + Gram-Schmidt re-
            // orthonormalize. Pure element-wise lerp would distort the
            // matrix (non-orthonormal); GS recovers a clean rotation.
            // Layout: rows are at floats [0..2], [4..6], [8..10]; the [3],
            // [7], [11] paddings carry havok-internal data which we snap.
            if (s.hasPrev) {
                float r0[3], r1[3], r2[3];
                for (int k = 0; k < 3; ++k) {
                    r0[k] = s.prevState[0  + k]
                          + alpha * (s.pendingState[0  + k] - s.prevState[0  + k]);
                    r1[k] = s.prevState[4  + k]
                          + alpha * (s.pendingState[4  + k] - s.prevState[4  + k]);
                    r2[k] = s.prevState[8  + k]
                          + alpha * (s.pendingState[8  + k] - s.prevState[8  + k]);
                }
                // Normalize r0
                float l0 = std::sqrt(r0[0]*r0[0] + r0[1]*r0[1] + r0[2]*r0[2]);
                if (l0 > 1e-6f) {
                    r0[0] /= l0; r0[1] /= l0; r0[2] /= l0;
                }
                // r1 -= dot(r0,r1) * r0  → orthogonal to r0
                float d01 = r0[0]*r1[0] + r0[1]*r1[1] + r0[2]*r1[2];
                r1[0] -= d01 * r0[0]; r1[1] -= d01 * r0[1]; r1[2] -= d01 * r0[2];
                float l1 = std::sqrt(r1[0]*r1[0] + r1[1]*r1[1] + r1[2]*r1[2]);
                if (l1 > 1e-6f) {
                    r1[0] /= l1; r1[1] /= l1; r1[2] /= l1;
                }
                // r2 = r0 × r1 (right-handed) - guarantees orthonormality
                r2[0] = r0[1]*r1[2] - r0[2]*r1[1];
                r2[1] = r0[2]*r1[0] - r0[0]*r1[2];
                r2[2] = r0[0]*r1[1] - r0[1]*r1[0];

                out_state[0] = r0[0]; out_state[1] = r0[1]; out_state[2] = r0[2];
                out_state[3] = s.pendingState[3];   // padding snap
                out_state[4] = r1[0]; out_state[5] = r1[1]; out_state[6] = r1[2];
                out_state[7] = s.pendingState[7];
                out_state[8] = r2[0]; out_state[9] = r2[1]; out_state[10] = r2[2];
                out_state[11] = s.pendingState[11];

                out_state[12] = s.prevState[12]
                              + alpha * (s.pendingState[12] - s.prevState[12]);
                out_state[13] = s.prevState[13]
                              + alpha * (s.pendingState[13] - s.prevState[13]);
                out_state[14] = s.prevState[14]
                              + alpha * (s.pendingState[14] - s.prevState[14]);
            } else {
                // No prev - snap to target.
                std::memcpy(out_state, s.pendingState, 15 * sizeof(float));
            }
            out_state[15] = s.pendingState[15];
            return true;
        }
        return false;
    }

    static void UpdateHostNpcSlot(void* a_body, const float a_state[16],
                                    std::uint64_t a_now)
    {
        std::lock_guard<std::mutex> lock(g_npcShipSlotsMtx);
        int slotIdx   = -1;
        int oldestIdx = 0;
        std::uint64_t oldestMs = g_npcShipSlots[0].lastUpdateMs;
        for (int i = 0; i < kNpcShipSlotCount; ++i) {
            if (g_npcShipSlots[i].body == a_body) { slotIdx = i; break; }
            if (g_npcShipSlots[i].body == nullptr && slotIdx < 0) slotIdx = i;
            if (g_npcShipSlots[i].lastUpdateMs < oldestMs) {
                oldestMs  = g_npcShipSlots[i].lastUpdateMs;
                oldestIdx = i;
            }
        }
        if (slotIdx < 0) slotIdx = oldestIdx;
        g_npcShipSlots[slotIdx].body = a_body;
        std::memcpy(g_npcShipSlots[slotIdx].lastState, a_state, 16 * sizeof(float));
        g_npcShipSlots[slotIdx].lastUpdateMs = a_now;
    }

    static void __fastcall HookedBhkSpaceshipVt8(void* a_param1, void* a_param2)
    {
        // Always run engine's normal flow first.
        if (g_origBhkSpaceshipVt8) {
            g_origBhkSpaceshipVt8(a_param1, a_param2);
        }

        // M.1 V1 - Discovery DONE. vt[8] hook fires for ALL ships in physics
        // simulation (verified: 9-16 unique bhkCharacterStateSpaceship
        // instances during pirate encounters). param_1 has no direct REFR
        // back-link at fixed offset (NiAVObject sub-pointers visible at
        // qw3=base+0xF0 and qw37=base, but base is NiAVObject not REFR).
        // V2 will track ships by param_1 as opaque handle + position-based
        // identity matching across host/client.
        //
        // Probe code below kept gated (max 16 captures, self-disables) as
        // a development tool; can be deleted once V2 is shipped.
#ifdef PULSAR_M1_DISCOVERY_PROBE
        // M.1 V1.3 - discover NPC ships via vt[8] + auto-locate REFR back-link.
        // Strategy: for each new param_1 we see, scan its qw0..qw47 + body's
        // bw0..bw23. For each plausible heap pointer, deref and check if
        // *(p+0x28) holds a sane formID and *(p+0x44) holds a sane formType
        // matching kREFR (0x4A) or kACHR (0x4B). If yes, we've located the
        // REFR back-link and the formID is the ship's runtime FormID.
        // Cap at 16 unique param_1 to capture larger encounters.
        {
            static std::atomic<std::uintptr_t> seenParam1[16]{};
            static std::atomic<int> seenCount{ 0 };
            const std::uintptr_t p1addr = reinterpret_cast<std::uintptr_t>(a_param1);
            if (a_param1) {
                bool already = false;
                int n = seenCount.load(std::memory_order_acquire);
                if (n > 16) n = 16;
                for (int i = 0; i < n; ++i) {
                    if (seenParam1[i].load(std::memory_order_acquire) == p1addr) {
                        already = true; break;
                    }
                }
                if (!already && n < 16) {
                    int idx = seenCount.fetch_add(1, std::memory_order_acq_rel);
                    if (idx < 16) {
                        seenParam1[idx].store(p1addr, std::memory_order_release);

                        // M.1 V1.4 - scan param_1 qw0..qw31 + body bw0..bw15
                        // for REFR back-links. NO deref-of-deref (V1.3 caused
                        // crash from multi-threaded heap walks). Cross-ship
                        // dedupe: if a REFR pointer was reported for another
                        // param_1, skip it (= singleton, e.g. WorldSpace).
                        static std::atomic<std::uint64_t> seenRefrPtr[32]{};
                        static std::atomic<int>           seenRefrCount{ 0 };

                        struct RefrCandidate {
                            int           src;
                            int           offset;
                            std::uint64_t ptr;
                            std::uint32_t formID;
                            std::uint8_t  formType;
                        };
                        RefrCandidate cands[4]{};
                        int ccount = 0;

                        auto check_ptr = [&](int src, int off, std::uint64_t ptr) {
                            if (ccount >= 4) return;
                            if (ptr < 0x10000ULL || ptr >= 0x7FFFFFFFFFFFULL) return;
                            // Skip arena-aligned addresses (likely singletons,
                            // e.g. 0x...42000000 was the WorldSpace REFR).
                            if ((ptr & 0xFFFFFFu) == 0) return;
                            std::uint32_t fid = 0;
                            std::uint8_t  ft  = 0;
                            __try {
                                fid = *reinterpret_cast<std::uint32_t*>(ptr + 0x28);
                                ft  = *reinterpret_cast<std::uint8_t*>(ptr + 0x2E);
                            } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
                            // Lenient formID range - runtime IDs 0xFF-prefixed,
                            // master/plugin IDs in lower ranges.
                            if (fid == 0 || fid == 0xFFFFFFFFu) return;
                            // No formType filter - log whatever we find. The
                            // first run found ships at ft=0x4A but base form
                            // might be something else. We'll filter post-hoc
                            // from the data we collect.

                            // Cross-ship singleton check.
                            int rc = seenRefrCount.load(std::memory_order_acquire);
                            if (rc > 32) rc = 32;
                            for (int i = 0; i < rc; ++i) {
                                if (seenRefrPtr[i].load(std::memory_order_acquire) == ptr) {
                                    return;  // singleton - already reported for another ship
                                }
                            }
                            int rc2 = seenRefrCount.fetch_add(1, std::memory_order_acq_rel);
                            if (rc2 < 32) {
                                seenRefrPtr[rc2].store(ptr, std::memory_order_release);
                            }
                            cands[ccount++] = { src, off, ptr, fid, ft };
                        };

                        __try {
                            auto* p1 = reinterpret_cast<std::uint8_t*>(a_param1);
                            // Scan 0..47 - V1.3 found the REFR back-link at
                            // qw37 (+0x128), so we need at least 48 qwords.
                            std::uint64_t qw[48]{};
                            for (int i = 0; i < 48; ++i) {
                                qw[i] = *reinterpret_cast<std::uint64_t*>(p1 + i * 8);
                            }
                            PLOG_INFO(
                                "[PULSAR] M.1 vt8-discover NEW #{} param1=0x{:X} qw0-7=[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
                                idx + 1, p1addr,
                                qw[0], qw[1], qw[2], qw[3], qw[4], qw[5], qw[6], qw[7]);
                            PLOG_INFO(
                                "[PULSAR] M.1   qw32-39=[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
                                qw[32], qw[33], qw[34], qw[35], qw[36], qw[37], qw[38], qw[39]);
                            for (int i = 0; i < 48; ++i) check_ptr(0, i, qw[i]);

                            // Triple-pointer pattern: qw3 often holds
                            // (base+0xF0). Extract the base and check it.
                            // Also check the typical triplet at qw36/37/38
                            // which appears to point at base+0xA0/0x50/0x00.
                            if (qw[3] >= 0x10000ULL && qw[3] < 0x7FFFFFFFFFFFULL) {
                                std::uint64_t base = qw[3] - 0xF0;
                                check_ptr(3, 300, base);  // src=3 marks "qw3-0xF0"
                            }
                            // Triplet base candidates from qw32..qw39.
                            for (int i = 32; i < 40; ++i) {
                                if (qw[i] >= 0x10000ULL && qw[i] < 0x7FFFFFFFFFFFULL) {
                                    // Try the value as-is and minus the typical
                                    // sub-offsets we observe (0x00/0x50/0xA0).
                                    check_ptr(4, i, qw[i]);
                                    check_ptr(5, i, qw[i] & ~0xFFULL);  // round to 0x100 boundary
                                }
                            }

                            if (a_param2) {
                                auto* p2 = reinterpret_cast<std::uint8_t*>(a_param2);
                                std::uint64_t bw[16]{};
                                for (int i = 0; i < 16; ++i) {
                                    bw[i] = *reinterpret_cast<std::uint64_t*>(p2 + i * 8);
                                }
                                for (int i = 0; i < 16; ++i) check_ptr(1, i, bw[i]);
                            }
                        } __except (EXCEPTION_EXECUTE_HANDLER) {}

                        if (ccount > 0) {
                            for (int c = 0; c < ccount; ++c) {
                                PLOG_INFO(
                                    "[PULSAR] M.1   REFR-cand src={} off={} ptr=0x{:X} formID=0x{:08X} formType=0x{:02X}"sv,
                                    cands[c].src, cands[c].offset, cands[c].ptr,
                                    cands[c].formID, cands[c].formType);
                            }
                        } else {
                            PLOG_INFO("[PULSAR] M.1   no per-ship REFR candidates (after singleton filter)"sv);
                        }
                    }
                }
            }
        }
#endif  // PULSAR_M1_DISCOVERY_PROBE

        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        if (!a_param2) return;

        const std::uint64_t now = ::GetTickCount64();

        // M.1 V2 - refresh player-ship-position cache (200ms throttle).
        const std::uint64_t lastPSC = g_psPosCacheMs.load(std::memory_order_relaxed);
        if ((now - lastPSC) > 200) {
            g_psPosCacheMs.store(now, std::memory_order_relaxed);
            // v2 - sticky lock at the window boundary. If the body we already
            // hold matched this window, KEEP it (ignore any other body that also
            // matched - that's the station/contamination). Only (re)acquire the
            // closest candidate when we have no lock, or the held body has been
            // absent for kPSStickyReleaseWindows windows (ship change / handover).
            {
                const bool  held   = g_psStickyHeld.exchange(false, std::memory_order_acq_rel);
                void*       best   = g_psMatchBestBody.exchange(nullptr, std::memory_order_acq_rel);
                const float bestD2 = g_psMatchBestD2.exchange(3.4e38f, std::memory_order_acq_rel);
                void*       cur    = g_playerBhkBody.load(std::memory_order_acquire);
                if (cur != nullptr && held) {
                    // Locked body still matching → keep it. Breaks the A↔B flicker.
                    g_psStickyMiss.store(0, std::memory_order_relaxed);
                } else {
                    const bool forceAcquire = (cur == nullptr);
                    int miss = 0;
                    if (!forceAcquire) {
                        miss = g_psStickyMiss.fetch_add(1, std::memory_order_acq_rel) + 1;
                    }
                    if ((forceAcquire || miss >= kPSStickyReleaseWindows) && best != nullptr) {
                        void* prev = g_playerBhkBody.exchange(best, std::memory_order_acq_rel);
                        g_psStickyMiss.store(0, std::memory_order_relaxed);
                        if (prev != best) {
                            PLOG_INFO(
                                "[PULSAR] M.1 V2 player-ship body acquired (sticky): "
                                "param2=0x{:X} d2={:.0f} prev=0x{:X}"sv,
                                reinterpret_cast<std::uintptr_t>(best), bestD2,
                                reinterpret_cast<std::uintptr_t>(prev));
                        }
                    }
                }
            }
            __try {
                auto* p = RE::PlayerCharacter::GetSingleton();
                auto* ship = p ? p->GetSpaceship(true) : nullptr;
                if (ship) {
                    // (2026-06-21) - player-ship-body match reference.
                    // During active flight, match the local havok body against the
                    // HOST's RECEIVED body position (havokState row3 = idx 12/13/14),
                    // NOT ship->data.location. data.location is the ship's LOGICAL
                    // space position (drives the questmarker) and lives in a DIFFERENT
                    // frame than the local havok bodies; matching against it failed
                    // (observed psPos=(849,3191) vs body~(0,0), >3500u apart →
                    // playerBhkBody never identified → no flight apply on the passive
                    // client). The host body and our body share the local physics
                    // frame, so they coincide → reliable match. When NOT receiving
                    // flight transforms (parked), fall back to data.location (≈ body
                    // at rest). g_psPosValid stays true whenever we have a ship, so
                    // NPC body tracking (gated on it) is unaffected.
                    if (g_pendingShipTransform.armed.load(std::memory_order_acquire)) {
                        const auto& d = g_pendingShipTransform.data;
                        g_psPosX.store(d.havokState[12], std::memory_order_relaxed);
                        g_psPosY.store(d.havokState[13], std::memory_order_relaxed);
                        g_psPosZ.store(d.havokState[14], std::memory_order_relaxed);
                    } else {
                        const auto& sl = ship->data.location;
                        g_psPosX.store(sl.x, std::memory_order_relaxed);
                        g_psPosY.store(sl.y, std::memory_order_relaxed);
                        g_psPosZ.store(sl.z, std::memory_order_relaxed);
                    }
                    g_psPosValid.store(true, std::memory_order_release);
                } else {
                    g_psPosValid.store(false, std::memory_order_release);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_psPosValid.store(false, std::memory_order_release);
            }
        }

        // M.1 V2 - position-match: is THIS body the player's ship?
        // Also: track local body translations (for both player + NPC) so
        // receiver-side matching has fresh local data.
        float bodyPos[3] = { 0, 0, 0 };
        bool  havePos    = Iter13SafeReadBodyPos(a_param2, bodyPos);
        bool  isPlayerCandidate = false;
        if (havePos && g_psPosValid.load(std::memory_order_acquire)) {
            const float dx = bodyPos[0] - g_psPosX.load(std::memory_order_relaxed);
            const float dy = bodyPos[1] - g_psPosY.load(std::memory_order_relaxed);
            const float dz = bodyPos[2] - g_psPosZ.load(std::memory_order_relaxed);
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < kPSMatchThreshSq) {
                isPlayerCandidate = true;
                // v2 - if the currently-LOCKED body is the one matching now,
                // flag it so the commit block keeps the lock (sticky). This is the
                // key disambiguator when several bodies report d2≈0: distance can't
                // tell them apart, so identity (same pointer as last time) does.
                if (a_param2 == g_playerBhkBody.load(std::memory_order_acquire)) {
                    g_psStickyHeld.store(true, std::memory_order_relaxed);
                }
                // Track the closest body this window as the (re)acquire candidate
                // (used only when we have no valid lock to keep).
                if (d2 < g_psMatchBestD2.load(std::memory_order_relaxed)) {
                    g_psMatchBestD2.store(d2, std::memory_order_relaxed);
                    g_psMatchBestBody.store(a_param2, std::memory_order_release);
                }
            }
        }

        // M.1 V2 iter2 - track local NPC body (for receiver-side matching)
        // and capture host-side NPC body state (for broadcast). Both via
        // helper functions to keep std::lock_guard out of this __try-using
        // function (C2712).
        // Only track when player-ship-pos cache is valid AND this body is
        // not the (stable) identified player body. L.3 V4 pilot handover:
        // peer's broadcast can move our player body away from REFR.location,
        // making isPlayerCandidate momentarily false - but the body is still
        // logically the player ship. Using stable g_playerBhkBody equality
        // keeps identity correct across position drift.
        void* psBodyStable = g_playerBhkBody.load(std::memory_order_acquire);
        if (havePos && a_param2 != psBodyStable
            && g_psPosValid.load(std::memory_order_acquire)) {
            UpdateLocalNpcBody(a_param2, bodyPos, now);
            if (net.IsHost()) {
                float st[16];
                if (SafeReadHavokState(a_param2, st)) {
                    UpdateHostNpcSlot(a_param2, st, now);
                }
            }
        }

        // Iter13 - owns g_isActivePilot. MaybeSyncShipTransform reads it.
        Iter13DetectAndSetActive(a_param2, now);
        const bool isActive = g_isActivePilot.load(std::memory_order_acquire);

        // V3 iter9 - one-time param_1 dump on active side only (so we know
        // we're looking at a real ship being piloted = highly likely player's).
        if (isActive && !g_param1Dumped.load(std::memory_order_acquire) && a_param1) {
            __try {
                auto* p1 = reinterpret_cast<std::uint8_t*>(a_param1);
                std::uint64_t qw[32]{};
                for (int i = 0; i < 32; ++i) {
                    qw[i] = *reinterpret_cast<std::uint64_t*>(p1 + i * 8);
                }
                PLOG_INFO(
                    "[PULSAR] V3 iter9 bhkState dump param1=0x{:X} qw0-7=[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
                    reinterpret_cast<std::uintptr_t>(a_param1),
                    qw[0], qw[1], qw[2], qw[3], qw[4], qw[5], qw[6], qw[7]);
                PLOG_INFO(
                    "[PULSAR] V3 iter9 bhkState qw8-15=[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
                    qw[8], qw[9], qw[10], qw[11], qw[12], qw[13], qw[14], qw[15]);
                PLOG_INFO(
                    "[PULSAR] V3 iter9 bhkState qw16-23=[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
                    qw[16], qw[17], qw[18], qw[19], qw[20], qw[21], qw[22], qw[23]);
                PLOG_INFO(
                    "[PULSAR] V3 iter9 bhkState qw24-31=[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
                    qw[24], qw[25], qw[26], qw[27], qw[28], qw[29], qw[30], qw[31]);
                g_param1Dumped.store(true, std::memory_order_release);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        if (isActive) {
            // M.1 V2 - only broadcast the *player's* ship body, not whichever
            // body Iter13 happens to flag as active. In encounters multiple
            // bodies move fast and Iter13 picks the highest EMA - that may
            // be a pirate. Gate broadcast to the position-matched player body.
            void* psBodyA = g_playerBhkBody.load(std::memory_order_acquire);
            if (psBodyA == nullptr || psBodyA != a_param2) return;

            float st[16]{};
            if (!SafeReadHavokState(a_param2, st)) return;
            // Store into the dedicated SEND buffer - separate from receive
            // buffer (g_pendingShipTransform.data) to avoid mixing concerns.
            PublishOutgoingHavokState(st);
            std::uint64_t cnt = g_v3iter7SendCount.fetch_add(1) + 1;
            std::uint64_t lastLog = g_lastIter7LogMs.load(std::memory_order_acquire);
            if ((now - lastLog) >= 1000) {
                g_lastIter7LogMs.store(now, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] V3 iter8 active read: cnt={} param2=0x{:X} "
                    "row0=({:.3f},{:.3f},{:.3f},{:.3f}) row3=({:.3f},{:.3f},{:.3f},{:.3f})"sv,
                    cnt, reinterpret_cast<std::uintptr_t>(a_param2),
                    st[0], st[1], st[2], st[3], st[12], st[13], st[14], st[15]);
            }
            return;
        }

        // M.1 V2 iter2d - non-player bodies: write cached NPC pending state
        // every physics step (60Hz). Engine physics writes here too, so we
        // need to override every step to keep client's NPC ships tracking
        // host's positions.
        void* psBody = g_playerBhkBody.load(std::memory_order_acquire);
        if (psBody != nullptr && a_param2 != psBody) {
            if (!net.IsHost()) {
                float npcSt[16];
                if (TakeNpcPendingApply(a_param2, now, npcSt)) {
                    SafeWriteHavokState(a_param2, npcSt);
                    // Suppress Iter13 motion detection - the write would
                    // otherwise count as motion and false-flip "active".
                    g_iter13LastApplyMs.store(now, std::memory_order_release);
                }
            }
            return;  // not player body; player passive apply is below
        }

        // Passive - write received havok state to body.
        // M.1 V2 - gate to player's bhkBody only. Without this, the passive
        // apply path teleports every NPC ship body to the host's player-ship
        // position (cluster bug). If position-match has not yet identified
        // the player's body, skip the apply - better to wait for a confident
        // match than to corrupt all ship transforms.
        if (psBody == nullptr) return;

        const bool armed = g_pendingShipTransform.armed.load(std::memory_order_acquire);
        if (!armed) return;
        const std::uint64_t recvAt = g_pendingShipTransform.receivedAtMs.load(std::memory_order_acquire);
        if (recvAt == 0 || (now - recvAt) > kV3StaleMs) return;

        const auto& d = g_pendingShipTransform.data;
        // Skip if state is all zeros (sender hadn't filled yet).
        bool allZero = true;
        for (int i = 0; i < 16 && allZero; ++i) if (d.havokState[i] != 0.0f) allZero = false;
        if (allZero) return;

        // L.5 Phase 3v8 - narrowed cruise filter. Earlier 1e7 clamp filtered
        // too many normal cruise frames → drift over time (user 2026-05-01
        // 15:51 test). Now only block clearly-pathological values: INF/NaN
        // or magnitude > 1e8 (extreme spikes). Normal cruise positions up to
        // 1e7 pass through, drift eliminated.
        if (IsCruiseActive()) {
            // Phase 4 Step 7 - relaxed from 1e8 magnitude clamp to NaN/INF only.
            // Cruise produces legitimate 10^11-scale ship positions (verified in
            // EXIT pos=(-140665943703,...) logs). The clamp blocked ALL of those,
            // freezing client's ship at cruise-start position → client never near
            // arrival POI → cell-stream loads no content. NaN/INF still filtered
            // (those WOULD crash the engine on apply).
            const float x = d.havokState[12];
            const float y = d.havokState[13];
            const float z = d.havokState[14];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
        }

        SafeWriteHavokState(a_param2, d.havokState);
        // Iter13 - mark apply timestamp so motion detection enters cooldown
        // and doesn't false-positive on host's deltas echoing through.
        g_iter13LastApplyMs.store(now, std::memory_order_release);
        // Cache body pointer so Tick can keep applying when vt[8] stops
        // firing (e.g. when client unsits → engine halts ship physics-step).
        g_iter13CachedApplyBody.store(a_param2, std::memory_order_release);
        g_iter13CachedApplyBodyMs.store(now, std::memory_order_release);
        // record this known-good ship-body's vtable as the type-guard for the
        // Tick body-commit (SafeCommitHavokTransform). Logged once so the in-game test
        // reveals whether the ship body is the proxy controller (RVA 0x4D65D60 → commit
        // path correct) or the rigidbody controller (RVA 0x4D67400 → needs its own setter).
        {
            const std::uintptr_t vt = *reinterpret_cast<std::uintptr_t*>(a_param2);
            if (vt && g_shipBodyVtbl.load(std::memory_order_relaxed) != vt) {
                g_shipBodyVtbl.store(vt, std::memory_order_release);
                PLOG_INFO("[PULSAR] ship-body vtbl recorded: 0x{:X} (RVA 0x{:X})"sv,
                          vt, vt - 0x140000000ull);
            }
        }

        std::uint64_t cnt = g_v3iter7ApplyCount.fetch_add(1) + 1;
        std::uint64_t lastLog = g_lastIter7LogMs.load(std::memory_order_acquire);
        if ((now - lastLog) >= 1000) {
            g_lastIter7LogMs.store(now, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] V3 iter8 passive apply: cnt={} param2=0x{:X} ageMs={} "
                "row0=({:.3f},{:.3f},{:.3f},{:.3f}) row3=({:.3f},{:.3f},{:.3f},{:.3f})"sv,
                cnt, reinterpret_cast<std::uintptr_t>(a_param2), (now - recvAt),
                d.havokState[0], d.havokState[1], d.havokState[2], d.havokState[3],
                d.havokState[12], d.havokState[13], d.havokState[14], d.havokState[15]);
        }
    }

    // L.5 Phase 2 - poll cruise-active byte from Tick. SEH-guarded read.
    // L.5 Phase 3 debug - read all cruise-state globals + actor/ship cell
    // formIDs and emit one structured log line. Called at every state
    // transition + 1Hz during cruise. SEH-guarded; missing data shown as 0/null.
    static void LogCruiseSnapshot(const char* a_label)
    {
        // Resolve module-relative VAs.
        const auto rd_u8  = [](std::uintptr_t va, std::uint8_t&  out) {
            __try { out = *reinterpret_cast<std::uint8_t* >(va); }
            __except (EXCEPTION_EXECUTE_HANDLER) { out = 0xFF; }
        };
        const auto rd_u32 = [](std::uintptr_t va, std::uint32_t& out) {
            __try { out = *reinterpret_cast<std::uint32_t*>(va); }
            __except (EXCEPTION_EXECUTE_HANDLER) { out = 0xDEADBEEF; }
        };
        const auto rd_u64 = [](std::uintptr_t va, std::uint64_t& out) {
            __try { out = *reinterpret_cast<std::uint64_t*>(va); }
            __except (EXCEPTION_EXECUTE_HANDLER) { out = 0xDEADBEEFDEADBEEFull; }
        };
        const auto rd_f32 = [](std::uintptr_t va, float& out) {
            __try { out = *reinterpret_cast<float*>(va); }
            __except (EXCEPTION_EXECUTE_HANDLER) { out = -999.f; }
        };

        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

        std::uint8_t  byteMain = 0, byte2nd = 0, exitAnim = 0;
        std::uint32_t cruiseDist = 0;
        std::uint64_t orient0 = 0, orient1 = 0, orient2 = 0, orient3 = 0;
        std::uint64_t ctxPtr = 0, ctx2Ptr = 0;
        float duration2 = 0.f, subVal = 0.f;

        rd_u8 (base + kRVA_CruiseActiveByte,  byteMain);
        rd_u8 (base + kRVA_Cruise2ndByte,     byte2nd);
        rd_u8 (base + kRVA_CruiseExitAnimVA,  exitAnim);
        rd_u32(base + kRVA_CruiseDistance,    cruiseDist);
        rd_u64(base + kRVA_CruiseOrient0,     orient0);
        rd_u64(base + kRVA_CruiseOrient1,     orient1);
        rd_u64(base + kRVA_CruiseOrient2,     orient2);
        rd_u64(base + kRVA_CruiseOrient3,     orient3);
        rd_u64(base + kRVA_CruiseContextPtr,  ctxPtr);
        rd_u64(base + kRVA_CruiseContext2,    ctx2Ptr);
        rd_f32(base + kRVA_CruiseDuration2,   duration2);
        rd_f32(base + kRVA_CruiseTimeSubVal,  subVal);

        // Phase 4 diag - assoc form pointers + their formIDs (TESForm.formID @ +0x28).
        std::uint64_t af0 = 0, af1 = 0, af2 = 0, af3 = 0;
        std::uint32_t af0Id = 0, af1Id = 0, af2Id = 0, af3Id = 0;
        rd_u64(base + kRVA_CruiseAssocForm0, af0);
        rd_u64(base + kRVA_CruiseAssocForm1, af1);
        rd_u64(base + kRVA_CruiseAssocForm2, af2);
        rd_u64(base + kRVA_CruiseAssocForm3, af3);
        if (af0) rd_u32(af0 + 0x28, af0Id);
        if (af1) rd_u32(af1 + 0x28, af1Id);
        if (af2) rd_u32(af2 + 0x28, af2Id);
        if (af3) rd_u32(af3 + 0x28, af3Id);

        // Phase 4 diag - also read DAT_145EB8274 (arrival state) + DAT_145EB2857 (UI selector).
        std::uint32_t arrivalState = 0;
        std::uint8_t  uiSelector = 0;
        rd_u32(base + kRVA_CruiseArrivalState, arrivalState);
        rd_u8 (base + kRVA_CruiseUiSelector,   uiSelector);

        std::uint32_t playerCellID = 0, shipCellID = 0, shipFormID = 0;
        std::uint32_t playerWS = 0, shipWS = 0;
        bool inSpace = false;
        // Multi-source position capture: data.location (+0x8C/0x90/0x94),
        // mesh.world.translate (+0x70 → +0x60 NiTransform.translate).
        float dataX = 0, dataY = 0, dataZ = 0;
        float meshX = 0, meshY = 0, meshZ = 0;
        std::uint64_t havokBodyAddr = 0;
        float row3X = 0, row3Y = 0, row3Z = 0;
        // Player position - different from ship.data which is always 0
        // (ship is anchor). Player.data may encode position relative to
        // the cell origin / current POI, which could be the signal that
        // identifies "near POI X".
        float playerDataX = 0, playerDataY = 0, playerDataZ = 0;
        __try {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (p) {
                __try { inSpace = p->IsInSpace(true); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                __try {
                    auto& ploc = p->data.location;
                    playerDataX = ploc.x; playerDataY = ploc.y; playerDataZ = ploc.z;
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                auto* pCell = p->parentCell;
                if (pCell) {
                    playerCellID = pCell->formID;
                    auto* ws = pCell->cellWorldspace;  // CommonLibSF +0x118
                    if (ws) playerWS = ws->formID;
                }
                auto* sh = p->GetSpaceship(true);
                if (sh) {
                    shipFormID = sh->formID;
                    auto* sCell = sh->parentCell;
                    if (sCell) {
                        shipCellID = sCell->formID;
                        auto* ws = sCell->cellWorldspace;
                        if (ws) shipWS = ws->formID;
                    }
                    // data.location via CommonLibSF struct field (avoid raw offsets).
                    __try {
                        auto& loc = sh->data.location;
                        dataX = loc.x; dataY = loc.y; dataZ = loc.z;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    // 3D world translate via loadedData->data3D (matches
                    // BoneSync pattern). loadedData is BSGuarded so access
                    // via raw +0xB8 pointer extraction. NiAVObject world.translate
                    // is at offset +0x60 from the 3D node base.
                    __try {
                        auto* loadedGuard = reinterpret_cast<std::uint8_t*>(sh) + 0xB8;
                        auto* loaded = *reinterpret_cast<RE::LOADED_REF_DATA**>(loadedGuard);
                        if (loaded && loaded->data3D) {
                            auto* nodeBytes = reinterpret_cast<std::uint8_t*>(
                                loaded->data3D.get());
                            meshX = *reinterpret_cast<float*>(nodeBytes + 0x60);
                            meshY = *reinterpret_cast<float*>(nodeBytes + 0x64);
                            meshZ = *reinterpret_cast<float*>(nodeBytes + 0x68);
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
                // Havok body via the cached pointer L.3 V3 actually uses.
                // This is the same body that drives ship sync, so reading
                // its row3 gives us the ground-truth position L.3 V3 sees.
                __try {
                    void* body = g_iter13CachedApplyBody.load(std::memory_order_acquire);
                    if (!body) body = g_playerBhkBody.load(std::memory_order_acquire);
                    if (body) {
                        havokBodyAddr = reinterpret_cast<std::uint64_t>(body);
                        auto* bodyBytes = reinterpret_cast<std::uint8_t*>(body);
                        row3X = *reinterpret_cast<float*>(
                            bodyBytes + kHavokBodyDataPtrOffset + 0x50);
                        row3Y = *reinterpret_cast<float*>(
                            bodyBytes + kHavokBodyDataPtrOffset + 0x54);
                        row3Z = *reinterpret_cast<float*>(
                            bodyBytes + kHavokBodyDataPtrOffset + 0x58);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        const auto& net = Pulsar::Network::GetSingleton();
        const char* role = net.IsConnected()
            ? (net.IsHost() ? "HOST" : "CLI ")
            : "SOLO";

        PLOG_INFO(
            "[PULSAR] L.5 D3 [{}] {} byte={}/{} exitAnim={} dist={} dur={:.2f} sub={:.4f}"sv,
            role, a_label, byteMain, byte2nd, exitAnim,
            cruiseDist, duration2, subVal);
        PLOG_INFO(
            "[PULSAR] L.5 D3 [{}]   orient=[{:016X} {:016X} {:016X} {:016X}]"sv,
            role, orient0, orient1, orient2, orient3);
        PLOG_INFO(
            "[PULSAR] L.5 D3 [{}]   ctx={:016X} ctx2={:016X} ship=0x{:08X}"sv,
            role, ctxPtr, ctx2Ptr, shipFormID);
        PLOG_INFO(
            "[PULSAR] L.5 D3 [{}]   playerCell=0x{:08X} playerWS=0x{:08X} shipCell=0x{:08X} shipWS=0x{:08X} inSpace={}"sv,
            role, playerCellID, playerWS, shipCellID, shipWS, inSpace ? 1 : 0);
        PLOG_INFO(
            "[PULSAR] L.5 D3 [{}]   ship.data=({:.2f},{:.2f},{:.2f}) ship.mesh=({:.2f},{:.2f},{:.2f}) body=0x{:X} row3=({:.2f},{:.2f},{:.2f})"sv,
            role, dataX, dataY, dataZ, meshX, meshY, meshZ,
            havokBodyAddr, row3X, row3Y, row3Z);
        PLOG_INFO(
            "[PULSAR] L.5 D3 [{}]   player.data=({:.2f},{:.2f},{:.2f})"sv,
            role, playerDataX, playerDataY, playerDataZ);
        PLOG_INFO(
            "[PULSAR] L.5 D3 [{}]   assocForms 8C0=ptr0x{:X}/id0x{:08X} 8C8=ptr0x{:X}/id0x{:08X} 810=ptr0x{:X}/id0x{:08X} 868=ptr0x{:X}/id0x{:08X}"sv,
            role, af0, af0Id, af1, af1Id, af2, af2Id, af3, af3Id);
        PLOG_INFO(
            "[PULSAR] L.5 D3 [{}]   arrivalState=0x{:08X} uiSelector={}"sv,
            role, arrivalState, static_cast<int>(uiSelector));
        NASLOG("L.5 D3 [%s] %s byte=%u/%u exitAnim=%u dist=%u dur=%.2f sub=%.4f orient=[%016llX %016llX %016llX %016llX] ctx=%016llX/%016llX ship=0x%08X playerCell=0x%08X(WS=0x%08X) shipCell=0x%08X(WS=0x%08X) inSpace=%d data=(%.2f,%.2f,%.2f) mesh=(%.2f,%.2f,%.2f) row3=(%.2f,%.2f,%.2f)",
            role, a_label, byteMain, byte2nd, exitAnim,
            cruiseDist, duration2, subVal,
            orient0, orient1, orient2, orient3,
            ctxPtr, ctx2Ptr, shipFormID,
            playerCellID, playerWS, shipCellID, shipWS, inSpace ? 1 : 0,
            dataX, dataY, dataZ, meshX, meshY, meshZ, row3X, row3Y, row3Z);
    }

    // L.5 Phase 4 Step 10 - SEH-safe hex dump of one memory region. Emits
    // ONE log line per region via REX::INFO (spdlog file). Switched from
    // NASLog after the NAS \\<NAS> share couldn't be opened from Starfield
    // process context (file stayed empty). The python diff tool matches on
    // `region=<name>` prefix.
    static void LogCruiseHexRegion(const char* a_role,
                                   const char* a_label,
                                   const char* a_regionName,
                                   std::uintptr_t a_va,
                                   std::size_t a_bytes)
    {
        if (a_bytes == 0 || a_bytes > 1024) return;
        // Stack buffer big enough for 1024 bytes of hex (3 chars each + slop).
        char hex[1024 * 3 + 64];
        std::size_t hpos = 0;
        std::uint8_t snapshot[1024];

        // Read the region into a local buffer first (one SEH frame).
        std::size_t got = 0;
        __try {
            auto* src = reinterpret_cast<const std::uint8_t*>(a_va);
            for (std::size_t i = 0; i < a_bytes; ++i) {
                snapshot[i] = src[i];
            }
            got = a_bytes;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            got = 0;
        }

        if (got == 0) {
            PLOG_INFO(
                "[PULSAR] L.5 D10 [{}] {} region={} @0x{:X} FAULT"sv,
                a_role, a_label, a_regionName,
                static_cast<unsigned long long>(a_va));
            return;
        }

        // Format hex bytes into the line buffer.
        for (std::size_t i = 0; i < got; ++i) {
            int n = std::snprintf(hex + hpos, sizeof(hex) - hpos,
                                  "%02x ", snapshot[i]);
            if (n <= 0 || hpos + static_cast<std::size_t>(n) >= sizeof(hex)) break;
            hpos += static_cast<std::size_t>(n);
        }
        if (hpos > 0 && hex[hpos - 1] == ' ') hex[hpos - 1] = 0;

        PLOG_INFO(
            "[PULSAR] L.5 D10 [{}] {} region={} @0x{:X} len={} data={}"sv,
            a_role, a_label, a_regionName,
            static_cast<unsigned long long>(a_va),
            static_cast<unsigned long long>(got),
            std::string_view(hex, hpos));
    }

    // L.5 Phase 4 Step 10 - chase a pointer in memory and dump pointed-to
    // bytes. Used for sink-context smart-ptrs etc. where the actual state
    // lives behind the static slot.
    static void LogCruiseHexChase(const char* a_role,
                                  const char* a_label,
                                  const char* a_regionName,
                                  std::uintptr_t a_ptrVa,
                                  std::size_t a_bytes)
    {
        std::uint64_t target = 0;
        __try { target = *reinterpret_cast<std::uint64_t*>(a_ptrVa); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (target < 0x10000 || target > 0x7FFFFFFFFFFFull) return;
        LogCruiseHexRegion(a_role, a_label, a_regionName,
                           static_cast<std::uintptr_t>(target), a_bytes);
    }

    // L.5 Phase 4 Step 10 - broad memory dump for runtime diff. Called at
    // 10Hz during cruise on both host and client. Output is parsed by
    // tools/diff_cruise_dumps.py. Each region is one log line with a
    // region-name tag so the diff matches by tag at the same wall-clock.
    static void LogCruiseMemDump(const char* a_label)
    {
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) return;

        const auto& net = Pulsar::Network::GetSingleton();
        const char* role = net.IsConnected()
            ? (net.IsHost() ? "HOST" : "CLI ")
            : "SOLO";

        // Header line - wall-clock + tick + label, lets the diff tool slice
        // the log into per-tick blocks.
        PLOG_INFO("[PULSAR] L.5 D10 [{}] {} BLOCK tick={}"sv,
                  role, a_label,
                  static_cast<unsigned long long>(::GetTickCount64()));

        // -- Cruise statics, primary block (byteMain, byte2nd, arrival,
        // uiSelector, gates) - RVA window 0x5EAA4C0..0x5EB2900.
        LogCruiseHexRegion(role, a_label, "cruise_5EB22",
                           base + 0x5EAA4C0, 256);
        LogCruiseHexRegion(role, a_label, "cruise_5EB28",
                           base + 0x5EAAA40, 256);

        // -- Cruise statics, secondary (state machine, distance, dur counter,
        // assoc-form area) - 0x5EB8200..0x5EB8500.
        LogCruiseHexRegion(role, a_label, "cruise_5EB82",
                           base + 0x5EB8200, 512);
        LogCruiseHexRegion(role, a_label, "cruise_5EAA8",
                           base + 0x5EAA800, 256);

        // -- Cruise orient/UI block (orient0..3, dur MAX, UI strings) -
        // 0x5F31C00..0x5F2A058.
        LogCruiseHexRegion(role, a_label, "cruise_5F31C",
                           base + 0x5F31C00, 512);

        // -- Cruise sink-context area (smart-ptrs + per-tick decrement) -
        // 0x5FBC800..0x5FB4C80 + 0x5FC7700.
        LogCruiseHexRegion(role, a_label, "cruise_5FBC8",
                           base + 0x5FBC800, 512);
        LogCruiseHexRegion(role, a_label, "cruise_5FC77",
                           base + 0x5FC7700, 256);

        // -- Per-tick math write targets (DAT_14591EFE4 cluster, DAT_145E75490,
        // DAT_145FBD140) - small windows around each.
        LogCruiseHexRegion(role, a_label, "tick_591EF",
                           base + 0x5916FA8, 128);
        LogCruiseHexRegion(role, a_label, "tick_5E754",
                           base + 0x5E6D6C4, 128);
        LogCruiseHexRegion(role, a_label, "tick_5FBD1",
                           base + 0x5FB5380, 128);

        // -- Star-system / arrival-init dur gate at 0x590627F.
        LogCruiseHexRegion(role, a_label, "cruise_590E2",
                           base + 0x5906260, 64);

        // -- Player world-state singleton (DAT_145F4AFD0 area) - used by
        // arrival-init's cell-stream task as `worldState ptr`.
        LogCruiseHexRegion(role, a_label, "ws_5F4AF",
                           base + 0x5F43230, 256);

        // -- Exit-anim area (DAT_1461E51F0+0x2D8 = 0x61DD738) and surrounding
        // engine flags.
        LogCruiseHexRegion(role, a_label, "exit_61E54",
                           base + 0x61E5400, 256);

        // -- Marker / atmosphere singleton vicinity. The
        // LocationMarkerArrayUpdate event source is at 0x14597A220; nearby
        // pointers may hold the marker manager + celestial-body lists.
        LogCruiseHexRegion(role, a_label, "marker_59822",
                           base + 0x597A200, 256);

        // -- World manager singleton DAT_1461EA098 (POI list anchor).
        LogCruiseHexRegion(role, a_label, "wmgr_61EA0",
                           base + 0x61EA000, 256);
        LogCruiseHexRegion(role, a_label, "wmgr_61EA1",
                           base + 0x61E22E0, 256);

        // -- Pointer-chase: dump bytes BEHIND the cruise sink-context pointers
        // because the lambda + cell-stream payload state lives there. Static
        // slots only show the pointer; the actual evolving state is at *(ptr).
        LogCruiseHexChase(role, a_label, "ctx_8F0_chase",
                          base + 0x5FB4B70, 256);
        LogCruiseHexChase(role, a_label, "ctx_8E8_chase",
                          base + 0x5FB4B68, 256);

        // -- Pointer-chase the player worldstate ptr (it's a singleton ptr
        // slot inside DAT_145F4AFD0 area; the actual world state lives behind
        // it with cell list, current parent body, etc.). Using +0xC8 as our
        // best guess from cell-stream-task arg shape.
        LogCruiseHexChase(role, a_label, "ws_chase",
                          base + 0x5F43230 + 0xC8, 512);

        // -- Pointer-chase the world manager singleton (DAT_1461EA098).
        // Sub-objects at +0x58 / +0x238 / +0x8B0 are POI list anchors.
        LogCruiseHexChase(role, a_label, "wmgr_chase",
                          base + 0x61E2288, 256);
    }

    static void PollCruiseActiveByte()
    {
        std::uintptr_t ptr = g_cruiseActiveBytePtr.load(std::memory_order_acquire);
        if (!ptr) {
            ptr = REL::Offset{ kRVA_CruiseActiveByte }.address();
            g_cruiseActiveBytePtr.store(ptr, std::memory_order_release);
        }

        std::uint8_t cur = 0;
        __try {
            cur = *reinterpret_cast<std::uint8_t*>(ptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }

        const std::uint8_t prev = g_lastCruiseActive.load(std::memory_order_acquire);
        if (prev == 0xFF) {
            // First poll - initialize without firing transition.
            g_lastCruiseActive.store(cur, std::memory_order_release);
            return;
        }
        if (cur == prev) return;

        // Transition detected.
        g_lastCruiseActive.store(cur, std::memory_order_release);

        // L.5 D3 snapshot at every byte transition. Compares host vs client
        // state at the SAME logical event (host's poll catches its own
        // toggle, client's poll catches the toggle we caused via Apply).
        char xLabel[32];
        std::snprintf(xLabel, sizeof(xLabel), "TRANSITION %u->%u", prev, cur);
        LogCruiseSnapshot(xLabel);

        auto& net = Pulsar::Network::GetSingleton();

        std::uint32_t shipFormId = 0;
        __try {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (p) {
                auto* sh = p->GetSpaceship(true);
                if (sh) shipFormId = sh->formID;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        const std::uint64_t now = ::GetTickCount64();
        const std::uint64_t suppress = g_cruiseApplySuppressUntilMs.load(std::memory_order_acquire);
        const bool inSuppressWindow = (now < suppress);

        // Phase 4 - flag whether this exit was "auto-arrival" (post-arrivalInit)
        // or "manual" (player pressed cruise key mid-flight). Manual exit
        // doesn't trigger FUN_140B6EB20 (cell-stream task), so client may not
        // see same cell content as host post-exit.
        const bool arrivalSeen = g_arrivalSeenThisCruise.load(std::memory_order_acquire);
        const char* exitType = "(n/a)";
        if (prev == 1 && cur == 0) {
            exitType = arrivalSeen ? "AUTO-ARRIVAL" : "MANUAL";
        } else if (prev == 0 && cur == 1) {
            // Reset for new cruise session.
            g_arrivalSeenThisCruise.store(false, std::memory_order_release);
            g_step13FiredThisCruise.store(false, std::memory_order_release);
            g_lastMarkerNotifyMs.store(0, std::memory_order_release);
            // Step 16 - mark self-initiated if not in suppress window (we
            // caused this cruise locally vs received and applied a remote one).
            g_cruiseSelfInitiated.store(!inSuppressWindow, std::memory_order_release);
            exitType = "(start)";
        }
        if (prev == 1 && cur == 0) {
            // Cruise exit - clear self-initiated flag for next session.
            g_cruiseSelfInitiated.store(false, std::memory_order_release);
        }

        PLOG_INFO(
            "[PULSAR] L.5 CruisePoll TRANSITION: {} -> {} ship=0x{:08X} "
            "isHost={} isConn={} suppressed={} exitType={}"sv,
            prev, cur, shipFormId,
            net.IsConnected() && net.IsHost() ? 1 : 0,
            net.IsConnected() ? 1 : 0,
            inSuppressWindow ? 1 : 0,
            exitType);

        // Broadcast: any connected peer can initiate cruise (Step 16 bidirectional).
        // Suppression prevents re-broadcasting a transition we just applied
        // from a remote packet - the originator broadcasts, the receiver
        // applies + suppresses.
        if (net.IsConnected() && !inSuppressWindow) {
            Pulsar::Network::CruiseStateData payload{};
            payload.enable      = cur ? 1 : 0;
            payload.shipFormId  = shipFormId;
            payload.timestampMs = now;
            // L.5 Phase 3v3 - at EXIT broadcast, capture host's world position
            // (3 doubles at DAT_145F31DA0/DA8/DB0). Client overrides its own
            // before EXIT call so engine derives final position from same state.
            // At ENTER we still send 0s (client force-ENTER computes its own).
            if (cur == 0) {
                const std::uintptr_t base =
                    reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                __try {
                    payload.shipPosX = *reinterpret_cast<double*>(
                        base + kRVA_CruiseOrient0);
                    payload.shipPosY = *reinterpret_cast<double*>(
                        base + kRVA_CruiseOrient1);
                    payload.shipPosZ = *reinterpret_cast<double*>(
                        base + kRVA_CruiseOrient2);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    payload.shipPosX = payload.shipPosY = payload.shipPosZ = 0.0;
                }
            }
            // Phase 4 Step 4 - at ENTER broadcast, capture host's resolved
            // destination formID (DAT_145EB8284 low 32b). Resolver FUN_1413FDFD0
            // already ran inside the engine ENTER fn before our poll fires.
            // Client writes this before its force-ENTER so per-tick arrival
            // check (FUN_1413FE900) has a target and the math advances.
            if (cur == 1) {
                const std::uintptr_t base =
                    reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                __try {
                    payload.destFormId = *reinterpret_cast<std::uint32_t*>(
                        base + kRVA_CruiseDistance);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    payload.destFormId = 0;
                }
            }
            std::uint8_t buf[96]{};
            Pulsar::Network::PacketWriter w(
                Pulsar::Network::PKT_CRUISE_STATE, buf, sizeof(buf));
            if (w.Write(payload)) {
                net.Send(w.Data(), w.Size(), true);  // reliable - single rare event
                PLOG_INFO(
                    "[PULSAR] L.5 CruiseState SEND: enable={} ship=0x{:08X} dest=0x{:08X} pos=({:.0f},{:.0f},{:.0f})"sv,
                    static_cast<int>(payload.enable), payload.shipFormId,
                    payload.destFormId,
                    payload.shipPosX, payload.shipPosY, payload.shipPosZ);
            }

            // L.5 Phase 3 v2 - capture host's shipCell at cruise start so we
            // can compare at EXIT and gate the resync. Skip resync entirely
            // when shipCell is unchanged (intra-system cruise, common case).
            if (cur == 1) {
                std::uint32_t startShipCell = 0;
                __try {
                    auto* p = RE::PlayerCharacter::GetSingleton();
                    if (p) {
                        auto* sh = p->GetSpaceship(true);
                        if (sh && sh->parentCell) startShipCell = sh->parentCell->formID;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                g_preCruiseShipCellID.store(startShipCell, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] L.5 Phase 3v2: cruise START - captured shipCell=0x{:08X}"sv,
                    startShipCell);
            }

            // L.5 Phase 3 - host: on EXIT broadcast, arm post-cruise resync.
            // Cruise can navigate static-space → static-space (different
            // system) which the existing HostTryTakeoffResync proc-boundary
            // logic does NOT detect. Use the EXIT signal as the trigger.
            if (cur == 0) {
                const std::uint64_t due = now + kPostCruiseResyncDelayMs;
                g_postCruiseResyncDueMs.store(due, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] L.5 Phase 3: post-cruise resync armed, due in {}ms"sv,
                    kPostCruiseResyncDelayMs);
            }
        }
    }

    // L.5 Phase 4 Step 6 - poll DAT_145EB2856 ("arrival initiated" gate). On
    // 0->1 transition, host's per-tick math just fired FUN_1413FA140 (= "Cruise
    // Mode Arrival Initiated"). We broadcast a PKT_CRUISE_STATE with arrivalInit
    // = 1 so client mirrors that engine state and dur countdown / cell-stream
    // task fire on its side too. 1->0 happens automatically inside FUN_1413FEBA0
    // when arrival completes (alongside cruise-byte 1->0 → already covered).
    // Same 60Hz tick + same suppression window as the cruise-byte poll.
    static void PollCruiseArrivalByte()
    {
        std::uintptr_t ptr = g_cruiseArrivalBytePtr.load(std::memory_order_acquire);
        if (!ptr) {
            ptr = REL::Offset{ kRVA_CruiseArrivalByte }.address();
            g_cruiseArrivalBytePtr.store(ptr, std::memory_order_release);
        }

        std::uint8_t cur = 0;
        __try {
            cur = *reinterpret_cast<std::uint8_t*>(ptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }

        const std::uint8_t prev = g_lastCruiseArrival.load(std::memory_order_acquire);
        if (prev == 0xFF) {
            g_lastCruiseArrival.store(cur, std::memory_order_release);
            return;
        }
        if (cur == prev) return;

        g_lastCruiseArrival.store(cur, std::memory_order_release);

        // Only the 0->1 transition needs sync. 1->0 is part of arrival-completed
        // sequence which also drives cruise-byte 1->0 (already synced).
        if (!(prev == 0 && cur == 1)) return;

        g_arrivalSeenThisCruise.store(true, std::memory_order_release);

        auto& net = Pulsar::Network::GetSingleton();
        // Step 16 - bidirectional: any connected originator broadcasts the
        // arrival init, since only the originator's per-tick math actually
        // fires it (DAT_145EB2856 0→1 is per-tick computation).
        if (!net.IsConnected()) return;
        if (!g_cruiseSelfInitiated.load(std::memory_order_acquire)) {
            // We received this cruise from a remote peer; their per-tick
            // math drives arrival init too. We won't see a meaningful
            // 0→1 here unless the engine fired locally - which only
            // happens for self-initiated cruises.
            return;
        }

        const std::uint64_t now = ::GetTickCount64();
        const std::uint64_t suppress = g_cruiseApplySuppressUntilMs.load(std::memory_order_acquire);
        if (now < suppress) {
            PLOG_INFO("[PULSAR] L.5 Phase 4 Step 6: arrival 0->1 in suppress window - skip"sv);
            return;
        }

        std::uint32_t shipFormId = 0;
        __try {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (p) {
                auto* sh = p->GetSpaceship(true);
                if (sh) shipFormId = sh->formID;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        Pulsar::Network::CruiseStateData payload{};
        payload.enable      = 1;       // cruise still active during arrival
        payload.arrivalInit = 1;
        payload.shipFormId  = shipFormId;
        payload.timestampMs = now;

        // Phase 4 Step 7 - capture host's UI state + arrival state machine value
        // so client's FUN_1413FA140 has same inputs.
        // Phase 4 Step 7b - also capture orient globals (DAT_145F31DA0/DA8/DB0).
        // The cell-stream task (FUN_140B6EB20) fires inside FUN_1413FEBA0's
        // dur countdown - BEFORE EXIT cleanup. Phase 3v3 writes orient at EXIT
        // (too late for cell stream). Writing at arrivalInit (= dur start)
        // ensures the streaming sees host's world position when picking what
        // content to load (planet, station mesh, NPC ships).
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        __try {
            payload.arrivalState = *reinterpret_cast<std::uint32_t*>(
                base + kRVA_CruiseArrivalState);
            payload.uiSelector = *reinterpret_cast<std::uint8_t*>(
                base + kRVA_CruiseUiSelector);
            payload.shipPosX = *reinterpret_cast<double*>(base + kRVA_CruiseOrient0);
            payload.shipPosY = *reinterpret_cast<double*>(base + kRVA_CruiseOrient1);
            payload.shipPosZ = *reinterpret_cast<double*>(base + kRVA_CruiseOrient2);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            payload.arrivalState = 0;
            payload.uiSelector = 0;
            payload.shipPosX = payload.shipPosY = payload.shipPosZ = 0.0;
        }

        std::uint8_t buf[96]{};
        Pulsar::Network::PacketWriter w(
            Pulsar::Network::PKT_CRUISE_STATE, buf, sizeof(buf));
        if (w.Write(payload)) {
            net.Send(w.Data(), w.Size(), true);
            PLOG_INFO(
                "[PULSAR] L.5 Phase 4 Step 6/7/7b SEND arrivalInit=1 ship=0x{:08X} state=0x{:08X} uiSel={} pos=({:.0f},{:.0f},{:.0f})"sv,
                shipFormId, payload.arrivalState, payload.uiSelector,
                payload.shipPosX, payload.shipPosY, payload.shipPosZ);
        }
    }

    // L.5 Phase 3 - fires the SaveBuffer-resync once the post-cruise delay
    // elapses on host. Re-uses the L.2 BeginHostQuicksaveTransfer pipeline
    // (its own 30s cooldown applies, so back-to-back cruise+takeoff in the
    // same window collapses to one transfer - acceptable).
    static void TickPostCruiseResync()
    {
        const std::uint64_t due = g_postCruiseResyncDueMs.load(std::memory_order_acquire);
        if (due == 0) return;
        const std::uint64_t now = ::GetTickCount64();
        if (now < due) return;

        // Disarm BEFORE the transfer call so a late re-arm during transfer
        // (unlikely, since cruise-EXIT is rare) can latch correctly.
        g_postCruiseResyncDueMs.store(0, std::memory_order_release);

        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected() || !net.IsHost()) {
            PLOG_INFO("[PULSAR] L.5 Phase 3: resync due but not host/connected - skip"sv);
            return;
        }

        // L.5 Phase 3v7 - gated resync (same-cell skip restored). Cross-cell
        // cruise (cross-system far-travel) → SaveBuffer fires (rare). Same-
        // cell cruise (intra-system, e.g. moving closer to a planet) → skip;
        // user's explicit guidance after 3v6 always-resync caused client
        // crash-to-menu on second same-cell cruise (2026-05-01 15:25 test).
        // L.3 V3 cruise-suspend (3v5) prevents in-cruise freeze.
        // Same-cell content drift (different POI proximity) accepted as
        // known limitation - engine's POI state is not in any single global
        // we've located, deeper RE deferred.
        std::uint32_t curShipCell = 0;
        __try {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (p) {
                auto* sh = p->GetSpaceship(true);
                if (sh && sh->parentCell) curShipCell = sh->parentCell->formID;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        const std::uint32_t startShipCell =
            g_preCruiseShipCellID.exchange(0, std::memory_order_acq_rel);

        if (curShipCell == startShipCell || curShipCell == 0) {
            PLOG_INFO(
                "[PULSAR] L.5 Phase 3v7: shipCell unchanged 0x{:08X} → 0x{:08X} - skip resync (intra-system cruise)"sv,
                startShipCell, curShipCell);
            NASLOG("L.5 Phase 3v7: same shipCell 0x%08X, skipping resync",
                   curShipCell);
            LogCruiseSnapshot("RESYNC SKIPPED (same cell)");
            return;
        }

        PLOG_INFO(
            "[PULSAR] L.5 Phase 3v7: shipCell CHANGED 0x{:08X} → 0x{:08X} - firing SaveBuffer-resync (cross-system cruise)"sv,
            startShipCell, curShipCell);
        NASLOG("L.5 Phase 3v7: shipCell 0x%08X -> 0x%08X, firing resync",
               startShipCell, curShipCell);

        LogCruiseSnapshot("PRE-RESYNC HOST");
        net.BeginHostQuicksaveTransfer();
    }

    // L.5 Phase 2 - apply a queued remote cruise transition. Runs on game
    // thread from Tick. Calls the cruise toggle function with the bool.
    static void ApplyPendingCruise()
    {
        // Phase 4 Step 6 - separate trigger for arrival-init. Fires
        // FUN_1413FA140 directly so client's per-frame dur countdown +
        // cell-stream task run mirroring host. Doesn't touch ENTER/EXIT
        // state (cruise byte stays 1, sinks already allocated).
        if (g_pendingCruise.armedArrivalInit.exchange(false, std::memory_order_acq_rel)) {
            std::uintptr_t fnArr = g_cruiseArrivalInitFn.load(std::memory_order_acquire);
            if (!fnArr) {
                fnArr = REL::Offset{ kRVA_CruiseArrivalInitFn }.address();
                g_cruiseArrivalInitFn.store(fnArr, std::memory_order_release);
            }
            if (fnArr) {
                g_cruiseApplySuppressUntilMs.store(::GetTickCount64() + 1500,
                                                    std::memory_order_release);

                // Phase 4 Step 7 - write host's state machine value + UI selector
                // BEFORE calling FUN_1413FA140 so its internal copy
                // (DAT_145EB84D4 = DAT_145EB8274) and UI string lookup
                // (DAT_145EB2857 controls Arrival vs EndCruise path) match host.
                // Phase 4 Step 7b - also write orient globals (DAT_145F31DA0/8/0)
                // BEFORE FUN_1413FA140 so the cell-stream task (FUN_140B6EB20)
                // fired during dur countdown picks content based on host's world
                // coords. Phase 3v3 writes orient at EXIT (too late - cell-stream
                // already ran with stale snapshot from cruise ENTER).
                const std::uint32_t state = g_pendingCruise.arrivalState.load(std::memory_order_acquire);
                const std::uint8_t  uiSel = g_pendingCruise.uiSelector.load(std::memory_order_acquire);
                const std::uint64_t bitsX = g_pendingCruise.shipPosX.load(std::memory_order_acquire);
                const std::uint64_t bitsY = g_pendingCruise.shipPosY.load(std::memory_order_acquire);
                const std::uint64_t bitsZ = g_pendingCruise.shipPosZ.load(std::memory_order_acquire);
                const std::uintptr_t base =
                    reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                __try {
                    *reinterpret_cast<std::uint32_t*>(base + kRVA_CruiseArrivalState) = state;
                    *reinterpret_cast<std::uint8_t*>(base + kRVA_CruiseUiSelector) = uiSel;
                    if (bitsX != 0 || bitsY != 0 || bitsZ != 0) {
                        double dX, dY, dZ;
                        std::memcpy(&dX, &bitsX, sizeof(dX));
                        std::memcpy(&dY, &bitsY, sizeof(dY));
                        std::memcpy(&dZ, &bitsZ, sizeof(dZ));
                        *reinterpret_cast<double*>(base + kRVA_CruiseOrient0) = dX;
                        *reinterpret_cast<double*>(base + kRVA_CruiseOrient1) = dY;
                        *reinterpret_cast<double*>(base + kRVA_CruiseOrient2) = dZ;
                        PLOG_INFO(
                            "[PULSAR] L.5 Phase 4 Step 7/7b: wrote state=0x{:08X} uiSel={} orient=({:.0f},{:.0f},{:.0f}) before arrival init"sv,
                            state, static_cast<int>(uiSel), dX, dY, dZ);
                    } else {
                        PLOG_INFO(
                            "[PULSAR] L.5 Phase 4 Step 7: wrote state=0x{:08X} uiSel={} (no orient) before arrival init"sv,
                            state, static_cast<int>(uiSel));
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    PLOG_WARN("[PULSAR] L.5 Phase 4 Step 7/7b: pre-write SEH"sv);
                }

                LogCruiseSnapshot("APPLY-PRE ARRIVAL-INIT");
                PLOG_INFO(
                    "[PULSAR] L.5 Phase 4 Step 6: calling FUN_1413FA140 @ 0x{:X} (arrival init)"sv,
                    fnArr);
                __try {
                    auto fnPtr = reinterpret_cast<CruiseArrivalInitFn>(fnArr);
                    fnPtr();
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    PLOG_WARN("[PULSAR] L.5 Phase 4 Step 6: FUN_1413FA140 SEH"sv);
                }
                LogCruiseSnapshot("APPLY-POST ARRIVAL-INIT");

                // Phase 4 Step 12 - schedule marker re-eval 500ms from now
                // so engine state has time to settle after arrival init's
                // gate flips + cell-stream task runs.
                g_markerNotifyDueMs.store(::GetTickCount64() + kMarkerNotifyDeferMs,
                                          std::memory_order_release);
            } else {
                PLOG_WARN("[PULSAR] L.5 Phase 4 Step 6: arrival-init fn unresolved"sv);
            }
        }

        if (!g_pendingCruise.armed.load(std::memory_order_acquire)) return;

        const std::uint8_t enable = g_pendingCruise.enable.load(std::memory_order_acquire);
        g_pendingCruise.armed.store(false, std::memory_order_release);

        // Resolve both enter/exit fns once.
        std::uintptr_t fnEnter = g_cruiseEnterFn.load(std::memory_order_acquire);
        if (!fnEnter) {
            fnEnter = REL::Offset{ kRVA_CruiseEnter }.address();
            g_cruiseEnterFn.store(fnEnter, std::memory_order_release);
        }
        std::uintptr_t fnExit = g_cruiseExitFn.load(std::memory_order_acquire);
        if (!fnExit) {
            fnExit = REL::Offset{ kRVA_CruiseExit }.address();
            g_cruiseExitFn.store(fnExit, std::memory_order_release);
        }
        if (!fnEnter || !fnExit) {
            PLOG_WARN("[PULSAR] L.5 CruiseApply: failed to resolve enter/exit fns"sv);
            return;
        }

        // Suppress the next ~1.5s of poll-driven broadcasts so we don't echo
        // back the transition we're about to cause locally.
        g_cruiseApplySuppressUntilMs.store(::GetTickCount64() + 1500,
                                            std::memory_order_release);

        // L.5 D3 - snapshot client state BEFORE applying remote toggle.
        LogCruiseSnapshot(enable ? "APPLY-PRE ENTER" : "APPLY-PRE EXIT");

        if (enable) {
            // Phase 4 Step 4 - write host's resolved destination formID into
            // DAT_145EB8284 BEFORE the force-ENTER call. force=true skips the
            // engine's own resolver (FUN_1413FDFD0) which would normally set
            // this field, so without our write it stays 0 → per-tick arrival
            // check (FUN_1413FE900) idles → cruise math doesn't advance and
            // POI/ship rendering desyncs from host.
            const std::uint32_t destId = g_pendingCruise.destFormId.load(std::memory_order_acquire);
            if (destId != 0) {
                const std::uintptr_t base =
                    reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                __try {
                    *reinterpret_cast<std::uint32_t*>(base + kRVA_CruiseDistance) = destId;
                    PLOG_INFO(
                        "[PULSAR] L.5 Phase 4: wrote host dest formID 0x{:08X} to DAT_145EB8284 before force-ENTER"sv,
                        destId);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    PLOG_WARN("[PULSAR] L.5 Phase 4: dest-formID write SEH"sv);
                }
            }

            PLOG_INFO(
                "[PULSAR] L.5 CruiseApply ENTER: calling enter fn @ 0x{:X}"sv,
                fnEnter);
            __try {
                auto fnPtr = reinterpret_cast<CruiseEnterFn>(fnEnter);
                fnPtr(true);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN("[PULSAR] L.5 CruiseApply ENTER: SEH exception"sv);
            }

            // L.5 Phase 4 Step 14 - force=true on client SKIPS the resolver
            // (FUN_1413FDFD0) at offset 0x1413FDB2E. The resolver iterates
            // world manager's POI list, sets nearest-POI tracking state, and
            // populates fields read by planet rendering. User observation:
            // even with cruise byte synced, client doesn't see ANY planets
            // host sees. → manually call the resolver after force-enter to
            // run the skipped setup. SEH-guarded; fatal-latch on first crash.
            if (!g_step14Fatal.load(std::memory_order_acquire)) {
                std::uintptr_t fnRes = REL::Offset{ kRVA_CruiseResolverFn }.address();
                if (fnRes) {
                    using ResolverFn = void(__fastcall*)(void* outA, int mode, void* outB);
                    // Stack-allocated outputs sized generously per disasm
                    // (engine uses 16B for outA, 12B for outB; we give 32B).
                    alignas(8) std::uint8_t outA[32]{};
                    alignas(8) std::uint8_t outB[32]{};
                    bool ok = true;
                    __try {
                        reinterpret_cast<ResolverFn>(fnRes)(outA, 0, outB);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        ok = false;
                        g_step14Fatal.store(true, std::memory_order_release);
                    }
                    PLOG_INFO(
                        "[PULSAR] L.5 Phase 4 Step 14: resolver fired result={}"sv,
                        ok ? "ok" : "SEH");
                }
            }
        } else {
            // L.5 Phase 3v3 - write host's world position into the engine's
            // 3 globals BEFORE the EXIT call. The exit-cleanup path uses these
            // doubles to derive the final ship position (visible planets / NPC
            // ships are loaded relative to this position). Without this write,
            // client's exit settles at its OWN locally-computed position which
            // drifted from host's during cruise.
            const std::uint64_t bitsX = g_pendingCruise.shipPosX.load(std::memory_order_acquire);
            const std::uint64_t bitsY = g_pendingCruise.shipPosY.load(std::memory_order_acquire);
            const std::uint64_t bitsZ = g_pendingCruise.shipPosZ.load(std::memory_order_acquire);
            if (bitsX != 0 || bitsY != 0 || bitsZ != 0) {
                const std::uintptr_t base =
                    reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                double dX, dY, dZ;
                std::memcpy(&dX, &bitsX, sizeof(dX));
                std::memcpy(&dY, &bitsY, sizeof(dY));
                std::memcpy(&dZ, &bitsZ, sizeof(dZ));
                __try {
                    *reinterpret_cast<double*>(base + kRVA_CruiseOrient0) = dX;
                    *reinterpret_cast<double*>(base + kRVA_CruiseOrient1) = dY;
                    *reinterpret_cast<double*>(base + kRVA_CruiseOrient2) = dZ;
                    PLOG_INFO(
                        "[PULSAR] L.5 Phase 3v3: wrote host world pos before EXIT: ({:.0f},{:.0f},{:.0f})"sv,
                        dX, dY, dZ);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    PLOG_WARN("[PULSAR] L.5 Phase 3v3: world-pos write SEH"sv);
                }
            }

            PLOG_INFO(
                "[PULSAR] L.5 CruiseApply EXIT: calling exit fn @ 0x{:X}"sv,
                fnExit);
            __try {
                auto fnPtr = reinterpret_cast<CruiseExitFn>(fnExit);
                fnPtr();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN("[PULSAR] L.5 CruiseApply EXIT: SEH exception"sv);
            }

            // Phase 4 deep - explicit cell-stream re-fire post-EXIT was attempted
            // (FUN_140B6EB20 with worldState ptr arg) but consistently SEH'd.
            // Removed; the natural engine call during dur countdown handles
            // cell content (NPCs/POI station load successfully).

            // Phase 4 deep iter2 - LocationMarkerArrayUpdate::Event refire
            // attempted but SEH'd (game-thread context not satisfied).
            // Removed; planet rendering needs deeper sync of cruise per-tick state.
        }

        // L.5 D3 - snapshot client state AFTER apply. Diff against PRE
        // tells us what the engine ENTER/EXIT fn actually did.
        LogCruiseSnapshot(enable ? "APPLY-POST ENTER" : "APPLY-POST EXIT");
    }

    // SEH-safe wrappers for force-unseat. Helpers used by TickPostLoadUnseat
    // so that function avoids object-unwinding restrictions on __try.
    static void SafeNotifyAnim(RE::Actor* a, const RE::BSFixedString& evt)
    {
        if (!a) return;
        __try { a->NotifyAnimationGraphImpl(evt); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // Returns furniture handle (0 if not seated or read-error).
    static std::uint32_t SafeReadFurnHandle(RE::Actor* a)
    {
        std::uint32_t h = 0;
        if (!a) return 0;
        __try {
            auto* aBytes = reinterpret_cast<std::uint8_t*>(a);
            auto* proc = *reinterpret_cast<std::uint8_t**>(aBytes + 0x228);
            if (proc) {
                auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x08);
                if (mh) {
                    h = *reinterpret_cast<std::uint32_t*>(mh + 0x428);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return h;
    }
    static void SafeClearFurnHandlePlayer(RE::Actor* a)
    {
        if (!a) return;
        __try {
            auto* aBytes = reinterpret_cast<std::uint8_t*>(a);
            auto* proc = *reinterpret_cast<std::uint8_t**>(aBytes + 0x228);
            if (proc) {
                auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x08);
                if (mh) *reinterpret_cast<std::uint32_t*>(mh + 0x428) = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // L.7 Path A.2 - close SpaceshipHud menu layer post-load.
    // Out-of-line so __try doesn't trip C2712 on the caller, which has
    // unwinding locals.
    static bool SafeCloseSpaceshipHudImpl()
    {
        auto* q = RE::UIMessageQueue::GetSingleton();
        if (!q) return false;
        static const RE::BSFixedString kSpaceshipHud("SpaceshipHud");
        static const RE::BSFixedString kTakeoffMenu("TakeoffMenu");
        q->AddMessage(kSpaceshipHud, RE::UI_MESSAGE_TYPE::kHide);
        q->AddMessage(kTakeoffMenu, RE::UI_MESSAGE_TYPE::kHide);
        return true;
    }
    static bool SafeCloseSpaceshipHud()
    {
        __try { return SafeCloseSpaceshipHudImpl(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // L.7 Path A.3 - empirically verified bits that flip when the
    // engine sets the pilot lock state from a save: actor+0x37c bit 4
    // = "in vehicle" (per FUN_141aeb6f0 RE), actorState2 bit 12.
    // Clearing these alone with the 30s retry-loop broke pilot handover;
    // the safe approach is a 1-shot clear AFTER TriggerExitShip + HUD
    // kHide, so the engine has a chance to do the proper state-machine
    // tear-down first and we only patch the residual bits if any.
    static void SafeClearPilotBindingImpl(RE::Actor* a)
    {
        auto* aBytes = reinterpret_cast<std::uint8_t*>(a);
        auto* a37c = reinterpret_cast<volatile std::uint32_t*>(aBytes + 0x37c);
        const std::uint32_t prev37c = *a37c;
        const std::uint32_t next37c = prev37c & ~static_cast<std::uint32_t>(0x10);
        *a37c = next37c;

        const std::uint32_t prevS2 = a->actorState2;
        const std::uint32_t nextS2 = prevS2 & ~static_cast<std::uint32_t>(0x1000);
        a->actorState2 = nextS2;

        PLOG_INFO(
            "[L.7] Path A.3: bit-clear a37c 0x{:08X}->0x{:08X} actS2 0x{:08X}->0x{:08X}"sv,
            prev37c, next37c, prevS2, nextS2);
    }
    static void SafeClearPilotBinding(RE::Actor* a)
    {
        if (!a) return;
        __try { SafeClearPilotBindingImpl(a); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[L.7] Path A.3: SEH on bit-clear"sv);
        }
    }

    // L.5 Phase 3v4 - post-load force-unseat for client. 1-SHOT fire 5s
    // post-load. After save-bridge load the save contains host's
    // "I am piloting" state - client's player may end up seated as pilot.
    // L.3 V4 then re-detects host-as-pilot and resumes proper sync.
    //
    // History: L.7 expanded this to a 30s retry-loop to handle engine
    // re-affirmation after the first clear. That broke normal pilot
    // handover: any save-load within the past 30s → user-initiated chair
    // sit-down would get its furniture handle cleared by the loop, leaving
    // the user stuck in chair with no UI (regression observed 2026-05-06
    // 13:50). REVERTED to 1-shot. Re-affirmation edge case handled less
    // robustly but pilot handover works for normal gameplay - that's the
    // bigger-impact requirement.
    static std::atomic<std::uint64_t> g_lastUnseatHandledLoadTick{ 0 };
    // Kept for the IsPostLoadUnseatInProgress public API. Set true during
    // the 5s settle window so Main.cpp can defer the post-teleport apply
    // while the engine is still completing post-load init.
    static std::atomic<bool> g_unseatInProgress{ false };

    // (g_postBridgeInputLockoutUntilMs hoisted up - see decl near g_isActivePilot.)

    static void TickPostLoadUnseat()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected() || net.IsHost()) {
            g_unseatInProgress.store(false, std::memory_order_release);
            return;
        }

        const std::uint64_t lastLoad = Pulsar::WorldSync::GetLastLoadCompletedTick();
        if (lastLoad == 0) {
            g_unseatInProgress.store(false, std::memory_order_release);
            return;
        }
        // Only handle each load tick once.
        if (lastLoad == g_lastUnseatHandledLoadTick.load(std::memory_order_acquire)) {
            g_unseatInProgress.store(false, std::memory_order_release);
            return;
        }
        // Wait for engine settle (5s) so the save-load actor pointers are stable.
        const std::uint64_t now = ::GetTickCount64();
        if ((now - lastLoad) < 5000) {
            // Mark in-progress while we wait - main apply path defers.
            g_unseatInProgress.store(true, std::memory_order_release);
            return;
        }

        // Latch this load tick BEFORE doing anything so we don't re-fire.
        g_lastUnseatHandledLoadTick.store(lastLoad, std::memory_order_release);

        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) {
            g_unseatInProgress.store(false, std::memory_order_release);
            return;
        }

        const std::uint32_t furnFormID = SafeReadFurnHandle(p);
        if (furnFormID == 0) {
            PLOG_INFO("[L.5] Unseat: not seated post-load - skip"sv);
            g_unseatInProgress.store(false, std::memory_order_release);
            return;
        }

        PLOG_INFO(
            "[L.5] Unseat: client seated post-load (furn=0x{:08X}) - 1-shot clear"sv,
            furnFormID);
        NASLOG("L5-288 UNSEAT 1-shot furn=0x%08X", furnFormID);

        // L.7 Path A.2 only: HIDE pilot HUD + TakeoffMenu so the
        // cockpit overlay disappears post-load. User then holds E ~2s
        // to unseat manually; engine's own press-and-hold handler runs
        // the proper anim + state-machine transition.
        //
        // What we DON'T do here (and why) - destructive Path A.1/A.3
        // steps were REMOVED 2026-05-07 because they sabotaged manual
        // E-unseat:
        //   * SafeClearFurnHandlePlayer (writes 0 → mh+0x428): engine's
        //     E-handler reads this furn handle to know "user is in
        //     chair X". Cleared → engine sees "not in any chair" → E
        //     does nothing.
        //   * SafeClearPilotBinding (clears actor+0x37c bit 4 +
        //     actorState2 bit 12): engine's pilot-state machine reads
        //     these. Cleared → half-unseat → user-visible slide-out
        //     of chair pose without proper anim.
        //   * SafeNotifyAnim "GetUpStart"/"ChairGetUp"/"ExitFurniture":
        //     fire anim transitions out of chair without engine state
        //     agreeing → confused mid-state.
        //   * TriggerExitShip (BSTGlobalEvent): may interact with
        //     engine sinks in same way; conservative removal until we
        //     confirm it's harmless.
        // Logs 2026-05-07 21:33-21:34 showed the destructive clears
        // ran (Path A "OK") yet the user reported stuck/sliding. With
        // engine state machines left untouched, manual E-hold should
        // work cleanly per the user's earlier pre-mechanical-clear
        // observation.
        const bool hudHideOk = SafeCloseSpaceshipHud();
        PLOG_INFO(
            "[L.7] post-load HUD+TakeoffMenu kHide = {} (furn=0x{:08X}) - "
            "user must hold E ~2s to unseat"sv,
            hudHideOk ? "OK" : "noop/SEH", furnFormID);
        NASLOG("L7-299d HUD-kHide=%s furn=0x%08X",
               hudHideOk ? "OK" : "noop", furnFormID);

        // L.7 arm 30s periodic [t+Xs] diag dump so we can observe
        // the post-load evolution of the actor state.
        Pulsar::Diag293::OnHostCloneUnseated();

        // L.7/arm 8s post-bridge input lockout. While active,
        // the FlightInput hook zeroes the client's local input AND
        // IsLocalDeterministicPilot returns false (suppresses pilot-claim).
        // Was 30s in; shortened 2026-05-07 because if the host stood
        // up during the lockout window, BOTH sides ended up passive (host
        // unseated → not pilot, client suppressed by lockout → not pilot)
        // → no ship-transform broadcasts → ship sync froze. 8s is enough
        // for the engine post-load settle (5s in TickPostLoadUnseat) plus
        // a small input grace window. The lockout also auto-releases when
        // the user manually unseats (furn handle=0) - see TickEarlyReleaseLockout.
        const std::uint64_t lockoutEnd = ::GetTickCount64() + 8000;
        g_postBridgeInputLockoutUntilMs.store(lockoutEnd, std::memory_order_release);
        PLOG_INFO(
            "[L.7] FlightInput LOCKOUT armed for 8s (until tick {})"sv,
            lockoutEnd);

        g_unseatInProgress.store(false, std::memory_order_release);
    }

    // L.7 early-release the post-bridge lockout when the local
    // player has unseated (furn handle = 0). Once unseated, the saved
    // pilot-binding state is naturally drained - no need to keep
    // suppressing pilot detection or zeroing input. Without this, if the
    // host stood up first, both sides would stay passive until lockout
    // timer expires → ship sync gap.
    static void TickEarlyReleaseLockout()
    {
        const std::uint64_t lockoutUntil =
            g_postBridgeInputLockoutUntilMs.load(std::memory_order_acquire);
        if (lockoutUntil == 0) return;
        if (::GetTickCount64() >= lockoutUntil) return;  // expired naturally

        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        std::uint32_t furn = SafeReadFurnHandle(p);
        if (furn != 0) return;  // still seated - keep lockout

        g_postBridgeInputLockoutUntilMs.store(0, std::memory_order_release);
        PLOG_INFO("[L.7] post-bridge lockout released early (user unseated)"sv);
        NASLOG("L7-299g lockout released early (user unseated)");
    }

    // L.7 Cockpit-overlap clone invisibility.
    //
    // After bridge load both avatars often share a tiny ship interior
    // and at least one of them is seated in the pilot chair. The user's
    // forward view is blocked by the partner's clone (visible behind
    // chair, sometimes overlapping). Rule (revision): if the
    // local player is seated AND the remote clone is in the same cell,
    // hide the clone - regardless of where the clone visually stands
    // (per user request 2026-05-07). When local stands up (furn handle
    // becomes 0), clone becomes visible again.
    //
    // Earlier revision compared furn handles for "both in same chair"
    // overlap, but VehicleSync's clone-into-furniture snap (snap=0 in
    // 21:50:49 logs) often fails post-bridge → clone never gets a furn
    // handle → invisibility never triggered.
    //
    // Layout used:
    //   Actor + 0xB8 = loadedData ptr (read raw, BSGuarded slot)
    //   loadedData + 0x08 = data3D (NiAVObject*)
    //   NiAVObject + 0x118 = flags (uint64); bit 0 = AppCulled
    static std::atomic<bool> g_cloneCulledForCoPilot{ false };

    static void SetClone3DAppCulled(RE::Actor* a_clone, bool a_cull) noexcept
    {
        if (!a_clone) return;
        __try {
            auto* p = reinterpret_cast<std::uint8_t*>(a_clone);
            std::uintptr_t loadedData = *reinterpret_cast<std::uintptr_t*>(p + 0xB8);
            if (!loadedData) return;
            std::uintptr_t data3D = *reinterpret_cast<std::uintptr_t*>(loadedData + 0x08);
            if (!data3D) return;
            auto& flags = *reinterpret_cast<std::uint64_t*>(data3D + 0x118);
            if (a_cull) flags |= 0x1ull;
            else        flags &= ~0x1ull;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void TickCoPilotInvisibility()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) {
            if (g_cloneCulledForCoPilot.exchange(false, std::memory_order_acq_rel)) {
                auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
                SetClone3DAppCulled(clone, false);
            }
            return;
        }

        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
        if (!p || !clone) return;

        const std::uint32_t hPlayer = SafeReadFurnHandle(p);
        if (hPlayer == 0) {
            // Local not seated → never hide.
            if (g_cloneCulledForCoPilot.exchange(false, std::memory_order_acq_rel)) {
                SetClone3DAppCulled(clone, false);
                PLOG_INFO("[L.7] clone visibility - SHOW (local stood up)"sv);
                NASLOG("L7-299b clone-visibility SHOW (local stood up)");
            }
            return;
        }

        // Local seated. Cell match decides hide.
        std::uint32_t playerCell = 0;
        __try {
            playerCell = p->parentCell ? p->parentCell->formID : 0u;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        const std::uint32_t cloneCell =
            Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteCellID();
        const bool overlap = (playerCell != 0 && playerCell == cloneCell);

        const bool wasCulled = g_cloneCulledForCoPilot.load(std::memory_order_acquire);
        if (overlap == wasCulled) return;

        SetClone3DAppCulled(clone, overlap);
        g_cloneCulledForCoPilot.store(overlap, std::memory_order_release);
        PLOG_INFO(
            "[L.7] clone visibility - {} (local seated furn=0x{:08X}, "
            "cell-match player=0x{:08X} clone=0x{:08X})"sv,
            overlap ? "HIDE" : "SHOW", hPlayer, playerCell, cloneCell);
        NASLOG("L7-299b clone-visibility %s playerCell=0x%08X cloneCell=0x%08X",
               overlap ? "HIDE" : "SHOW", playerCell, cloneCell);
    }

    // L.7 Path A.5 (QuickSave/QuickLoad reset cycle) and Path A.6
    // (SendInput VK_E hold) - both REMOVED 2026-05-07 . A.5 was
    // disruptive (extra load screen) and A.6 crashed clients with
    // NULL+0x24 access violation during the loading menu. Replaced with
    // the simpler "user holds E for ~2s manually" workaround now that
    // SAME-CELL-RESTORE teleport is gone .

    // L.5 Phase 4 Step 12 - DISABLED. FUN_141A1ED40 SEH'd 100% of the time
    // even mid-cruise (test 2026-05-01 22:09). The Notify call walks a sink
    // list that's invalid on client. Replaced with Step 13 below.
    //
    // L.5 Phase 4 Step 13 - call FUN_1409FAC52 on client during cruise to
    // force lazy-init of the singleton at 0x1461E2340 (memory diff Step 10
    // showed this stays uninit on client, while host inits it during cruise).
    // The fn is a magic-static pattern: if already inited returns ptr; else
    // runs init + returns ptr. Idempotent and self-contained. Fire ONCE per
    // cruise session, gated by g_step13FiredThisCruise.
    static constexpr std::uintptr_t kRVA_WmgrLazyInitFn = 0x9FA5F2;
    using WmgrLazyInitFn = void*(__fastcall*)();
    static std::atomic<std::uintptr_t> g_wmgrLazyInitFn{ 0 };

    static void TickMarkerNotify()
    {
        // L.5 Phase 4 Step 12 + 13 both DISABLED.
        // Step 12: FUN_141A1ED40 SEH'd 100% - Notify walks invalid sink list.
        // Step 13: FUN_1409FAC52 hard-crashed client (SEH frame bypassed) when
        //   client initiated cruise on its own - call chain into FUN_1428CFB80
        //   needs engine state client doesn't have.
        // Drain deferred timestamp (left over from Step 12) so it doesn't trip
        // anything else.
        g_markerNotifyDueMs.store(0, std::memory_order_release);
    }

    // L.5 Phase 4 Step 11 - host broadcasts cruise orient @5Hz; client applies
    // received orient directly to DAT_145F31DA0/DA8/DB0. Called from Tick()
    // every frame; gates on cruise-active byte.
    static void TickCruiseOrient()
    {
        if (!IsCruiseActive()) return;
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) return;

        // Step 16 - bidirectional: the cruise originator (whoever initiated
        // locally) broadcasts orient. The receiver applies. Originator's
        // engine actually does cruise math; receiver's force=true skipped it.
        if (g_cruiseSelfInitiated.load(std::memory_order_acquire)) {
            const std::uint64_t now = ::GetTickCount64();
            const std::uint64_t last = g_lastCruiseOrientSendMs.load(std::memory_order_acquire);
            if ((now - last) < kCruiseOrientIntervalMs) return;
            g_lastCruiseOrientSendMs.store(now, std::memory_order_release);

            Pulsar::Network::CruiseOrientData payload{};
            __try {
                payload.posX = *reinterpret_cast<double*>(base + kRVA_CruiseOrient0);
                payload.posY = *reinterpret_cast<double*>(base + kRVA_CruiseOrient1);
                payload.posZ = *reinterpret_cast<double*>(base + kRVA_CruiseOrient2);
            } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
            // Skip if all-zero (engine hasn't populated yet - happens briefly
            // at cruise ENTER before resolver runs).
            if (payload.posX == 0.0 && payload.posY == 0.0 && payload.posZ == 0.0) return;
            // Skip NaN/INF - broken floats break engine elsewhere.
            if (!std::isfinite(payload.posX) || !std::isfinite(payload.posY) ||
                !std::isfinite(payload.posZ)) return;
            payload.timestampMs = now;

            std::uint8_t buf[Pulsar::Network::HEADER_SIZE +
                             sizeof(Pulsar::Network::CruiseOrientData)];
            Pulsar::Network::PacketWriter w(
                Pulsar::Network::PKT_CRUISE_ORIENT, buf, sizeof(buf));
            if (w.Write(payload)) {
                net.Send(w.Data(), w.Size(), false);  // unreliable - stream
            }
        } else {
            // Client: drain pending and write to globals.
            if (!g_pendingCruiseOrient.armed.exchange(false, std::memory_order_acq_rel))
                return;
            const std::uint64_t bX = g_pendingCruiseOrient.bitsX.load(std::memory_order_acquire);
            const std::uint64_t bY = g_pendingCruiseOrient.bitsY.load(std::memory_order_acquire);
            const std::uint64_t bZ = g_pendingCruiseOrient.bitsZ.load(std::memory_order_acquire);
            double dx, dy, dz;
            std::memcpy(&dx, &bX, sizeof(dx));
            std::memcpy(&dy, &bY, sizeof(dy));
            std::memcpy(&dz, &bZ, sizeof(dz));
            __try {
                *reinterpret_cast<double*>(base + kRVA_CruiseOrient0) = dx;
                *reinterpret_cast<double*>(base + kRVA_CruiseOrient1) = dy;
                *reinterpret_cast<double*>(base + kRVA_CruiseOrient2) = dz;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    // L.5 D3+D10 - periodic snapshot during cruise. LogCruiseSnapshot at 1Hz
    // (structured human-readable). LogCruiseMemDump at 10Hz (broad memory
    // hex dump for runtime diff). Both are SEH-guarded.
    static void TickCruiseSnapshot()
    {
        std::uintptr_t ptr = g_cruiseActiveBytePtr.load(std::memory_order_acquire);
        if (!ptr) return;

        std::uint8_t cur = 0;
        __try { cur = *reinterpret_cast<std::uint8_t*>(ptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        if (cur == 0) return;  // only snapshot during active cruise

        const std::uint64_t now = ::GetTickCount64();

        // 10Hz memory diff dump (Step 10).
        const std::uint64_t lastDump = g_lastCruiseMemDumpMs.load(std::memory_order_acquire);
        if ((now - lastDump) >= kCruiseMemDumpIntervalMs) {
            g_lastCruiseMemDumpMs.store(now, std::memory_order_release);
            LogCruiseMemDump("PERIODIC");
        }

        // 1Hz structured snapshot (D3 - kept for human readability).
        const std::uint64_t lastSnap = g_lastCruiseSnapshotMs.load(std::memory_order_acquire);
        if ((now - lastSnap) >= kCruiseSnapshotIntervalMs) {
            g_lastCruiseSnapshotMs.store(now, std::memory_order_release);
            LogCruiseSnapshot("PERIODIC");
        }
    }

    // crash-absorb (interim, real fix tracked separately): the engine notify-dispatch
    // (FUN_1422CB8F0) cascades into the AI condition-eval (0x22CE9xx), which UAF-crashes
    // (0xC0000005 WRITE at null+0x24) when a clone/container is left dangling during the
    // space + F8-bridge transition (clone-despawn-churn, family; see
    // reference_crash_22ce9c6.md). SEH-absorb the original dispatch so a transient
    // transition crash drops one event frame instead of taking the client down. Separate
    // noexcept fn because __try cannot coexist with C++ unwinding in HookedNotifyDispatch.
    // Logging lives in its own fn: PLOG creates temporaries that need C++ unwinding,
    // which cannot appear in the same fn as __try (C2712). The __except body only CALLS
    // this, so the guarded fn stays unwind-free.
    static void LogAbsorbedFault() noexcept
    {
        static std::atomic<std::uint32_t> n{ 0 };
        const std::uint32_t c = n.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c == 1 || (c % 500) == 0)
            PLOG_WARN("[PULSAR] space-bridge dispatch fault ABSORBED x{} (#22CE9 family)"sv, c);
    }

    static void CallOrigNotifyDispatchGuarded(NotifyDispatchFn a_fn,
                                              void* a_eventSource, void* a_dispatcher) noexcept
    {
        __try {
            a_fn(a_eventSource, a_dispatcher);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // dangling-container UAF during space/bridge transition; drop this frame.
            LogAbsorbedFault();
        }
    }

    static void __fastcall HookedNotifyDispatch(void* a_eventSource, void* a_dispatcher)
    {
        // Quick filter: most calls are NOT PMO. The PMO singleton VA is
        // a constant - pointer compare avoids any per-call work for other
        // event types.
        const std::uintptr_t pmoSrc = ResolvePMOEventSourceVA();
        if (reinterpret_cast<std::uintptr_t>(a_eventSource) == pmoSrc) {
            g_notifyPMOCount.fetch_add(1, std::memory_order_relaxed);

            std::uint8_t* payload = SafeGetPayloadFromDispatcher(a_dispatcher);
            if (payload) {
                auto& net = Pulsar::Network::GetSingleton();
                if (net.IsConnected()) {
                    if (net.IsHost()) {
                        // Capture + broadcast (rate-limited).
                        const std::uint64_t now = ::GetTickCount64();
                        const std::uint64_t last = g_lastPMOSendMs.load(std::memory_order_acquire);
                        if ((now - last) >= kPMOSendIntervalMs) {
                            g_lastPMOSendMs.store(now, std::memory_order_release);
                            Pulsar::Network::PMOPayloadData snap{};
                            SafeReadPayload(snap.bytes, payload, kPMOPayloadSize);
                            std::uint8_t buf[128]{};
                            Pulsar::Network::PacketWriter w(
                                Pulsar::Network::PKT_PMO_PAYLOAD, buf, sizeof(buf));
                            if (w.Write(snap)) {
                                net.Send(w.Data(), w.Size(), false);
                            }
                            std::uint64_t lastLog = g_lastNotifyLogMs.load(std::memory_order_acquire);
                            if ((now - lastLog) >= 1000) {
                                g_lastNotifyLogMs.store(now, std::memory_order_release);
                                // Dump all 24 floats so we can spot which
                                // offsets carry live flight input.
                                float f[24]{};
                                __try {
                                    for (int i = 0; i < 24; ++i) {
                                        f[i] = *reinterpret_cast<float*>(payload + i * 4);
                                    }
                                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                                PLOG_INFO(
                                    "[PULSAR] PMO send: pmoCnt={} totalCnt={} pld="
                                    "[{:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} | "
                                    "{:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} | "
                                    "{:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f}]"sv,
                                    g_notifyPMOCount.load(), g_notifyTotalCount.load(),
                                    f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                                    f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15],
                                    f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23]);
                            }
                        }
                    } else {
                        // Client: overwrite payload with received host bytes.
                        const bool armed = g_pendingPMOPayload.armed.load(std::memory_order_acquire);
                        const std::uint64_t recvAt = g_pendingPMOPayload.receivedAtMs.load(std::memory_order_acquire);
                        const std::uint64_t now = ::GetTickCount64();
                        if (armed && (recvAt == 0 || (now - recvAt) <= kPMOStaleMs)) {
                            SafeWritePayload(payload, g_pendingPMOPayload.data.bytes,
                                             kPMOPayloadSize);
                            std::uint64_t lastLog = g_lastPMOApplyLogMs.load(std::memory_order_acquire);
                            if ((now - lastLog) >= 1000) {
                                g_lastPMOApplyLogMs.store(now, std::memory_order_release);
                                // Dump same 24 floats as host send for direct compare.
                                float f[24]{};
                                __try {
                                    for (int i = 0; i < 24; ++i) {
                                        f[i] = *reinterpret_cast<float*>(payload + i * 4);
                                    }
                                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                                PLOG_INFO(
                                    "[PULSAR] PMO apply: ageMs={} pld="
                                    "[{:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} | "
                                    "{:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} | "
                                    "{:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f}]"sv,
                                    (now - recvAt),
                                    f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                                    f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15],
                                    f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23]);
                            }
                        }
                    }
                }
            }
        }
        g_notifyTotalCount.fetch_add(1, std::memory_order_relaxed);

        // V3 - ship NiNode transform sync. The PMO Notify is a stable per-
        // pilot-frame anchor on host AND client, so we piggyback the sync
        // here to avoid a second hook. Gated on PMO-source match.
        if (reinterpret_cast<std::uintptr_t>(a_eventSource) == pmoSrc) {
            MaybeSyncShipTransform();
        }

        // V4 Fire-Probe v3 - match-only against 3 candidate vtables. Drop
        // the newEvt firehose; we already know the vtables don't match the
        // resolved one for OnPlayerFireWeapon (probe v2 had 200 newEvt logs
        // none matching). Try WeaponFired + ProjectileBeginUpdate too.
        if (reinterpret_cast<std::uintptr_t>(a_eventSource) != pmoSrc &&
            a_eventSource != nullptr)
        {
            std::uintptr_t srcVtbl = 0;
            __try {
                srcVtbl = *reinterpret_cast<std::uintptr_t*>(a_eventSource);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            if (srcVtbl != 0) {
                const std::uintptr_t vtA = ResolveFireVt(g_fireVt_OnPlayer, 413845);
                const std::uintptr_t vtB = ResolveFireVt(g_fireVt_Generic , 412442);
                const std::uintptr_t vtC = ResolveFireVt(g_fireVt_ProjBeg , 454462);
                const char* matchKind = nullptr;
                if (srcVtbl == vtA) matchKind = "OnPlayerFire";
                else if (srcVtbl == vtB) matchKind = "WeaponFired";
                else if (srcVtbl == vtC) matchKind = "ProjBeginUpdate";

                if (matchKind != nullptr) {
                    g_fireProbeMatchCount.fetch_add(1);

                    std::uint64_t qw[12]{};
                    std::uint8_t* payload = SafeGetPayloadFromDispatcher(a_dispatcher);
                    if (payload) {
                        __try {
                            for (int i = 0; i < 12; ++i) {
                                qw[i] = *reinterpret_cast<std::uint64_t*>(payload + i * 8);
                            }
                        } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }

                    bool          inSpace = false;
                    std::uint32_t shipFormId = 0;
                    __try {
                        auto* p = RE::PlayerCharacter::GetSingleton();
                        if (p) {
                            inSpace = p->IsInSpace(true);
                            auto* sh = p->GetSpaceship(true);
                            if (sh) shipFormId = sh->formID;
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}

                    // V4 Phase A v5 - for WeaponFired, qw[8] looks like a
                    // per-shot pointer (per-shot delta ~0x40). Probe-test:
                    // deref as Projectile* and dump the same fields. This
                    // moment is later in projectile lifecycle than
                    // ProjBeginUpdate, so transform/handle should be set.
                    if (payload && std::strcmp(matchKind, "WeaponFired") == 0) {
                        std::uint64_t qw8 = qw[8];
                        if (qw8 && qw8 > 0x10000 && qw8 < 0x7FFFFFFFFFFF) {
                            float pos[3]{}, mvDir[3]{}, vel[3]{};
                            std::uint32_t shooterHandle = 0;
                            std::uint32_t projFormId    = 0;
                            float damage = 0.0f;
                            __try {
                                auto* p = reinterpret_cast<std::uint8_t*>(qw8);
                                projFormId    = *reinterpret_cast<std::uint32_t*>(p + 0x28);
                                pos[0]        = *reinterpret_cast<float*>(p + 0x8C);
                                pos[1]        = *reinterpret_cast<float*>(p + 0x90);
                                pos[2]        = *reinterpret_cast<float*>(p + 0x94);
                                mvDir[0]      = *reinterpret_cast<float*>(p + 0x158);
                                mvDir[1]      = *reinterpret_cast<float*>(p + 0x15C);
                                mvDir[2]      = *reinterpret_cast<float*>(p + 0x160);
                                vel[0]        = *reinterpret_cast<float*>(p + 0x164);
                                vel[1]        = *reinterpret_cast<float*>(p + 0x168);
                                vel[2]        = *reinterpret_cast<float*>(p + 0x16C);
                                shooterHandle = *reinterpret_cast<std::uint32_t*>(p + 0x180);
                                damage        = *reinterpret_cast<float*>(p + 0x224);
                            } __except (EXCEPTION_EXECUTE_HANDLER) {}
                            PLOG_INFO(
                                "[PULSAR] FireProbe WF-deref: qw8=0x{:X} projFID=0x{:08X} "
                                "pos=({:.2f},{:.2f},{:.2f}) mvDir=({:.3f},{:.3f},{:.3f}) "
                                "vel=({:.2f},{:.2f},{:.2f}) shooterH=0x{:08X} dmg={:.1f}"sv,
                                qw8, projFormId,
                                pos[0], pos[1], pos[2],
                                mvDir[0], mvDir[1], mvDir[2],
                                vel[0], vel[1], vel[2],
                                shooterHandle, damage);
                        }
                    }

                    // Layout-Probe: dump raw qwords +0x100..+0x1FF for inspection.
                    // (Only for ProjBeginUpdate, first 3 events.)
                    if (payload && std::strcmp(matchKind, "ProjBeginUpdate") == 0
                        && g_fireProbeMatchCount.load(std::memory_order_acquire) <= 3)
                    {
                        std::uint64_t raw[16]{};
                        __try {
                            for (int i = 0; i < 16; ++i) {
                                raw[i] = *reinterpret_cast<std::uint64_t*>(payload + 0x100 + i * 8);
                            }
                        } __except (EXCEPTION_EXECUTE_HANDLER) {}
                        PLOG_INFO(
                            "[PULSAR] FireProbe RAW100: payload=0x{:X} +100..+178="
                            "[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
                            reinterpret_cast<std::uintptr_t>(payload),
                            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                            raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
                    }

                    // V4 Phase A - for ProjBeginUpdate, payload IS the
                    // RE::Projectile* (TESObjectREFR-derived, sizeof 0x250).
                    // Extract V1-relevant fields per CommonLibSF Projectile.h.
                    if (payload && std::strcmp(matchKind, "ProjBeginUpdate") == 0) {
                        float pos[3]{}, mvDir[3]{}, vel[3]{};
                        std::uint32_t shooterHandle = 0;
                        std::uint32_t projFormId    = 0;
                        std::uint64_t baseFormPtr   = 0;  // BGSProjectile*
                        std::uint64_t weaponPtr     = 0;  // TESObjectWEAP*
                        float damage = 0.0f, power = 0.0f, speedMult = 0.0f, range = 0.0f;
                        __try {
                            // TESForm.formID @ +0x28
                            projFormId = *reinterpret_cast<std::uint32_t*>(payload + 0x28);
                            // TESObjectREFR.data.location @ +0x8C..+0x94
                            pos[0] = *reinterpret_cast<float*>(payload + 0x8C);
                            pos[1] = *reinterpret_cast<float*>(payload + 0x90);
                            pos[2] = *reinterpret_cast<float*>(payload + 0x94);
                            // Projectile.movementDirection @ +0x158
                            mvDir[0] = *reinterpret_cast<float*>(payload + 0x158);
                            mvDir[1] = *reinterpret_cast<float*>(payload + 0x15C);
                            mvDir[2] = *reinterpret_cast<float*>(payload + 0x160);
                            // Projectile.velocity @ +0x164
                            vel[0] = *reinterpret_cast<float*>(payload + 0x164);
                            vel[1] = *reinterpret_cast<float*>(payload + 0x168);
                            vel[2] = *reinterpret_cast<float*>(payload + 0x16C);
                            // Projectile.shooterHandle @ +0x180
                            shooterHandle = *reinterpret_cast<std::uint32_t*>(payload + 0x180);
                            // Projectile.weaponSource.object @ +0x1F0 (BGSObjectInstanceT.object)
                            weaponPtr = *reinterpret_cast<std::uint64_t*>(payload + 0x1F0);
                            // Damage / shot params
                            power     = *reinterpret_cast<float*>(payload + 0x214);
                            speedMult = *reinterpret_cast<float*>(payload + 0x218);
                            range     = *reinterpret_cast<float*>(payload + 0x21C);
                            damage    = *reinterpret_cast<float*>(payload + 0x224);
                            // TESObjectREFR base form (objectReference). CommonLibSF:
                            // OBJ_REFR data @ +0x80, objectReference @ data+0x18 => +0x98.
                            // (The old +0x68 was a wrong guess → garbage baseFormId that
                            //  didn't resolve on the client, slice3 st=3.)
                            baseFormPtr = *reinterpret_cast<std::uint64_t*>(payload + 0x98);
                        } __except (EXCEPTION_EXECUTE_HANDLER) {}

                        // Resolve weapon/base FormID via TESForm.formID @ +0x28
                        std::uint32_t weaponFormId = 0;
                        std::uint32_t baseFormId   = 0;
                        __try {
                            if (weaponPtr) {
                                weaponFormId = *reinterpret_cast<std::uint32_t*>(weaponPtr + 0x28);
                            }
                            if (baseFormPtr) {
                                baseFormId = *reinterpret_cast<std::uint32_t*>(baseFormPtr + 0x28);
                            }
                        } __except (EXCEPTION_EXECUTE_HANDLER) {}

                        PLOG_INFO(
                            "[PULSAR] FireProbe Proj: projFID=0x{:08X} baseFID=0x{:08X} "
                            "weaponFID=0x{:08X} shooterH=0x{:08X} ourShip=0x{:08X} "
                            "pos=({:.2f},{:.2f},{:.2f}) mvDir=({:.3f},{:.3f},{:.3f}) "
                            "vel=({:.2f},{:.2f},{:.2f}) dmg={:.1f} power={:.2f} spdMult={:.2f} range={:.0f}"sv,
                            projFormId, baseFormId, weaponFormId, shooterHandle, shipFormId,
                            pos[0], pos[1], pos[2],
                            mvDir[0], mvDir[1], mvDir[2],
                            vel[0], vel[1], vel[2],
                            damage, power, speedMult, range);

                        // L.4 V1 - SEND PKT_SHIP_FIRE if we are the active
                        // pilot. baseFormId / weaponFormId extracted above.
                        // Fires twice per click (paired cannons); V1.1 will
                        // dedupe if needed.
                        auto& netFire = Pulsar::Network::GetSingleton();
                        if (netFire.IsConnected() && netFire.IsHost()
                            && g_isActivePilot.load(std::memory_order_acquire)
                            && shipFormId != 0)
                        {
                            Pulsar::Network::ShipFireData fire{};
                            fire.shooterShipFormId = shipFormId;
                            fire.weaponFormId      = weaponFormId;
                            fire.baseProjFormId    = baseFormId;
                            fire.timestampMs       = ::GetTickCount64();
                            // slice 3 - real shot geometry so the client can
                            // spawn the SAME aimed bolt (host already aimed). All
                            // already read above from the live RE::Projectile.
                            fire.origin[0] = pos[0];
                            fire.origin[1] = pos[1];
                            fire.origin[2] = pos[2];
                            fire.dir[0]    = mvDir[0];
                            fire.dir[1]    = mvDir[1];
                            fire.dir[2]    = mvDir[2];
                            fire.power     = power;
                            fire.range     = range;
                            fire.speedMult = speedMult;
                            fire.vel[0]    = vel[0];
                            fire.vel[1]    = vel[1];
                            fire.vel[2]    = vel[2];
                            std::uint8_t buf[96]{};
                            Pulsar::Network::PacketWriter w(
                                Pulsar::Network::PKT_SHIP_FIRE, buf, sizeof(buf));
                            if (w.Write(fire)) {
                                netFire.Send(w.Data(), w.Size(), false);
                                PLOG_INFO(
                                    "[PULSAR] L.4 fire SEND: shooter=0x{:08X} "
                                    "weapon=0x{:08X} baseProj=0x{:08X}"sv,
                                    fire.shooterShipFormId, fire.weaponFormId,
                                    fire.baseProjFormId);
                            }
                        }
                    }

                    PLOG_INFO(
                        "[PULSAR] FireProbe MATCH-{}: src=0x{:X} vt=0x{:X} "
                        "inSpace={} ship=0x{:08X} payloadPtr=0x{:X} "
                        "qw[0..11]=[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
                        matchKind,
                        reinterpret_cast<std::uintptr_t>(a_eventSource), srcVtbl,
                        inSpace ? 1 : 0, shipFormId,
                        reinterpret_cast<std::uintptr_t>(payload),
                        qw[0], qw[1], qw[2], qw[3], qw[4], qw[5],
                        qw[6], qw[7], qw[8], qw[9], qw[10], qw[11]);
                }
            }
        }

        // L.5 Phase 1 - Cruise event probe. Match a_eventSource->vtable
        // against the 4 known cruise BSTGlobalEvent::EventSource vtables and
        // log payload qwords. Logging-only; Phase 2 will add network sends.
        if (a_eventSource != nullptr) {
            std::uintptr_t srcVtblC = 0;
            __try {
                srcVtblC = *reinterpret_cast<std::uintptr_t*>(a_eventSource);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            if (srcVtblC != 0) {
                const std::uintptr_t vtAct = ResolveFireVt(g_cruiseVt_Activate , 1036687);
                const std::uintptr_t vtExt = ResolveFireVt(g_cruiseVt_ExitAnim , 1036685);
                const std::uintptr_t vtLck = ResolveFireVt(g_cruiseVt_LockCourse, 1036683);
                const std::uintptr_t vtArr = ResolveFireVt(g_cruiseVt_Arrival  , 1037134);
                const char* cruiseKind = nullptr;
                if      (srcVtblC == vtAct) cruiseKind = "OnCruiseActivate";
                else if (srcVtblC == vtExt) cruiseKind = "OnCruiseExitAnimFinished";
                else if (srcVtblC == vtLck) cruiseKind = "OnCruiseLockCourse";
                else if (srcVtblC == vtArr) cruiseKind = "SpaceCruiseArrival";

                // L.5 Phase 1.5 EvtDisco probe removed - Phase 0 RE done,
                // cruise = AVCruise ActorValue. Direct hook on EnableCruiseMode
                // installed below; generic event-source discovery no longer
                // needed.

                if (cruiseKind != nullptr) {
                    g_cruiseProbeHitCount.fetch_add(1, std::memory_order_relaxed);

                    std::uint64_t qw[12]{};
                    std::uint8_t* payload = SafeGetPayloadFromDispatcher(a_dispatcher);
                    if (payload) {
                        __try {
                            for (int i = 0; i < 12; ++i) {
                                qw[i] = *reinterpret_cast<std::uint64_t*>(payload + i * 8);
                            }
                        } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }

                    bool          inSpace = false;
                    std::uint32_t shipFormId = 0;
                    __try {
                        auto* p = RE::PlayerCharacter::GetSingleton();
                        if (p) {
                            inSpace = p->IsInSpace(true);
                            auto* sh = p->GetSpaceship(true);
                            if (sh) shipFormId = sh->formID;
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}

                    auto& netC = Pulsar::Network::GetSingleton();
                    PLOG_INFO(
                        "[PULSAR] L.5 CruiseProbe MATCH-{}: hit#{} src=0x{:X} vt=0x{:X} "
                        "isHost={} inSpace={} ship=0x{:08X} payloadPtr=0x{:X} "
                        "qw[0..11]=[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
                        cruiseKind,
                        g_cruiseProbeHitCount.load(),
                        reinterpret_cast<std::uintptr_t>(a_eventSource), srcVtblC,
                        netC.IsConnected() && netC.IsHost() ? 1 : 0,
                        inSpace ? 1 : 0, shipFormId,
                        reinterpret_cast<std::uintptr_t>(payload),
                        qw[0], qw[1], qw[2], qw[3], qw[4], qw[5],
                        qw[6], qw[7], qw[8], qw[9], qw[10], qw[11]);
                }
            }
        }

        if (g_origNotifyDispatch) {
            CallOrigNotifyDispatchGuarded(g_origNotifyDispatch, a_eventSource, a_dispatcher);
        }
    }

    // Helper: read 3 floats from handler+0x40 + 1 byte from +0xA4. SEH-guarded.
    static bool SafeReadInputBuf(void* a_handler, float& a_x, float& a_y,
                                 float& a_z, std::uint8_t& a_pilotByte)
    {
        a_x = a_y = a_z = 0.0f; a_pilotByte = 0;
        if (!a_handler) return false;
        __try {
            auto* p = reinterpret_cast<std::uint8_t*>(a_handler);
            a_x = *reinterpret_cast<float*>(p + 0x40);
            a_y = *reinterpret_cast<float*>(p + 0x44);
            a_z = *reinterpret_cast<float*>(p + 0x48);
            a_pilotByte = *reinterpret_cast<std::uint8_t*>(p + 0xA4);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    static void SafeWriteInputBuf(void* a_handler, float a_x, float a_y,
                                  float a_z, std::uint8_t a_pilotByte)
    {
        if (!a_handler) return;
        __try {
            auto* p = reinterpret_cast<std::uint8_t*>(a_handler);
            *reinterpret_cast<float*>(p + 0x40) = a_x;
            *reinterpret_cast<float*>(p + 0x44) = a_y;
            *reinterpret_cast<float*>(p + 0x48) = a_z;
            *reinterpret_cast<std::uint8_t*>(p + 0xA4) = a_pilotByte;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // V2.x diagnostic: count hook invocations regardless of conditions.
    static std::atomic<std::uint64_t> g_hookFireCount{ 0 };
    static std::atomic<std::uint64_t> g_lastHookLogMs{ 0 };

    // Hook: post-call to FUN_14068f440 (input handler). Engine has already
    // read player input + written to handler+0x40. We then either capture
    // and broadcast (host) or overwrite with received (client).
    static void __fastcall HookedFlightInputProcess(void* a_handler, void* a_event)
    {
        // Diagnostic - log every 1s regardless of network state, so we know
        // whether vt[7] is even called per-frame during flight.
        std::uint64_t fc = g_hookFireCount.fetch_add(1);
        std::uint64_t now0 = ::GetTickCount64();
        std::uint64_t last0 = g_lastHookLogMs.load(std::memory_order_acquire);
        if ((now0 - last0) >= 1000) {
            g_lastHookLogMs.store(now0, std::memory_order_release);
            PLOG_INFO("[PULSAR] FlightInput hook fired: count={} handler=0x{:X}"sv,
                      fc, reinterpret_cast<std::uintptr_t>(a_handler));
            NASLOG("FlightInput hook fired: count=%llu handler=0x%llX",
                   (unsigned long long)fc,
                   (unsigned long long)reinterpret_cast<std::uintptr_t>(a_handler));
        }

        // Always run engine logic first - never skip it (would break input).
        if (g_origFlightInputProcess) {
            g_origFlightInputProcess(a_handler, a_event);
        }

        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        if (!a_handler) return;

        const std::uint64_t now = ::GetTickCount64();

        if (net.IsHost()) {
            // Active pilot - capture and broadcast.
            float x = 0, y = 0, z = 0;
            std::uint8_t pilotByte = 0;
            if (!SafeReadInputBuf(a_handler, x, y, z, pilotByte)) return;

            // Rate limit.
            const std::uint64_t last = g_lastInputSendMs.load(std::memory_order_acquire);
            if ((now - last) < kInputSendIntervalMs) return;
            g_lastInputSendMs.store(now, std::memory_order_release);

            Pulsar::Network::FlightInputData payload{};
            payload.axis0     = x;
            payload.axis1     = y;
            payload.axis2     = z;
            payload.pilotByte = pilotByte;
            payload.hasInput  = 1;

            std::uint8_t buf[32]{};
            Pulsar::Network::PacketWriter w(
                Pulsar::Network::PKT_FLIGHT_INPUT, buf, sizeof(buf));
            if (!w.Write(payload)) return;
            net.Send(w.Data(), w.Size(), false);  // unreliable; high-rate

            std::uint64_t cnt = g_inputDiagCount.fetch_add(1);
            if ((cnt % 60) == 0) {
                PLOG_INFO(
                    "[PULSAR] FlightInput send: ax=({:.3f},{:.3f},{:.3f}) pByte=0x{:02X} handler=0x{:X}"sv,
                    x, y, z, pilotByte, reinterpret_cast<std::uintptr_t>(a_handler));
                NASLOG(
                    "FlightInput send: ax=(%.3f,%.3f,%.3f) pByte=0x%02X handler=0x%llX",
                    x, y, z, pilotByte,
                    (unsigned long long)reinterpret_cast<std::uintptr_t>(a_handler));
            }
        } else {
            // L.7 post-bridge input lockout. If the client just
            // finished a save-bridge load AND host is real pilot, the
            // engine still routes the client's local WASD into the
            // ship-control input handler (separate engine layer that
            // our actor-state clear and save-patcher don't reach).
            // Zero the input buffer for 30s post-load to mask the
            // symptom: ship sees "no input" → doesn't respond to local
            // WASD until the lockout expires.
            const std::uint64_t lockoutUntil =
                g_postBridgeInputLockoutUntilMs.load(std::memory_order_acquire);
            if (lockoutUntil != 0 && now < lockoutUntil) {
                SafeWriteInputBuf(a_handler, 0.0f, 0.0f, 0.0f, 0);
                std::uint64_t cnt = g_inputDiagCount.fetch_add(1);
                if ((cnt % 120) == 0) {
                    PLOG_INFO(
                        "[PULSAR] [L.7] FlightInput LOCKOUT active "
                        "(remaining {}ms) - zeroed input"sv,
                        lockoutUntil - now);
                }
                return;
            }

            // Passive (client). If we have a recent host packet, overwrite the
            // engine's locally-written values so the flight controller
            // downstream sees host's input. If stale (network drop), let
            // engine's own values stand so client can pilot again.
            const bool armed = g_pendingInput.armed.load(std::memory_order_acquire);
            const std::uint64_t recvAt = g_pendingInput.receivedAtMs.load(std::memory_order_acquire);
            if (!armed || (recvAt > 0 && (now - recvAt) > kInputStaleMs)) {
                return;
            }
            Pulsar::Network::FlightInputData snap = g_pendingInput.data;
            SafeWriteInputBuf(a_handler, snap.axis0, snap.axis1, snap.axis2, snap.pilotByte);

            std::uint64_t cnt = g_inputDiagCount.fetch_add(1);
            if ((cnt % 60) == 0) {
                PLOG_INFO(
                    "[PULSAR] FlightInput apply: ax=({:.3f},{:.3f},{:.3f}) pByte=0x{:02X} ageMs={} handler=0x{:X}"sv,
                    snap.axis0, snap.axis1, snap.axis2, snap.pilotByte,
                    (now - recvAt),
                    reinterpret_cast<std::uintptr_t>(a_handler));
                NASLOG(
                    "FlightInput apply: ax=(%.3f,%.3f,%.3f) pByte=0x%02X ageMs=%llu handler=0x%llX",
                    snap.axis0, snap.axis1, snap.axis2, snap.pilotByte,
                    (unsigned long long)(now - recvAt),
                    (unsigned long long)reinterpret_cast<std::uintptr_t>(a_handler));
            }
        }
    }

    // -----------------------------------------------------------------------
    // V1 helpers below (kept for diagnostic-dump usage)
    // -----------------------------------------------------------------------

    // L.5 Phase 4 Step 15 - bracket the cruise toggle with pre/post mem dumps.
    static void __fastcall HookedCruiseToggle(bool force)
    {
        const char* labelPre  = force ? "PRE-TOGGLE force=true"  : "PRE-TOGGLE force=false";
        const char* labelPost = force ? "POST-TOGGLE force=true" : "POST-TOGGLE force=false";
        __try { LogCruiseMemDump(labelPre); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (g_origCruiseToggle) {
            __try { g_origCruiseToggle(force); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN("[PULSAR] L.5 Step 15: original cruise toggle SEH"sv);
            }
        }

        __try { LogCruiseMemDump(labelPost); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Are we the pilot? Based on player IsInSpace + GetSpaceship validity.
    static bool IsPilot()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        bool inSpace = false;
        bool hasShip = false;
        __try {
            inSpace = player->IsInSpace(true);
            hasShip = (player->GetSpaceship(true) != nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return inSpace && hasShip;
    }

}  // namespace

// L.5 public read-flag for Main.cpp's teleport gate. Defined in
// the named namespace because the header declares it here; the underlying
// atomic lives in the anonymous namespace above and is readable from the
// same TU.
bool IsPostLoadUnseatInProgress()
{
    return g_unseatInProgress.load(std::memory_order_acquire);
}

void ResetBodyCache()
{
    // A save-bridge load destroys every Havok body. The cached ship body,
    // the vt[8] apply body and the local NPC-ship bodies would otherwise
    // keep pointing at freed memory until something re-acquired them, and
    // the tick body-commit writes a transform through them. If the memory
    // is reused by a body of the same controller type the vtable guard in
    // SafeCommitHavokTransform does not catch it. Drop them; the sticky
    // matcher and the vt[8] hook re-acquire fresh ones after the load.
    g_playerBhkBody.store(nullptr, std::memory_order_release);
    g_psMatchBestBody.store(nullptr, std::memory_order_release);
    g_psMatchBestD2.store(3.4e38f, std::memory_order_release);
    g_psStickyHeld.store(false, std::memory_order_release);
    g_psStickyMiss.store(0, std::memory_order_release);
    g_iter13CachedApplyBody.store(nullptr, std::memory_order_release);
    g_iter13CachedApplyBodyMs.store(0, std::memory_order_release);
    g_pendingShipTransform.armed.store(false, std::memory_order_release);
    {
        std::lock_guard lk(g_npcLocalBodiesMtx);
        for (auto& b : g_npcLocalBodies) b = NpcLocalBody{};
    }
    PLOG_INFO("[PULSAR] FlightSync: body cache reset for save-bridge load"sv);
}

void Init()
{
    // Hook the ShipHudDataModel constructor.
    void* fn = reinterpret_cast<void*>(
        REL::Offset{ kRVA_ShipHudDataModelCtor }.address());
    MH_STATUS st = MH_CreateHook(
        fn,
        reinterpret_cast<LPVOID>(&HookedCtor),
        reinterpret_cast<LPVOID*>(&g_origCtor));
    if (st != MH_OK) {
        PLOG_WARN("[PULSAR] FlightSync: MH_CreateHook (ShipHudDataModel ctor) failed: {}"sv,
                  static_cast<int>(st));
        return;
    }
    st = MH_EnableHook(fn);
    if (st != MH_OK) {
        PLOG_WARN("[PULSAR] FlightSync: MH_EnableHook failed: {}"sv,
                  static_cast<int>(st));
        return;
    }
    PLOG_INFO(
        "[PULSAR] FlightSync L.3 V2 V1 init - ctor hook @ 0x{:X}"sv,
        reinterpret_cast<std::uintptr_t>(fn));
    NASLOG("FlightSync: init, ctor hook installed");

    // V4 Fire-Probe - resolve + log candidate vtables at init so we have
    // ground truth even if no fire event ever fires.
    {
        const std::uintptr_t vtA = ResolveFireVt(g_fireVt_OnPlayer, 413845);
        const std::uintptr_t vtB = ResolveFireVt(g_fireVt_Generic , 412442);
        const std::uintptr_t vtC = ResolveFireVt(g_fireVt_ProjBeg , 454462);
        PLOG_INFO(
            "[PULSAR] FireProbe init - vtA(OnPlayerFire 413845)=0x{:X} "
            "vtB(WeaponFired 412442)=0x{:X} vtC(ProjBeginUpdate 454462)=0x{:X}"sv,
            vtA, vtB, vtC);
    }

    // L.5 Phase 2 - log the cruise-active byte address at init and arm the
    // polling. No hook installed; poll runs from Tick.
    {
        std::uintptr_t ptr = REL::Offset{ kRVA_CruiseActiveByte }.address();
        g_cruiseActiveBytePtr.store(ptr, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] L.5 CruisePoll armed - byte @ 0x{:X}"sv, ptr);
    }

    // L.5 Phase 4 Step 15 - hook cruise toggle FUN_1413FDC90 to bracket it
    // with memory dumps. Captures pre+post snapshots on both host (force=
    // false natural pipeline) and client (force=true forced pipeline). Diff
    // reveals exactly what the engine writes during enter that force=true
    // skips on client → next sync targets.
    {
        void* fnTog = reinterpret_cast<void*>(
            REL::Offset{ kRVA_CruiseEnter }.address());
        MH_STATUS stTog = MH_CreateHook(
            fnTog,
            reinterpret_cast<LPVOID>(&HookedCruiseToggle),
            reinterpret_cast<LPVOID*>(&g_origCruiseToggle));
        if (stTog != MH_OK) {
            PLOG_WARN(
                "[PULSAR] L.5 Step 15: MH_CreateHook (cruise toggle) failed: {}"sv,
                static_cast<int>(stTog));
        } else {
            stTog = MH_EnableHook(fnTog);
            if (stTog != MH_OK) {
                PLOG_WARN(
                    "[PULSAR] L.5 Step 15: MH_EnableHook (cruise toggle) failed: {}"sv,
                    static_cast<int>(stTog));
            } else {
                PLOG_INFO(
                    "[PULSAR] L.5 Step 15: cruise-toggle bracket hook armed @ 0x{:X}"sv,
                    reinterpret_cast<std::uintptr_t>(fnTog));
            }
        }
    }

    // V2: hook the flight-input processor - vt[7] of PlayerFlightControls.
    // Verified to fire only on flight-mode attach (count=0 single-shot in
    // diagnostic test). Kept for diagnostic visibility.
    void* fn2 = reinterpret_cast<void*>(
        REL::Offset{ kRVA_FlightInputProcess }.address());
    MH_STATUS st2 = MH_CreateHook(
        fn2,
        reinterpret_cast<LPVOID>(&HookedFlightInputProcess),
        reinterpret_cast<LPVOID*>(&g_origFlightInputProcess));
    if (st2 != MH_OK) {
        PLOG_WARN("[PULSAR] FlightSync V2: MH_CreateHook (FlightInputProcess) failed: {}"sv,
                  static_cast<int>(st2));
    } else {
        st2 = MH_EnableHook(fn2);
        if (st2 != MH_OK) {
            PLOG_WARN("[PULSAR] FlightSync V2: MH_EnableHook failed: {}"sv,
                      static_cast<int>(st2));
        } else {
            PLOG_INFO(
                "[PULSAR] FlightSync V2 init - flight-input hook @ 0x{:X}"sv,
                reinterpret_cast<std::uintptr_t>(fn2));
            NASLOG("FlightSync V2: input hook installed @ 0x%llX",
                   (unsigned long long)reinterpret_cast<std::uintptr_t>(fn2));
        }
    }

    // V2.1: hook the per-frame PMO fire site (FUN_142115260). Diagnostic
    // pass first - verify it actually fires per-frame during piloting.
    // Once confirmed, we'll add the broadcast/override logic.
    void* fn3 = reinterpret_cast<void*>(
        REL::Offset{ kRVA_PMOFire }.address());
    MH_STATUS st3 = MH_CreateHook(
        fn3,
        reinterpret_cast<LPVOID>(&HookedPMOFire),
        reinterpret_cast<LPVOID*>(&g_origPMOFire));
    if (st3 != MH_OK) {
        PLOG_WARN("[PULSAR] FlightSync V2.1: MH_CreateHook (PMOFire) failed: {}"sv,
                  static_cast<int>(st3));
    } else {
        st3 = MH_EnableHook(fn3);
        if (st3 != MH_OK) {
            PLOG_WARN("[PULSAR] FlightSync V2.1: MH_EnableHook (PMOFire) failed: {}"sv,
                      static_cast<int>(st3));
        } else {
            PLOG_INFO(
                "[PULSAR] FlightSync V2.1 init - PMO fire hook @ 0x{:X}"sv,
                reinterpret_cast<std::uintptr_t>(fn3));
            NASLOG("FlightSync V2.1: PMO fire hook installed @ 0x%llX",
                   (unsigned long long)reinterpret_cast<std::uintptr_t>(fn3));
        }
    }

    // V2.2: hook BSTEventSource::Notify dispatcher. Filter to PMO via
    // singleton match; capture/broadcast on host, overwrite-payload on
    // client. This is the per-frame state-sync choke point.
    void* fn4 = reinterpret_cast<void*>(
        REL::Offset{ kRVA_NotifyDispatch }.address());
    MH_STATUS st4 = MH_CreateHook(
        fn4,
        reinterpret_cast<LPVOID>(&HookedNotifyDispatch),
        reinterpret_cast<LPVOID*>(&g_origNotifyDispatch));
    if (st4 != MH_OK) {
        PLOG_WARN("[PULSAR] FlightSync V2.2: MH_CreateHook (NotifyDispatch) failed: {}"sv,
                  static_cast<int>(st4));
    } else {
        st4 = MH_EnableHook(fn4);
        if (st4 != MH_OK) {
            PLOG_WARN("[PULSAR] FlightSync V2.2: MH_EnableHook (NotifyDispatch) failed: {}"sv,
                      static_cast<int>(st4));
        } else {
            const std::uintptr_t pmoSrc = ResolvePMOEventSourceVA();
            PLOG_INFO(
                "[PULSAR] FlightSync V2.2 init - Notify hook @ 0x{:X}, PMO source @ 0x{:X}"sv,
                reinterpret_cast<std::uintptr_t>(fn4), pmoSrc);
            NASLOG("FlightSync V2.2: Notify hook installed @ 0x%llX, PMO source @ 0x%llX",
                   (unsigned long long)reinterpret_cast<std::uintptr_t>(fn4),
                   (unsigned long long)pmoSrc);
        }
    }

    // V3 iter5: hook bhkCharacterStateSpaceship vt[8] (havok→cache writer).
    // Override rotation cache after engine writes it; downstream physics-to-
    // NiNode sync then propagates our values.
    {
        void* fn5 = reinterpret_cast<void*>(
            REL::Offset{ kRVA_BhkSpaceshipVt8 }.address());
        MH_STATUS st5 = MH_CreateHook(
            fn5,
            reinterpret_cast<LPVOID>(&HookedBhkSpaceshipVt8),
            reinterpret_cast<LPVOID*>(&g_origBhkSpaceshipVt8));
        if (st5 != MH_OK) {
            PLOG_WARN("[PULSAR] FlightSync V3 iter5: MH_CreateHook (BhkSpaceshipVt8) failed: {}"sv,
                      static_cast<int>(st5));
        } else {
            st5 = MH_EnableHook(fn5);
            if (st5 != MH_OK) {
                PLOG_WARN("[PULSAR] FlightSync V3 iter5: MH_EnableHook (BhkSpaceshipVt8) failed: {}"sv,
                          static_cast<int>(st5));
            } else {
                PLOG_INFO(
                    "[PULSAR] FlightSync V3 iter5 init - bhkSpaceship vt[8] hook @ 0x{:X}"sv,
                    reinterpret_cast<std::uintptr_t>(fn5));
                NASLOG("FlightSync V3 iter5: bhkSpaceship vt[8] hook installed @ 0x%llX",
                       (unsigned long long)reinterpret_cast<std::uintptr_t>(fn5));
            }
        }
    }
}

void Shutdown()
{
    g_modelPtr.store(0, std::memory_order_release);
}

// M.1 V2 iter2 - host-side: drain NPC ship slots → broadcast batch (1Hz).
static void NpcBatchTickSend(std::uint64_t a_now)
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) return;
    const std::uint64_t lastSend = g_lastNpcBatchSendMs.load(std::memory_order_acquire);
    if ((a_now - lastSend) < kNpcBatchIntervalMs) return;
    g_lastNpcBatchSendMs.store(a_now, std::memory_order_release);

    Pulsar::Network::NPCShipBatch batch{};
    batch.count = 0;
    {
        std::lock_guard<std::mutex> lock(g_npcShipSlotsMtx);
        for (int i = 0; i < kNpcShipSlotCount && batch.count < Pulsar::Network::MAX_NPC_SHIPS_PER_BATCH; ++i) {
            const auto& s = g_npcShipSlots[i];
            if (!s.body) continue;
            if ((a_now - s.lastUpdateMs) > kNpcSlotStaleMs) continue;
            auto& e = batch.entries[batch.count];
            e.handleHash = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(s.body));
            e.pad0       = 0;
            std::memcpy(e.havokState, s.lastState, sizeof(e.havokState));
            ++batch.count;
        }
    }
    if (batch.count == 0) return;

    // Use PacketWriter so receiver's PacketReader parses correctly (3-byte
    // PacketHeader + payload). Sending hand-rolled bytes broke the 3-byte
    // header expectation and IsValid()/Read() ran off-by-2.
    std::uint8_t buf[Pulsar::Network::HEADER_SIZE + sizeof(Pulsar::Network::NPCShipBatch)];
    Pulsar::Network::PacketWriter w(
        Pulsar::Network::PKT_NPC_SHIP_BATCH, buf, sizeof(buf));
    if (!w.Write(batch)) return;
    net.Send(w.Data(), w.Size(), false);  // unreliable

    static std::atomic<std::uint64_t> lastLogMs{ 0 };
    std::uint64_t lastLog = lastLogMs.load(std::memory_order_acquire);
    if ((a_now - lastLog) >= 5000) {
        lastLogMs.store(a_now, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] M.1 V2 NPC batch SEND: count={} firstPos=({:.0f},{:.0f},{:.0f})"sv,
            batch.count,
            batch.entries[0].havokState[12],
            batch.entries[0].havokState[13],
            batch.entries[0].havokState[14]);
    }
}

// M.1 V2 iter2 - client-side: position-match pending batch → apply.
static void NpcBatchTickApply(std::uint64_t a_now)
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || net.IsHost()) return;

    // Don't apply when client isn't in space - avoids matching stale local
    // body entries from a previous space session against fresh remote data.
    if (!g_psPosValid.load(std::memory_order_acquire)) return;

    const std::uint64_t batchAt = g_pendingNpcBatchMs.load(std::memory_order_acquire);
    if (batchAt == 0 || (a_now - batchAt) > kPendingNpcBatchStaleMs) return;

    // M.1 V2 iter2d - match-and-cache approach.
    // Match phase (here, throttled): for each remote entry, find best local
    // body and store state in slot's pendingApply. Apply phase happens 60Hz
    // in the vt[8] hook so engine physics can't drift bodies between matches.
    //
    // Throttle matching to ~200ms. Local body positions update via vt[8] but
    // we re-match periodically so the mapping tracks fast-moving ships.
    static std::atomic<std::uint64_t> lastMatchMs{ 0 };
    const std::uint64_t lastMatch = lastMatchMs.load(std::memory_order_acquire);
    if ((a_now - lastMatch) < 100) return;  // 10Hz match rate
    lastMatchMs.store(a_now, std::memory_order_release);

    std::vector<PendingNpcEntry> entries;
    {
        std::lock_guard<std::mutex> lock(g_pendingNpcBatchMtx);
        if (g_pendingNpcBatch.empty()) return;
        entries = g_pendingNpcBatch;  // copy, don't clear - vt[8] re-applies cached state
    }

    void* psBody = g_playerBhkBody.load(std::memory_order_acquire);

    // For each remote entry: match to local body, store pendingState in slot.
    int matched = 0;
    int missed  = 0;
    bool localUsed[kNpcShipSlotCount] = { false };
    {
        std::lock_guard<std::mutex> lock(g_npcLocalBodiesMtx);
        for (const auto& re : entries) {
            const float rx = re.havokState[12];
            const float ry = re.havokState[13];
            const float rz = re.havokState[14];
            int   bestIdx = -1;
            float bestD2  = kNpcMatchRadiusSq;
            for (int i = 0; i < kNpcShipSlotCount; ++i) {
                if (localUsed[i]) continue;
                if (!g_npcLocalBodies[i].body) continue;
                if (g_npcLocalBodies[i].body == psBody) continue;
                if ((a_now - g_npcLocalBodies[i].seenMs) > 5000) continue;
                const float dx = g_npcLocalBodies[i].pos[0] - rx;
                const float dy = g_npcLocalBodies[i].pos[1] - ry;
                const float dz = g_npcLocalBodies[i].pos[2] - rz;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; bestIdx = i; }
            }
            if (bestIdx >= 0) {
                auto& slot = g_npcLocalBodies[bestIdx];
                // M.1 V2 iter2g - promote current target to prev BEFORE
                // overwriting (snapshot history for interpolation).
                if (slot.hasPendingApply) {
                    std::memcpy(slot.prevState, slot.pendingState,
                                sizeof(slot.prevState));
                    slot.prevMs  = slot.pendingMs;
                    slot.hasPrev = true;
                }
                std::memcpy(slot.pendingState, re.havokState,
                            sizeof(re.havokState));
                slot.pendingMs       = a_now;
                slot.hasPendingApply = true;
                localUsed[bestIdx] = true;
                ++matched;
            } else {
                ++missed;
            }
        }

        // Un-stick: any local that wasn't matched this pass - clear its
        // pending apply. Otherwise we'd keep writing stale remote-state to
        // it from a previous match, fighting engine physics → jitter.
        for (int i = 0; i < kNpcShipSlotCount; ++i) {
            if (!localUsed[i] && g_npcLocalBodies[i].body
                && g_npcLocalBodies[i].hasPendingApply) {
                g_npcLocalBodies[i].hasPendingApply = false;
                g_npcLocalBodies[i].hasPrev         = false;
            }
        }
    }

    static std::atomic<std::uint64_t> lastLogMs{ 0 };
    std::uint64_t lastLog = lastLogMs.load(std::memory_order_acquire);
    if ((a_now - lastLog) >= 2000) {
        lastLogMs.store(a_now, std::memory_order_release);

        // Snapshot a few local bodies + first remote for diagnosis.
        std::uint32_t localCount = 0;
        float          loc0[3]   = { 0, 0, 0 };
        float          loc1[3]   = { 0, 0, 0 };
        float          loc2[3]   = { 0, 0, 0 };
        bool           haveLoc0 = false, haveLoc1 = false, haveLoc2 = false;
        {
            std::lock_guard<std::mutex> lock(g_npcLocalBodiesMtx);
            for (int i = 0; i < kNpcShipSlotCount; ++i) {
                if (!g_npcLocalBodies[i].body) continue;
                if (g_npcLocalBodies[i].body == g_playerBhkBody.load(std::memory_order_acquire)) continue;
                if ((a_now - g_npcLocalBodies[i].seenMs) > 5000) continue;
                ++localCount;
                if (!haveLoc0) {
                    haveLoc0 = true;
                    loc0[0] = g_npcLocalBodies[i].pos[0];
                    loc0[1] = g_npcLocalBodies[i].pos[1];
                    loc0[2] = g_npcLocalBodies[i].pos[2];
                } else if (!haveLoc1) {
                    haveLoc1 = true;
                    loc1[0] = g_npcLocalBodies[i].pos[0];
                    loc1[1] = g_npcLocalBodies[i].pos[1];
                    loc1[2] = g_npcLocalBodies[i].pos[2];
                } else if (!haveLoc2) {
                    haveLoc2 = true;
                    loc2[0] = g_npcLocalBodies[i].pos[0];
                    loc2[1] = g_npcLocalBodies[i].pos[1];
                    loc2[2] = g_npcLocalBodies[i].pos[2];
                }
            }
        }

        PLOG_INFO(
            "[PULSAR] M.1 V2 NPC batch APPLY: total={} matched={} missed={} localBodies={} "
            "remote0=({:.0f},{:.0f},{:.0f}) local0=({:.0f},{:.0f},{:.0f}) "
            "local1=({:.0f},{:.0f},{:.0f}) local2=({:.0f},{:.0f},{:.0f})"sv,
            entries.size(), matched, missed, localCount,
            entries.empty() ? 0.0f : entries[0].havokState[12],
            entries.empty() ? 0.0f : entries[0].havokState[13],
            entries.empty() ? 0.0f : entries[0].havokState[14],
            loc0[0], loc0[1], loc0[2],
            loc1[0], loc1[1], loc1[2],
            loc2[0], loc2[1], loc2[2]);
    }
}

void Tick()
{
    auto& netForV3 = Pulsar::Network::GetSingleton();

    // L.4 V1.1 - visual notification removed (see globals comment).
    {
        const std::uint64_t nowMs = ::GetTickCount64();
        NpcBatchTickSend(nowMs);
        NpcBatchTickApply(nowMs);
        V4PilotDiagTick(nowMs);

        // L.5 - poll cruise-active global byte for transitions, apply any
        // queued remote cruise transition. Phase 3: trigger post-EXIT
        // SaveBuffer-resync on host once the delay window elapses.
        // D3: 1Hz periodic snapshot of cruise-state globals during cruise.
        PollCruiseActiveByte();
        PollCruiseArrivalByte();  // Phase 4 Step 6 - DAT_145EB2856 arrival-init detection
        ApplyPendingCruise();
        TickPostCruiseResync();
        TickPostLoadUnseat();
        TickEarlyReleaseLockout();  // L.7 release lockout when user unseats
        TickCoPilotInvisibility();  // L.7 hide overlapping clone in pilot chair
        TickCruiseOrient();       // Phase 4 Step 11 - continuous orient sync @5Hz
        TickMarkerNotify();       // Phase 4 Step 12 - force client marker re-eval mid-cruise
        TickCruiseSnapshot();

        // L.3 V4 - when local is detPilot but vt[8] silent (engine not
        // physics-stepping the body, observed on client even when seated),
        // force g_isActivePilot from Tick so MaybeSyncShipTransform
        // (called from PMO Notify, which DOES fire on client) broadcasts.
        // ALWAYS sync the active flag to detPilot - without this, after
        // standing up vt[8] also stops firing on host so g_isActivePilot
        // stays at "1" stuck. Explicit reset is mandatory.
        if (netForV3.IsConnected()) {
            const bool detPilot = IsLocalDeterministicPilot();
            g_isActivePilot.store(detPilot, std::memory_order_release);
            if (detPilot) {
                void* body = g_iter13CachedApplyBody.load(std::memory_order_acquire);
                if (!body) body = g_playerBhkBody.load(std::memory_order_acquire);
                if (body) {
                    float st[16];
                    if (SafeReadHavokState(body, st)) {
                        PublishOutgoingHavokState(st);
                    }
                }
            }
        }
    }

    // V3 - late-frame apply attempt. Notify-hook apply (post-physics-output)
    // gets overwritten by something running later in the frame. Tick runs in
    // the SFSE OnUpdate callback which may be later in the frame than Notify.
    // Multiple write points = whichever runs last wins.
    // Gate on !g_isActivePilot - applying received data on the active side
    // fights the player's own physics (user reported "fliegen war komisch").
    if (netForV3.IsConnected() && kV3ApplyEnabled
        && !g_isActivePilot.load(std::memory_order_acquire)) {
        const bool armed = g_pendingShipTransform.armed.load(std::memory_order_acquire);
        if (armed) {
            const std::uint64_t recvAtV3 = g_pendingShipTransform.receivedAtMs.load(std::memory_order_acquire);
            const std::uint64_t nowV3 = ::GetTickCount64();
            if (recvAtV3 != 0 && (nowV3 - recvAtV3) <= kV3StaleMs) {
                void* niAv = SafeGetShipNiAVObject();
                if (niAv) {
                    // Only apply if we are NOT the active pilot. Re-use the
                    // observed/wrote tracking from MaybeSyncShipTransform.
                    Pulsar::Network::ShipTransformData live{};
                    if (SafeReadShipTransform(niAv, live)) {
                        // Use a less-strict activity test here: only suppress
                        // apply if the live values strongly differ from what
                        // we last wrote (engine resetting). Otherwise apply.
                        const auto& d = g_pendingShipTransform.data;
                        SafeWriteShipTransform(niAv, d);

                        std::uint64_t lastTickLog = g_lastV3LogMs.load(std::memory_order_acquire);
                        if ((nowV3 - lastTickLog) >= 1000) {
                            g_lastV3LogMs.store(nowV3, std::memory_order_release);
                            PLOG_INFO(
                                "[PULSAR] V3 tick-apply: pre=({:.3f},{:.3f},{:.3f}) "
                                "wrote=({:.3f},{:.3f},{:.3f})"sv,
                                live.rotate[0], live.rotate[1], live.rotate[2],
                                d.rotate[0], d.rotate[1], d.rotate[2]);
                        }
                    }
                }

                // V3 iter13 - also write havokState to cached body, in case
                // vt[8] is silent (engine stopped stepping the ship because
                // local player isn't seated). Only valid if we recently saw
                // the body via vt[8] passive applies.
                {
                void*               cachedBody    = g_iter13CachedApplyBody.load(std::memory_order_acquire);
                const std::uint64_t cachedBodyMs  = g_iter13CachedApplyBodyMs.load(std::memory_order_acquire);
                const bool cachedRecent = (cachedBody && cachedBodyMs != 0
                    && (nowV3 - cachedBodyMs) <= kIter13TickBodyMaxAgeMs);
                // when the client UNSEATS, vt[8] stops firing so cachedBody goes
                // stale (the exact "only the quest-marker moves" case). Fall back to the
                // position-matched ship body (g_playerBhkBody), which is vt[8]-independent
                // and survives unseat, so the body commit keeps running while walking.
                void* applyBody = cachedRecent
                    ? cachedBody
                    : g_playerBhkBody.load(std::memory_order_acquire);
                if (applyBody) {
                    const auto& d = g_pendingShipTransform.data;
                    bool allZero = true;
                    for (int i = 0; i < 16 && allZero; ++i) {
                        if (d.havokState[i] != 0.0f) allZero = false;
                    }
                    // L.5 Phase 3v8 - narrowed cruise filter (matches iter8).
                    bool cruiseSpike = false;
                    if (IsCruiseActive()) {
                        // Phase 4 Step 7 - relaxed (see vt[8] hook for rationale).
                        const float x = d.havokState[12];
                        const float y = d.havokState[13];
                        const float z = d.havokState[14];
                        cruiseSpike =
                            !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z);
                    }
                    if (!allZero && !cruiseSpike) {
                        // 244-fix: cache-only memcpy no longer carries a STANDING
                        // player on 244 (needs the hknp world-commit). Use the engine
                        // setter (writes cache AND commits to the live physics world) so
                        // the body physically rotates and carries the standing client.
                        const bool commitOn = Pulsar::GetConfig().coopSpaceBodyCommit;
                        if (commitOn) {
                            SafeCommitHavokTransform(applyBody, d.havokState);
                        } else {
                            SafeWriteHavokState(applyBody, d.havokState);
                        }
                        const std::uint64_t cnt =
                            g_iter13TickApplyCount.fetch_add(1) + 1;
                        const std::uint64_t lastLog =
                            g_iter13LastTickApplyLogMs.load(std::memory_order_acquire);
                        if ((nowV3 - lastLog) >= 1000) {
                            g_iter13LastTickApplyLogMs.store(nowV3, std::memory_order_release);
                            PLOG_INFO(
                                "[PULSAR] V3 iter13 tick-body-apply: cnt={} "
                                "body=0x{:X} src={} commit={} ageMs={} "
                                "row3=({:.2f},{:.2f},{:.2f})"sv,
                                cnt, reinterpret_cast<std::uintptr_t>(applyBody),
                                cachedRecent ? "vt8" : "sticky",
                                commitOn ? 1 : 0, (nowV3 - recvAtV3),
                                d.havokState[12], d.havokState[13], d.havokState[14]);
                        }
                    }
                }
                }  // close: not IsCruiseActive
            }
        }
    }

    auto modelPtr = g_modelPtr.load(std::memory_order_acquire);
    if (!modelPtr) return;

    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected()) return;

    const std::uint64_t now = ::GetTickCount64();

    // --- Client side: apply pending received state (V1 disabled) -----------
    if (kV1ApplyEnabled && !net.IsHost() && g_pending.armed.load(std::memory_order_acquire)) {
        Pulsar::Network::FlightStateData snap = g_pending.data;
        g_pending.armed.store(false, std::memory_order_release);
        ApplySnapshot(modelPtr, snap);

        // Diagnostic: log a sample so we see what's flowing.
        std::uint64_t cnt = g_diagSampleCount.fetch_add(1);
        if ((cnt % 60) == 0) {  // every ~3s at 20Hz
            PLOG_INFO(
                "[PULSAR] FlightSync apply: thr={:.2f} adjThr={:.2f} spd={:.2f} "
                "boost={:.2f} crossX={:.3f} crossY={:.3f} boostAct={} reverse={} "
                "cruise={} model=0x{:X}"sv,
                snap.throttle, snap.adjustedThrottle, snap.speed, snap.boost,
                snap.dirCrosshairX, snap.dirCrosshairY,
                snap.boostActive, snap.bReverse, snap.uiCruiseLevel,
                modelPtr);
            NASLOG(
                "FlightSync apply: thr=%.2f adjThr=%.2f spd=%.2f boost=%.2f "
                "crossX=%.3f crossY=%.3f boostAct=%u reverse=%u cruise=%u model=0x%llX",
                snap.throttle, snap.adjustedThrottle, snap.speed, snap.boost,
                snap.dirCrosshairX, snap.dirCrosshairY,
                snap.boostActive, snap.bReverse, snap.uiCruiseLevel,
                (unsigned long long)modelPtr);
        }
    }

    // V1.0.1 diagnostic: full dump every 500ms (regardless of host/client) so
    // we can see which entries actually move during steering. Drops out once
    // we ID the right yaw/pitch entries.
    {
        const std::uint64_t lastDump = g_lastDumpMs.load(std::memory_order_acquire);
        if ((now - lastDump) >= 500) {
            g_lastDumpMs.store(now, std::memory_order_release);
            DumpAllEntries(modelPtr);
        }
    }

    // --- Host side: rate-limited send while piloting (V1 disabled) ---------
    if (!kV1SendEnabled) return;
    if (!net.IsHost()) return;
    if (!IsPilot()) return;

    const std::uint64_t last = g_lastSendMs.load(std::memory_order_acquire);
    if ((now - last) < kSendIntervalMs) return;
    g_lastSendMs.store(now, std::memory_order_release);

    Pulsar::Network::FlightStateData snap{};
    if (!ReadSnapshot(modelPtr, snap)) return;

    std::uint8_t buf[64]{};
    Pulsar::Network::PacketWriter w(
        Pulsar::Network::PKT_FLIGHT_STATE, buf, sizeof(buf));
    if (!w.Write(snap)) return;
    net.Send(w.Data(), w.Size(), false);  // unreliable - frequent, lossy ok

    std::uint64_t cnt = g_diagSampleCount.fetch_add(1);
    if ((cnt % 60) == 0) {
        PLOG_INFO(
            "[PULSAR] FlightSync send: thr={:.2f} adjThr={:.2f} spd={:.2f} "
            "boost={:.2f} crossX={:.3f} crossY={:.3f} boostAct={} reverse={} "
            "cruise={} model=0x{:X}"sv,
            snap.throttle, snap.adjustedThrottle, snap.speed, snap.boost,
            snap.dirCrosshairX, snap.dirCrosshairY,
            snap.boostActive, snap.bReverse, snap.uiCruiseLevel,
            modelPtr);
        NASLOG(
            "FlightSync send: thr=%.2f adjThr=%.2f spd=%.2f boost=%.2f "
            "crossX=%.3f crossY=%.3f boostAct=%u reverse=%u cruise=%u",
            snap.throttle, snap.adjustedThrottle, snap.speed, snap.boost,
            snap.dirCrosshairX, snap.dirCrosshairY,
            snap.boostActive, snap.bReverse, snap.uiCruiseLevel);
    }
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    Pulsar::Network::PacketReader r(data, length);
    if (!r.IsValid()) return;

    if (r.Type() == Pulsar::Network::PKT_FLIGHT_STATE) {
        // V1 - receiver-side stage (no longer applied; kept for back-compat).
        Pulsar::Network::FlightStateData snap{};
        if (!r.Read(snap)) return;
        g_pending.data         = snap;
        g_pending.receivedAtMs = ::GetTickCount64();
        g_pending.armed.store(true, std::memory_order_release);
        return;
    }

    if (r.Type() == Pulsar::Network::PKT_FLIGHT_INPUT) {
        // V2 - store host's flight input axes; the input hook on the game
        // thread reads this and overrides handler+0x40 each frame.
        Pulsar::Network::FlightInputData payload{};
        if (!r.Read(payload)) return;
        g_pendingInput.data = payload;
        g_pendingInput.receivedAtMs.store(::GetTickCount64(), std::memory_order_release);
        g_pendingInput.armed.store(true, std::memory_order_release);
        return;
    }

    if (r.Type() == Pulsar::Network::PKT_PMO_PAYLOAD) {
        // V2.2 - full PMO event payload (96 bytes). The Notify hook on
        // the game thread reads this and overwrites the engine's
        // about-to-be-fired payload before original Notify dispatches.
        Pulsar::Network::PMOPayloadData payload{};
        if (!r.Read(payload)) return;
        g_pendingPMOPayload.data = payload;
        g_pendingPMOPayload.receivedAtMs.store(::GetTickCount64(), std::memory_order_release);
        g_pendingPMOPayload.armed.store(true, std::memory_order_release);
        return;
    }

    if (r.Type() == Pulsar::Network::PKT_SHIP_TRANSFORM) {
        // V3 - ship NiNode world transform (48 bytes). Apply runs in the
        // PMO Notify hook on game thread; this just stages.
        Pulsar::Network::ShipTransformData payload{};
        if (!r.Read(payload)) return;
        g_pendingShipTransform.data = payload;
        g_pendingShipTransform.receivedAtMs.store(::GetTickCount64(), std::memory_order_release);
        g_pendingShipTransform.armed.store(true, std::memory_order_release);
        // L.3 V5 - cache shipFormID so SafeGetShipNiAVObject can fall back
        // to a FormID lookup when our local GetSpaceship returns null.
        if (payload.shipFormID != 0) {
            g_lastReceivedShipFormID.store(payload.shipFormID,
                                           std::memory_order_release);
        }
        return;
    }

    if (r.Type() == Pulsar::Network::PKT_SHIP_FIRE) {
        // L.4 V1 - wire validation. Visual stub deferred to V2.
        Pulsar::Network::ShipFireData payload{};
        if (!r.Read(payload)) return;
        const std::uint64_t now = ::GetTickCount64();
        PLOG_INFO(
            "[PULSAR] L.4 fire RECV: shooter=0x{:08X} weapon=0x{:08X} "
            "baseProj=0x{:08X} ts={} ageMs={}"sv,
            payload.shooterShipFormId, payload.weaponFormId,
            payload.baseProjFormId, payload.timestampMs,
            (now > payload.timestampMs) ? (now - payload.timestampMs) : 0);
        // slice 3 - hand off to ShipCombatSync to spawn the visible bolt
        // on the game thread (queues; drained in ShipCombatSync::Tick).
        Pulsar::ShipCombatSync::OnRemoteFire(payload);
        return;
    }

    if (r.Type() == Pulsar::Network::PKT_CRUISE_STATE) {
        // L.5 Phase 2 - host says cruise toggled. Stage for game-thread apply.
        Pulsar::Network::CruiseStateData payload{};
        if (!r.Read(payload)) return;
        // Phase 4 Step 6 - arrival-init packets are a separate trigger; they
        // arrive WHILE cruise is active (enable=1) so we must NOT re-arm the
        // ENTER/EXIT path (would call ENTER fn again and double-allocate sinks).
        if (payload.arrivalInit) {
            g_pendingCruise.arrivalState.store(payload.arrivalState, std::memory_order_release);
            g_pendingCruise.uiSelector.store(payload.uiSelector, std::memory_order_release);
            // Phase 4 Step 7b - orient globals at arrival moment for cell-stream task.
            std::uint64_t bitsX, bitsY, bitsZ;
            std::memcpy(&bitsX, &payload.shipPosX, sizeof(bitsX));
            std::memcpy(&bitsY, &payload.shipPosY, sizeof(bitsY));
            std::memcpy(&bitsZ, &payload.shipPosZ, sizeof(bitsZ));
            g_pendingCruise.shipPosX.store(bitsX, std::memory_order_release);
            g_pendingCruise.shipPosY.store(bitsY, std::memory_order_release);
            g_pendingCruise.shipPosZ.store(bitsZ, std::memory_order_release);
            g_pendingCruise.armedArrivalInit.store(true, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] L.5 Phase 4 Step 6/7/7b RECV arrivalInit=1 ship=0x{:08X} state=0x{:08X} uiSel={} pos=({:.0f},{:.0f},{:.0f}) ts={}"sv,
                payload.shipFormId, payload.arrivalState,
                static_cast<int>(payload.uiSelector),
                payload.shipPosX, payload.shipPosY, payload.shipPosZ,
                payload.timestampMs);
            return;
        }
        g_pendingCruise.enable.store(payload.enable, std::memory_order_release);
        g_pendingCruise.receivedAtMs.store(::GetTickCount64(), std::memory_order_release);
        // Phase 3v3 - stage host's world position (only meaningful for EXIT;
        // ENTER carries 0s). Stored as raw double bits in atomic uint64.
        std::uint64_t bitsX, bitsY, bitsZ;
        std::memcpy(&bitsX, &payload.shipPosX, sizeof(bitsX));
        std::memcpy(&bitsY, &payload.shipPosY, sizeof(bitsY));
        std::memcpy(&bitsZ, &payload.shipPosZ, sizeof(bitsZ));
        g_pendingCruise.shipPosX.store(bitsX, std::memory_order_release);
        g_pendingCruise.shipPosY.store(bitsY, std::memory_order_release);
        g_pendingCruise.shipPosZ.store(bitsZ, std::memory_order_release);
        // Phase 4 Step 4 - stage destination formID for ApplyPendingCruise.
        g_pendingCruise.destFormId.store(payload.destFormId, std::memory_order_release);
        g_pendingCruise.armed.store(true, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] L.5 CruiseState RECV: enable={} ship=0x{:08X} dest=0x{:08X} ts={} pos=({:.0f},{:.0f},{:.0f})"sv,
            static_cast<int>(payload.enable), payload.shipFormId,
            payload.destFormId, payload.timestampMs,
            payload.shipPosX, payload.shipPosY, payload.shipPosZ);
        return;
    }

    if (r.Type() == Pulsar::Network::PKT_CRUISE_ORIENT) {
        // Phase 4 Step 11 - continuous orient stream. Stage; TickCruiseOrient
        // applies on game thread when cruise byte is active on this side too.
        Pulsar::Network::CruiseOrientData payload{};
        if (!r.Read(payload)) return;
        if (!std::isfinite(payload.posX) || !std::isfinite(payload.posY) ||
            !std::isfinite(payload.posZ)) return;
        std::uint64_t bitsX, bitsY, bitsZ;
        std::memcpy(&bitsX, &payload.posX, sizeof(bitsX));
        std::memcpy(&bitsY, &payload.posY, sizeof(bitsY));
        std::memcpy(&bitsZ, &payload.posZ, sizeof(bitsZ));
        g_pendingCruiseOrient.bitsX.store(bitsX, std::memory_order_release);
        g_pendingCruiseOrient.bitsY.store(bitsY, std::memory_order_release);
        g_pendingCruiseOrient.bitsZ.store(bitsZ, std::memory_order_release);
        g_pendingCruiseOrient.receivedAtMs.store(::GetTickCount64(), std::memory_order_release);
        g_pendingCruiseOrient.armed.store(true, std::memory_order_release);
        return;
    }

    if (r.Type() == Pulsar::Network::PKT_NPC_SHIP_BATCH) {
        // M.1 V2 iter2 - queue for game-thread apply via NpcBatchTickApply.
        Pulsar::Network::NPCShipBatch batch{};
        if (!r.Read(batch)) return;
        const std::uint64_t now = ::GetTickCount64();
        if (batch.count == 0 || batch.count > Pulsar::Network::MAX_NPC_SHIPS_PER_BATCH) return;
        std::vector<PendingNpcEntry> entries;
        entries.reserve(batch.count);
        for (uint8_t i = 0; i < batch.count; ++i) {
            PendingNpcEntry e{};
            e.handleHash = batch.entries[i].handleHash;
            std::memcpy(e.havokState, batch.entries[i].havokState, sizeof(e.havokState));
            entries.push_back(e);
        }
        {
            std::lock_guard<std::mutex> lock(g_pendingNpcBatchMtx);
            g_pendingNpcBatch = std::move(entries);
        }
        g_pendingNpcBatchMs.store(now, std::memory_order_release);

        static std::atomic<std::uint64_t> lastLogMs{ 0 };
        const std::uint64_t lastLog = lastLogMs.load(std::memory_order_acquire);
        if ((now - lastLog) >= 5000) {
            lastLogMs.store(now, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] M.1 V2 NPC batch RECV: count={} firstHandle=0x{:08X} "
                "firstPos=({:.0f},{:.0f},{:.0f})"sv,
                batch.count, batch.entries[0].handleHash,
                batch.entries[0].havokState[12],
                batch.entries[0].havokState[13],
                batch.entries[0].havokState[14]);
        }
        return;
    }
}

}  // namespace Pulsar::FlightSync
