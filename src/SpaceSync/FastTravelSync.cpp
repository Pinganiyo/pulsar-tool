#include "PCH.h"
#include "SpaceSync/FastTravelSync.h"
#include "TransitSync/TransitSync.h"

#include "Cell3DAttach.h"
#include "NASLog.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "RemotePlayer/RemotePlayerManager.h"   // #docking - GetRemoteCellID for co-location guard
#include "SaveScanner/SaveScanner.h"
#include "WorldSync/StateProbe.h"
#include "WorldSync/WorldSync.h"

#include <RE/B/BSContainer.h>
#include <RE/E/Events.h>
#include <RE/N/NiSmartPointer.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/U/UI.h>

#include <REL/Relocation.h>

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// L.7 V8: fast-travel save-bridge re-enabled. The original v6 disabled it
// because the bridge fired during the client's initial save-load window -
// race-conditioned by guessing timers. V8 fixes the root cause with
// PKT_CLIENT_LOAD_COMPLETE: client tells host explicitly when the previous
// save-load is fully done, and the bridge only fires after that ack. We
// also drop the pre-emptive 1-fire-per-session limit - single-player
// loads unlimited saves with no engine bug, so cross-system L.7 bridges
// should too. If empirical testing shows otherwise, we add the limit back.
#define PULSAR_L7_FT_BRIDGE_ENABLED 1

using namespace std::string_view_literals;

namespace Pulsar::SpaceSync::FastTravel {
namespace {

    std::atomic<std::uint64_t> g_lastGravJumpFireMs{ 0 };

    struct DeltaSnap {
        std::uint32_t cell       = 0;
        std::uint32_t cellWS     = 0;
        std::uint32_t shipCell   = 0;
        std::uint32_t shipFormID = 0;
        std::int32_t  posXi      = 0;
        std::int32_t  posYi      = 0;
        std::int32_t  posZi      = 0;
        bool          inSpace    = false;
        bool          hasShip    = false;
    };

    DeltaSnap g_lastHost{};
    DeltaSnap g_lastClient{};

    void TickDeltaProbe()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        DeltaSnap cur{};
        __try {
            if (player->parentCell) {
                cur.cell = player->parentCell->formID;
                void* ws = *reinterpret_cast<void**>(
                    reinterpret_cast<std::uint8_t*>(player->parentCell) + 0x118);
                if (ws) {
                    cur.cellWS = *reinterpret_cast<std::uint32_t*>(
                        reinterpret_cast<std::uint8_t*>(ws) + 0x28);
                }
                std::uint32_t flags = *reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uint8_t*>(player->parentCell) + 0x48);
                cur.inSpace = ((flags & 0x01) == 0);
            }
            cur.posXi = static_cast<std::int32_t>(player->data.location.x);
            cur.posYi = static_cast<std::int32_t>(player->data.location.y);
            cur.posZi = static_cast<std::int32_t>(player->data.location.z);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        __try {
            auto* ship = player->GetSpaceship(true);
            if (ship) {
                cur.hasShip    = true;
                cur.shipFormID = ship->formID;
                if (ship->parentCell) cur.shipCell = ship->parentCell->formID;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        auto& net = Pulsar::Network::GetSingleton();
        const bool isHost = net.IsConnected() && net.IsHost();
        DeltaSnap& last = isHost ? g_lastHost : g_lastClient;

        const std::int32_t bigPosDelta = 100;
        const bool changed =
            (cur.cell       != last.cell)       ||
            (cur.cellWS     != last.cellWS)     ||
            (cur.shipCell   != last.shipCell)   ||
            (cur.shipFormID != last.shipFormID) ||
            (cur.inSpace    != last.inSpace)    ||
            (cur.hasShip    != last.hasShip)    ||
            (std::abs(cur.posXi - last.posXi) > bigPosDelta) ||
            (std::abs(cur.posYi - last.posYi) > bigPosDelta) ||
            (std::abs(cur.posZi - last.posZi) > bigPosDelta);
        if (!changed) return;

        const char* role = isHost ? "HOST" : "CLIENT";
        PLOG_INFO(
            "[PULSAR] [L.7 FT-DELTA] {} cell=0x{:08X}→0x{:08X} ws=0x{:08X}→0x{:08X} "
            "inSpace={}→{} hasShip={}→{} shipFmt=0x{:08X}→0x{:08X} "
            "shipCell=0x{:08X}→0x{:08X} pos=({},{},{})→({},{},{})"sv,
            role,
            last.cell, cur.cell,
            last.cellWS, cur.cellWS,
            last.inSpace, cur.inSpace,
            last.hasShip, cur.hasShip,
            last.shipFormID, cur.shipFormID,
            last.shipCell, cur.shipCell,
            last.posXi, last.posYi, last.posZi,
            cur.posXi, cur.posYi, cur.posZi);
        NASLOG("L.7 FT-DELTA %s cell=0x%08X→0x%08X inSpace=%d→%d",
               role, last.cell, cur.cell,
               static_cast<int>(last.inSpace), static_cast<int>(cur.inSpace));

        last = cur;
    }

#if PULSAR_L7_FT_BRIDGE_ENABLED
    // L.7 V8 save-bridge state. The actual fire happens at the end of
    // TickHostPFTDeferredSend after the diagnostic PKT_FAST_TRAVEL_SYNC
    // packet, gated on the client having ack'd a prior save-load.
    std::atomic<bool>          g_clientLoadCompleted{ false };
    std::atomic<std::uint64_t> g_lastBridgeFireMs{ 0 };
    // Minimum spacing between bridge fires, just so a stale arrival flag
    // can't double-fire if PFT detection reruns mid-load.
    constexpr std::uint64_t    kBridgeCooldownMs = 15000;

    // L.7 1.4 - pending-bridge retry. When FireSaveBridge is called while
    // client hasn't ack'd a prior load (clientReady=false), the fire was
    // previously dropped on the floor. Discovered 2026-05-05: host PFT-
    // arrival fired ~18s before client's initial-connect LOAD_COMPLETE
    // arrived, so the post-landing save was never sent and client stayed
    // in space. Now we record the latest skipped fire and re-dispatch from
    // OnClientLoadCompletePacket once the ack lands.
    struct PendingBridge {
        std::string    reason;
        bool           triggerOwnSave;
        std::uint64_t  armedMs;
        bool           valid;
    };
    PendingBridge      g_pendingBridge{};
    std::mutex         g_pendingBridgeMutex;
    constexpr std::uint64_t kPendingBridgeMaxAgeMs = 60000;

    // L.7 V14.1 - deferred transition fire. When a TakeOff or Landing
    // SpaceshipSink event fires we arm a pending timer instead of
    // triggering the quicksave immediately - V14 immediate-fire crashed
    // host with DEP violation because the engine's mid-cutscene state
    // had transient (vtable-null) REFRs that the quicksave serializer
    // dereferenced. Wait until the engine has settled (5s after the
    // last sink fire) before triggering save.
    //
    // V15.3 - at-arm-time also record inSpace baseline. TickPendingTransition
    // re-checks inSpace at fire-time and only fires if it FLIPPED since
    // arm (real takeoff/landing). Same-state = spurious (door/POI).
    // V14.2's at-fire-time-only flip check was wrong because TakeOff
    // event fires BEFORE the engine transitions to space - at-fire
    // inSpace still matches pre-state, gets classified as spurious.
    std::atomic<std::uint64_t> g_pendingTransitionArmMs{ 0 };
    std::atomic<int>           g_pendingTransitionInSpaceAtArm{ -1 };
    static const char*         g_pendingTransitionReason = nullptr;
    // Minimum settle floor - even a fully stable engine waits this long after
    // arm before fire. Prevents firing during the very first frames of a
    // cinematic when the engine hasn't started transitioning yet.
    constexpr std::uint64_t    kTransitionSettleMs = 5000;

    // L.7 1.13 - engine-state stability checkpoint. Replaces the hard-coded
    // 15s wait with an event-driven gate: after the transition arms,
    // monitor (pcell, inSpace, shipCell) per tick. The engine is "settled"
    // once at least one of those changed since arm AND no change has been
    // observed for kTransitionStableMs.
    //
    // Why: the takeoff cinematic length varies - ~8-12s typically but can
    // be longer/shorter based on system performance. A fixed 15s settle
    // either fires too early (cutscene still running, save corrupts state)
    // or wastes time (cutscene done much earlier). Tying to actual state
    // stability gives the right amount of wait every time.
    //
    // The "must change since arm" condition catches spurious events
    // (engine fires TakeOff sink without transitioning anything, e.g.
    // host loaded save in cockpit). If state never changes in
    // kTransitionMaxWaitMs, give up - it was a false positive.
    // L.7 1.13.2 - increased stability window. With shipPos tracking the
    // monitor catches cutscene-driven motion, but the ship still drifts
    // for several seconds after inSpace flips. 8s gives the engine time
    // to fully settle ship pose at the destination before we serialize.
    constexpr std::uint64_t    kTransitionStableMs   = 8000;
    constexpr std::uint64_t    kTransitionMaxWaitMs  = 60000;
    struct EngineSnapshot {
        std::uint32_t  pcell    = 0;
        int            inSpace  = -1;
        std::uint32_t  shipCell = 0;
        // L.7 1.13.2 - track ship position rounded to nearest meter. The
        // landing cutscene moves the ship over ~20s after inSpace flips
        // 1→0; without shipPos tracking, the snapshot stabilizes too soon
        // and the bridge fires mid-cutscene (engine state corruption →
        // host crash, client too fast).
        std::int32_t   shipPosX = 0;
        std::int32_t   shipPosY = 0;
        std::int32_t   shipPosZ = 0;
        bool operator!=(const EngineSnapshot& o) const noexcept {
            return pcell != o.pcell || inSpace != o.inSpace ||
                   shipCell != o.shipCell ||
                   shipPosX != o.shipPosX ||
                   shipPosY != o.shipPosY ||
                   shipPosZ != o.shipPosZ;
        }
    };
    static EngineSnapshot           g_engineAtArm{};
    static EngineSnapshot           g_engineLast{};
    std::atomic<std::uint64_t>      g_engineLastChangeMs{ 0 };
    std::atomic<bool>               g_engineChangedSinceArm{ false };

    // L.7 1.14 - engine-autosave checkpoint. The engine writes a new .sfs
    // file when it considers a major transition complete (after grav-jump,
    // after landing cinematic, after far-travel arrival). Capture the
    // newest filename at arm-time; per-tick check if a NEWER one appeared.
    // When yes → engine just autosaved → that's our checkpoint.
    static std::string              g_armNewestSavePath;

    // L.7 1.5 - last time PFT arrival was detected (host targeted a planet
    // via star map). Used as a "real Landing" signal in TickPendingTransition:
    // when the player stays inside the cockpit during landing, IsInSpace
    // doesn't flip 1→0 (player's parent cell is the persistent ship interior
    // whose worldspace association lags). The V15.3 inSpace-flip filter then
    // misclassifies the legitimate Landing event as SPURIOUS. PFT-recent
    // gives us a positive signal: if we detected arrival at a planet within
    // the last 60s, the Landing event that follows IS real.
    std::atomic<std::uint64_t> g_lastPFTArmedMs{ 0 };
    constexpr std::uint64_t    kPFTRecentLandingMs = 60000;

    // L.7 1.7 - per-reason suppress window. Engine fires Landing/TakeOff
    // multiple times during a cinematic (each animation phase). After we
    // successfully fire a save-bridge for "Landing", suppress further
    // arms for "Landing" for kReasonSuppressMs to avoid double-loadscreens
    // on client. Cleared when a different reason arms (= state actually
    // moved on).
    std::atomic<std::uint64_t> g_lastFiredReasonMs{ 0 };
    static const char*         g_lastFiredReason = nullptr;
    constexpr std::uint64_t    kReasonSuppressMs = 60000;

    // L.7 1.9 - first-ack checkpoint (replaces 1.8 time-based suppression).
    //
    // Problem 1.8 solved with a 10-30s heuristic: when host loads a save
    // before/during a connect, the engine's cell-init fires Landing/RampDown
    // events that try to fire a save-bridge. That bridge gets queued (pending
    // because client hasn't ack'd yet), then re-fires on first ack - causing
    // a redundant second loadscreen on client because the initial-connect
    // save already covered the same state.
    //
    // 1.9 is event-driven instead of time-based: on the FIRST LOAD_COMPLETE
    // ack from a fresh connection, drop any pending bridge. By definition
    // anything queued before that first ack represents state already covered
    // by the initial-connect save transfer. After first ack, the pending-
    // retry mechanism works normally for subsequent transitions.
    //
    // Trade-off: a real TakeOff/Landing fired by the user during the initial
    // connect-load window (typically 10-30s) would also be dropped. Rare in
    // practice because the user is usually waiting for the load to finish
    // and not driving the ship.
    std::atomic<bool>          g_firstAckReceived{ false };
    std::atomic<std::uint64_t> g_firstAckMs{ 0 };
    // L.7 1.11 - TakeOff arms within this window of first ack are spurious
    // engine cell-init artifacts (observed empirically: TakeOff sink fires
    // ~14s after host loads a save-in-space, even though the host is just
    // sitting in the cockpit). Real user takeoffs typically happen later
    // (user reads UI, navigates menus, etc).
    constexpr std::uint64_t    kFirstAckTakeOffSuppressMs = 30000;
    // L.7 V8d - 1-fire-per-session limit. Empirically confirmed (logs from
    // 18:55+ and 19:05+): the engine handles the FIRST mid-session save-
    // load fine, but the SECOND puts the client in cell=0x00000000 / main-
    // menu fallback regardless of save type (quicksave or regular save).
    // Same root as L.6 v17 forward-bridge issue. Documented in
    // memory/reference_engine_2nd_save_load_limit.md. Until that engine
    // limit is RE'd and worked around, ship 1.0 with one-shot sync per
    // session - user reconnects to re-sync.
    std::atomic<bool>          g_bridgeFiredThisSession{ false };
#endif  // PULSAR_L7_FT_BRIDGE_ENABLED

    // =====================================================================
    // L.7 V7 - GravJump→RampDown sequence-driven MoveTo bridge
    // =====================================================================
    //
    // Trigger sequence per Star Map ship jump (verified from logs):
    //   Multiple GravJumpEvents fire during the cinematic → RampDownEvent
    //   fires when the ramp animation finishes at destination = arrival is
    //   complete and ship.parentCell is updated.
    //
    // FarTravelEvent fires for some Star-Map flows (planet-to-planet) but
    // NOT for cross-system GravJump in current testing - so we don't gate on
    // it. PlayerFastTravel::Event is for surface map markers, also unused.
    //
    // RampDown alone is ambiguous (fires for any landing); we filter out
    // plain landings by requiring that GravJump fired in the last 30s.
    //
    // Host: when RampDown fires after a recent GravJump, arm a 3s deferred
    // read+send. The delay lets ship.parentCell stabilize on the destination
    // cell. After the delay we read ship.parentCell + ship.position and
    // broadcast PKT_FAST_TRAVEL_SYNC.
    //
    // Client: packet handler stages the destination data; the next game-tick
    // drain calls engine MoveTo (RVA 0x14191EFB0) on the local same-FormID
    // ship REFR with a destination REFR resolved out of the destination cell.
    // Engine MoveTo is cell-streamer aware so the receiver's worldspace gets
    // streamed in - that's why this beats BeginHostQuicksaveTransfer (which
    // broke the engine 2nd-save-load limit).

    constexpr std::uintptr_t kRVA_EngineMoveTo       = 0x191EFB0;
    // L.7 V7c - engine GetCurrentPlanet Papyrus native, called directly to
    // get the player's currently-orbited planet REFR (0 = deep space).
    // Signature: void* __fastcall(VMArg1_unused, VMArg2_unused, REFR* self);
    // Returned ptr is a TESForm* with formType byte 0xBA at +0x2E (planet
    // REFR). RVA verified via Ghidra string xref pass on "GetCurrentPlanet".
    constexpr std::uintptr_t kRVA_GetCurrentPlanet   = 0x2083A20;

    // Forward decls (defined further down in the same anon namespace).
    static RE::TESObjectREFR* FindRefByBaseInCell(RE::TESObjectCELL* cell,
                                                   std::uint32_t baseFormID);
    static RE::TESObjectREFR* FindAnyRefInCell(RE::TESObjectCELL* cell,
                                                RE::TESObjectREFR* exclude);
    constexpr std::uint64_t  kPFTSettleDelayMs       = 3000;
    constexpr std::uint64_t  kPFTApplyStaleMs        = 60000;
    constexpr std::uint64_t  kPFTApplyCooldownMs     = 5000;
    // L.7 V7b - polling-based arrival detection (replaces fragile GravJump
    // →RampDown event sequence; RampDown does not fire for cross-system
    // jumps in current testing, and the engine's GravJumpEvent dispatch is
    // indirect through vtable so we couldn't find a clean send hook).
    //
    // Track the host's ship state every tick. When any of (parentCell formID,
    // cellWorldspace formID, integer position) changes, mark a "state churn"
    // timestamp. Once the state has been STABLE (no change) for the settle
    // window AND there was a recent churn, treat that as arrival → fire
    // PKT_FAST_TRAVEL_SYNC. After firing, hold a cooldown to avoid retrigger
    // on the GravJump-back-to-cruise micro-changes.
    constexpr std::uint64_t  kPFTPollIntervalMs      = 250;
    constexpr std::uint64_t  kPFTStableSettleMs      = 5000;
    constexpr std::uint64_t  kPFTSendCooldownMs      = 30000;
    constexpr std::int32_t   kPFTPosChangeThreshold  = 1000;  // 1km world units

    static std::atomic<std::uint64_t>         g_pftCompletedAtMs{ 0 };
    static std::atomic<std::uint64_t>         g_pftLastApplyMs{ 0 };
    static std::atomic<std::uint64_t>         g_pftLastPollMs{ 0 };
    static std::atomic<std::uint64_t>         g_pftLastChurnMs{ 0 };
    static std::atomic<std::uint64_t>         g_pftLastSendMs{ 0 };
    // L.7 V7c - track the currently-orbited planet's FormID. Changes when
    // host completes a fast-travel jump. (0 = deep space, no planet.)
    static std::atomic<std::uint32_t>         g_pftPrevPlanetFormID{ 0 };
    static std::atomic<bool>                  g_pftHaveBaseline{ false };
    // L.7 1.12 - last planet we actually armed a bridge for. Same-planet
    // POI travel sees planet churn 0x5E30E → 0 → 0x5E30E (host briefly
    // leaves orbit, returns to same planet's surface). Without this, the
    // post-settle PFT-arrival fires another bridge though the planet ID
    // is the same as last bridge - = redundant client load. Updated only
    // when an arm actually fires, so first arrival at a new planet still
    // bridges.
    static std::atomic<std::uint32_t>         g_pftLastBridgedPlanetID{ 0 };

    struct PendingFastTravel {
        std::atomic<bool>          armed{ false };
        std::atomic<std::uint32_t> shipFormID{ 0 };
        std::atomic<std::uint32_t> destCellID{ 0 };
        std::atomic<std::uint32_t> destWSID{ 0 };
        std::atomic<float>         destX{ 0.0f };
        std::atomic<float>         destY{ 0.0f };
        std::atomic<float>         destZ{ 0.0f };
        std::atomic<std::uint64_t> receivedAtMs{ 0 };
    };
    static PendingFastTravel g_pendingFT;

    // (no PFT sink - replaced with FarTravel+RampDown sequence in
    // OnFarTravelFired / OnRampDownFired below)

    // L.7 V7c: poll-based arrival detector via the engine's GetCurrentPlanet
    // Papyrus native. Returns the player's currently-orbited planet REFR.
    // When that REFR's FormID changes between polls = host has arrived in a
    // different planet/system. ship.parentCell + cellWorldspace + ship pos
    // all stay constant during inter-system jumps (verified in 17:43 logs),
    // so they can't drive detection - but GetCurrentPlanet does change.

    using GetCurrentPlanetFn =
        RE::TESForm* (__fastcall*)(void*, void*, RE::TESObjectREFR*);

    static RE::TESForm* CallGetCurrentPlanet(RE::TESObjectREFR* a_refr)
    {
        if (!a_refr) return nullptr;
        const auto fnAddr = REL::Offset(kRVA_GetCurrentPlanet).address();
        if (!fnAddr) return nullptr;
        auto fn = reinterpret_cast<GetCurrentPlanetFn>(fnAddr);
        RE::TESForm* result = nullptr;
        __try { result = fn(nullptr, nullptr, a_refr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { result = nullptr; }
        return result;
    }

    static void TickHostPFTArrivalPoll()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected() || !net.IsHost()) return;

        const std::uint64_t now = ::GetTickCount64();
        const std::uint64_t lastPoll =
            g_pftLastPollMs.load(std::memory_order_acquire);
        if (lastPoll != 0 && (now - lastPoll) < kPFTPollIntervalMs) return;
        g_pftLastPollMs.store(now, std::memory_order_release);

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        RE::TESForm* planet = CallGetCurrentPlanet(player);
        std::uint32_t planetID = 0;
        __try { planetID = planet ? planet->formID : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { planetID = 0; }

        const bool hadBaseline =
            g_pftHaveBaseline.load(std::memory_order_acquire);
        const std::uint32_t prevID =
            g_pftPrevPlanetFormID.load(std::memory_order_acquire);

        const bool changed = hadBaseline && (planetID != prevID);

        // Always commit baseline.
        g_pftPrevPlanetFormID.store(planetID, std::memory_order_release);
        g_pftHaveBaseline.store(true, std::memory_order_release);

        if (changed) {
            g_pftLastChurnMs.store(now, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] [L.7 PFT] planet churn: 0x{:08X}→0x{:08X}"sv,
                prevID, planetID);
            return;
        }

        // No change - check if recent churn settled.
        const std::uint64_t lastChurn =
            g_pftLastChurnMs.load(std::memory_order_acquire);
        if (lastChurn == 0) return;
        if ((now - lastChurn) < kPFTStableSettleMs) return;

        // Send-cooldown gate.
        const std::uint64_t lastSend =
            g_pftLastSendMs.load(std::memory_order_acquire);
        if (lastSend != 0 && (now - lastSend) < kPFTSendCooldownMs) {
            g_pftLastChurnMs.store(0, std::memory_order_release);
            return;
        }

        // Don't fire if we've settled into "no planet" (deep space) - only
        // arrival AT a planet is meaningful.
        if (planetID == 0) {
            g_pftLastChurnMs.store(0, std::memory_order_release);
            return;
        }

        // L.7 1.12 - same-planet POI travel filter. Engine fires planet
        // churn 0x5E30E → 0 → 0x5E30E for surface-POI fast-travel because
        // the host briefly leaves orbit and returns to the same planet
        // surface. The state visible to client (host on planet 0x5E30E)
        // didn't change, so re-bridging is redundant and triggers a
        // double-loadscreen. If new planet matches the last planet we
        // bridged, treat as POI within same planet and skip.
        const std::uint32_t lastBridged =
            g_pftLastBridgedPlanetID.load(std::memory_order_acquire);
        if (lastBridged != 0 && planetID == lastBridged) {
            g_pftLastChurnMs.store(0, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] [L.7 1.12] PFT arrival at SAME planet 0x{:08X} "
                "as last bridge - likely POI within same planet, skipping"sv,
                planetID);
            NASLOG("L.7 1.12 PFT same-planet skip planet=0x%08X", planetID);
            return;
        }

        g_pftLastChurnMs.store(0, std::memory_order_release);
        g_pftLastSendMs.store(now, std::memory_order_release);
        g_pftCompletedAtMs.store(now, std::memory_order_release);
        // L.7 1.5 - record arrival time so TickPendingTransition can classify
        // a follow-up Landing event as REAL even when inSpace doesn't flip.
        g_lastPFTArmedMs.store(now, std::memory_order_release);
        // L.7 1.12 - record this as the last bridged planet so future
        // same-planet POI travels are filtered.
        g_pftLastBridgedPlanetID.store(planetID, std::memory_order_release);

        PLOG_INFO(
            "[PULSAR] [L.7 PFT] arrival at planet 0x{:08X} "
            "(stable {}ms) - deferred send armed +{}ms"sv,
            planetID, now - lastChurn, kPFTSettleDelayMs);
        NASLOG("L.7 PFT arrival planet=0x%08X", planetID);
    }

    static void TickHostPFTDeferredSend()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected() || !net.IsHost()) return;

        const std::uint64_t armed = g_pftCompletedAtMs.load(std::memory_order_acquire);
        if (armed == 0) return;
        const std::uint64_t now = ::GetTickCount64();
        if (now - armed < kPFTSettleDelayMs) return;
        // Disarm before reading so a second PFT during settle doesn't re-fire.
        g_pftCompletedAtMs.store(0, std::memory_order_release);

        // L.7 V7e: send only the BGSPlanet PNDT FormID. The earlier V7d
        // cell-walk (FindRefByBaseInCell) caused a host crash exactly at
        // send-time - ForEachReference holds a read-lock while we walk +
        // dereference NiPointers, and arrival cell-streaming can leave
        // half-initialised REFRs in the array. Reverting that walk keeps
        // the host stable; PNDT→REFR resolution moves to the client side
        // where it can run after the apply path has already MoveTo'd into
        // the destination cell (see TickClientPFTApply).
        Pulsar::Network::FastTravelSyncData data{};
        std::uint32_t planetPNDTId = 0;
        RE::TESObjectREFR* ship = nullptr;
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            ship = player->GetSpaceship(true);
            if (!ship) return;
            auto* planet = CallGetCurrentPlanet(player);
            if (!planet) {
                PLOG_WARN(
                    "[PULSAR] [L.7 PFT-SEND] GetCurrentPlanet=null at send "
                    "(deep space?) - skipping"sv);
                return;
            }
            planetPNDTId = planet->formID;
            data.shipFormID     = ship->formID;
            data.destCellFormID = planetPNDTId;  // BGSPlanet PNDT FormID
            data.destWSFormID   = 0;
            data.destPos[0]     = ship->data.location.x;
            data.destPos[1]     = ship->data.location.y;
            data.destPos[2]     = ship->data.location.z;
            data.timestampMs    = now;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] [L.7 PFT-SEND] host state read threw"sv);
            return;
        }
        if (planetPNDTId == 0) return;
        PLOG_INFO(
            "[PULSAR] [L.7 PFT-SEND] planetPNDT=0x{:08X}"sv, planetPNDTId);

        std::uint8_t buf[64]{};
        Pulsar::Network::PacketWriter w(
            Pulsar::Network::PKT_FAST_TRAVEL_SYNC, buf, sizeof(buf));
        if (!w.Write(data)) {
            PLOG_WARN("[PULSAR] [L.7 PFT-SEND] PacketWriter::Write failed"sv);
            return;
        }
        net.Send(buf, w.Size(), /*reliable*/ true);
        PLOG_INFO(
            "[PULSAR] [L.7 PFT-SEND] ship=0x{:08X} destCell=0x{:08X} destWS=0x{:08X} "
            "pos=({:.1f},{:.1f},{:.1f})"sv,
            data.shipFormID, data.destCellFormID, data.destWSFormID,
            data.destPos[0], data.destPos[1], data.destPos[2]);

#if PULSAR_L7_FT_BRIDGE_ENABLED
        // L.7 V14 - unified entrypoint. PFT-arrival path: engine has just
        // auto-quicksaved post-grav-jump, so a_triggerOwnSave=false (poll
        // existing newest .sfs). For TakeOff/Landing/Dock the SpaceshipSink
        // calls FireSaveBridge with trigger=true.
        FireSaveBridge("PFT-arrival", /*a_triggerOwnSave=*/false);
#endif  // PULSAR_L7_FT_BRIDGE_ENABLED
    }

    using EngineMoveTo_t = bool (__fastcall*)(RE::TESObjectREFR*, RE::TESObjectREFR*);

    // Splitting the ForEachReference call out of any function that uses __try:
    // the callable involves a std::function copy + destructor, which trips
    // MSVC's C2712 ("cannot use __try in functions with destructor-bearing
    // locals"). The lock-and-iterate path itself doesn't need SEH.
    static RE::TESObjectREFR* FindAnyRefInCellInner(RE::TESObjectCELL* cell,
                                                     RE::TESObjectREFR* exclude)
    {
        RE::TESObjectREFR* found = nullptr;
        cell->ForEachReference(
            [&](const RE::NiPointer<RE::TESObjectREFR>& r) {
                if (r && r.get() != exclude) {
                    found = r.get();
                    return RE::BSContainer::ForEachResult::kStop;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        return found;
    }

    static RE::TESObjectREFR* FindAnyRefInCell(RE::TESObjectCELL* cell,
                                                RE::TESObjectREFR* exclude)
    {
        if (!cell) return nullptr;
        return FindAnyRefInCellInner(cell, exclude);
    }

    // L.7 V7d: locate a REFR in the cell whose baseForm matches the given
    // BGSPlanet PNDT. That REFR is the in-world placement of the planet -
    // exactly what engine MoveTo needs as its destination.
    static RE::TESObjectREFR* FindRefByBaseInCellInner(
        RE::TESObjectCELL* cell, std::uint32_t baseFormID)
    {
        RE::TESObjectREFR* found = nullptr;
        cell->ForEachReference(
            [&](const RE::NiPointer<RE::TESObjectREFR>& r) {
                if (r) {
                    auto base = r->data.objectReference;
                    if (base && base->formID == baseFormID) {
                        found = r.get();
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        return found;
    }

    static RE::TESObjectREFR* FindRefByBaseInCell(RE::TESObjectCELL* cell,
                                                   std::uint32_t baseFormID)
    {
        if (!cell || baseFormID == 0) return nullptr;
        return FindRefByBaseInCellInner(cell, baseFormID);
    }

    static void TickClientPFTApply()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        // Both peers may be receivers (pilot handover); only the non-active
        // side should consume. Cheap gate: skip if our local player is the
        // active pilot of a ship right now.
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        if (!g_pendingFT.armed.load(std::memory_order_acquire)) return;

        const std::uint64_t now    = ::GetTickCount64();
        const std::uint64_t recvAt = g_pendingFT.receivedAtMs.load(std::memory_order_acquire);
        if (recvAt != 0 && (now - recvAt) > kPFTApplyStaleMs) {
            PLOG_WARN("[PULSAR] [L.7 PFT-APPLY] stale by {}ms - dropping"sv,
                      now - recvAt);
            g_pendingFT.armed.store(false, std::memory_order_release);
            return;
        }
        const std::uint64_t lastApply = g_pftLastApplyMs.load(std::memory_order_acquire);
        if (lastApply != 0 && (now - lastApply) < kPFTApplyCooldownMs) {
            return;  // hold the pending until cooldown elapses
        }

        const std::uint32_t shipFormID = g_pendingFT.shipFormID.load(std::memory_order_acquire);
        const std::uint32_t destCellID = g_pendingFT.destCellID.load(std::memory_order_acquire);

        // Locate local ship REFR - primary path = local pilot's GetSpaceship,
        // fallback = TESForm::LookupByID(shipFormID).
        RE::TESObjectREFR* localShip = player->GetSpaceship(true);
        if (!localShip && shipFormID != 0) {
            auto* form = RE::TESForm::LookupByID(shipFormID);
            if (form) localShip = form->As<RE::TESObjectREFR>();
        }
        if (!localShip) {
            PLOG_WARN(
                "[PULSAR] [L.7 PFT-APPLY] no local ship REFR (shipFormID=0x{:08X})"sv,
                shipFormID);
            g_pendingFT.armed.store(false, std::memory_order_release);
            return;
        }

        // L.7 V7c: destCellID is the BGSPlanet::PlanetData FormID (formType
        // 0xBA / kPNDT) - NOT a REFR. We log diagnostics about what we got
        // back and try the cast anyway; the Phase-3 follow-up will resolve
        // the planet's in-world REFR placement (so engine MoveTo can target
        // it for cell-streamer-aware teleport).
        auto* form = RE::TESForm::LookupByID(destCellID);
        if (!form) {
            PLOG_WARN(
                "[PULSAR] [L.7 PFT-APPLY] form lookup returned null (formID=0x{:08X})"sv,
                destCellID);
            g_pendingFT.armed.store(false, std::memory_order_release);
            return;
        }

        std::uint8_t formTypeByte = 0;
        std::uintptr_t formVTable = 0;
        __try {
            formTypeByte = *reinterpret_cast<std::uint8_t*>(
                reinterpret_cast<std::uint8_t*>(form) + 0x2E);
            formVTable = *reinterpret_cast<std::uintptr_t*>(form);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        PLOG_INFO(
            "[PULSAR] [L.7 PFT-APPLY] form lookup: ptr=0x{:X} formType=0x{:02X} vt=0x{:X}"sv,
            reinterpret_cast<std::uintptr_t>(form), formTypeByte, formVTable);

        auto* destMarker = form->As<RE::TESObjectREFR>();
        if (!destMarker) {
            PLOG_WARN(
                "[PULSAR] [L.7 PFT-APPLY] form is not a REFR (formType=0x{:02X}, "
                "expected 0x40 for REFR; got 0xBA = BGSPlanet::PlanetData). "
                "Phase-3 needs to resolve PNDT→REFR or call Game.FastTravel."sv,
                formTypeByte);
            g_pendingFT.armed.store(false, std::memory_order_release);
            return;
        }

        const auto fnAddr = REL::Offset(kRVA_EngineMoveTo).address();
        if (!fnAddr) {
            PLOG_WARN("[PULSAR] [L.7 PFT-APPLY] EngineMoveTo address unresolved"sv);
            g_pendingFT.armed.store(false, std::memory_order_release);
            return;
        }
        auto fn = reinterpret_cast<EngineMoveTo_t>(fnAddr);

        bool moveOk = false;
        __try { moveOk = fn(localShip, destMarker); }
        __except (EXCEPTION_EXECUTE_HANDLER) { moveOk = false; }

        PLOG_INFO(
            "[PULSAR] [L.7 PFT-APPLY] MoveTo result={} ship=0x{:08X} dest=0x{:08X} "
            "destCell=0x{:08X}"sv,
            moveOk ? "ok" : "fail",
            shipFormID,
            destMarker ? destMarker->formID : 0u,
            destCellID);
        NASLOG("L.7 PFT-APPLY MoveTo=%s ship=0x%08X destCell=0x%08X",
               moveOk ? "ok" : "fail", shipFormID, destCellID);

        g_pftLastApplyMs.store(now, std::memory_order_release);
        g_pendingFT.armed.store(false, std::memory_order_release);
    }

}  // namespace

// L.7 - forward declarations for cross-block referenced symbols. The
// autosave_checker_ (line ~1348) and landing_diag_ (line ~1421) namespaces
// are defined further down but referenced earlier by ArmTransitionFire,
// Init, and Tick. Forward-declare just the cross-referenced symbols here;
// internal-only symbols stay inside their respective namespace bodies.
namespace autosave_checker_ {
    extern std::atomic<std::uint64_t> g_landingArmedMs;
    extern std::atomic<int>           g_landingInSpaceAtArm;
    extern std::atomic<std::uint64_t> g_landingFlipMs;
    // L.7 event-gated post-flip wait (replaces fixed timer). Floor +
    // ceiling stay constant; gate condition is `LoadingMenu == 0` =
    // cell-stream complete = safe to serialize via TriggerHostQuicksave.
    inline constexpr std::uint64_t    kLandingMinPostFlipMs = 5000;
    inline constexpr std::uint64_t    kLandingMaxPostFlipMs = 30000;
    // L.7 manual-force flag: set by F8 hotkey. While non-zero, the
    // autosave-checker fires the bridge on the next new save regardless of
    // WorldKey (normal POI/manual-save world-key filter is bypassed).
    // Cleared on fire or after kManualForceTimeoutMs.
    extern std::atomic<std::uint64_t> g_manualForceArmedMs;
    inline constexpr std::uint64_t    kManualForceTimeoutMs = 30000;

    // L.7 in-space POI fix: set when SpaceshipSink fires a real travel
    // event (FarTravel / GravJump). Hosts POI-traveling within space keep
    // (inSpace=1, planetID) constant for both endpoints (orbit-to-orbit in
    // same system), so the WorldKey filter would otherwise skip the post-
    // travel autosave as "POI/timer". This timestamp tells the autosave-
    // checker "an actual travel just happened - fire bridge on next save".
    extern std::atomic<std::uint64_t> g_recentSpaceEventMs;
    inline constexpr std::uint64_t    kRecentSpaceEventWindowMs = 30000;
}
namespace landing_diag_ {
    void RegisterLandedSetSink();
    // L.7 diag-driven gate for post-flip quicksave decision.
    // Returns true when LoadingMenu is closed (cell-stream complete).
    // SEH-guarded internally.
    bool IsSafeQuicksaveGate();
}

void Init()
{
    PLOG_INFO(
        "[PULSAR] L.7 V7 FastTravelSync init "
        "(trigger: FarTravel→RampDown sequence)"sv);

    // L.7 register diag-only sink for LandedSetEvent. Used to
    // correlate engine "landed-set" signal vs the Landing-armed window
    // and identify a safe quicksave-trigger gate.
    landing_diag_::RegisterLandedSetSink();
}

// Shared core of the manual save-bridge (F8 hotkey AND client-request).
// MUST run on the GAME thread (TriggerHostAutoSave touches engine state - calling
// it from the network-receive thread freezes the host; see OnClientLoadComplete).
//
// Non-blocking bit-OR path only. CustomSaveDirect was tried earlier but is
// synchronous (waits on BSService completion); if the engine is in a save-lock
// state it hangs the game thread forever (observed 2026-05-06 - F8 froze the
// host). Bit-OR returns immediately; either the engine processes it (file lands
// → autosave-checker fires bridge on next tick) or it silently no-ops (retry).
static void FireManualBridgeNow(const char* a_reason)
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) return;

    PLOG_INFO(
        "[PULSAR] [L.7/] manual bridge ({}) - forcing host save + "
        "client bridge (bypassing auto-fire gates AND world-key filter)"sv,
        a_reason);
    NASLOG("L.7 manual bridge: %s", a_reason);

    Pulsar::WorldSync::TriggerHostAutoSave();

    // Arm the manual-force flag so the autosave-checker bypasses its world-key
    // filter for the NEXT new save (else a same-planet save gets filtered as
    // "world key unchanged - POI/timer/manual save, skipping bridge").
    autosave_checker_::g_manualForceArmedMs.store(
        ::GetTickCount64(), std::memory_order_release);
}

// client bridge-request queue. The packet handler runs on the
// network-receive thread and only records the request timestamp; the game-thread
// poll (TickManualBridgeHotkey) drains it and fires FireManualBridgeNow.
static std::atomic<std::uint64_t> g_clientBridgeReqMs{ 0 };       // set by packet, drained game-thread
static std::atomic<std::uint64_t> g_lastClientBridgeFireMs{ 0 };  // host-side debounce
inline constexpr std::uint64_t    kClientBridgeDebounceMs = 20000;

// Network-receive thread: queue a client's "I can't AutoFollow you" request.
void OnBridgeRequestPacket(const std::uint8_t* /*data*/, std::size_t /*length*/)
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) return;  // host-only
    g_clientBridgeReqMs.store(::GetTickCount64(), std::memory_order_release);
    PLOG_INFO(
        "[PULSAR] [L.7] client bridge-request received - queued for game thread"sv);
    NASLOG("L.7 client bridge-request RECV");
}

// L.7 manual save-bridge trigger. F8 edge-detect (matches the F7
// pattern used by TransitSync::TickFollowHotkey). Host-only; client-side
// presses are no-ops. Polled per-tick from Main.cpp. Also drains the
// client bridge-request queue (auto co-location).
//
// Use case: engine sometimes skips its natural disembark-autosave when it
// considers state unchanged (e.g. pilot returns to a cached planet) - in
// that case the autosave-checker has nothing to fire on, and the client
// stays parked. Manual trigger gives the user a "force-pull-client-here"
// override that doesn't depend on engine quirks.
void TickManualBridgeHotkey(int a_vkey)
{
#if !PULSAR_L7_FT_BRIDGE_ENABLED
    return;
#else
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected()) return;

    // Client: F8 asks the host to regroup us (PKT_BRIDGE_REQUEST). Before 0.4.0
    // this function returned here for clients, so F8 did nothing on the client
    // although the README tells the client to press it.
    if (!net.IsHost()) {
        static bool s_clientLastDown = false;
        const bool down = (::GetAsyncKeyState(a_vkey) & 0x8000) != 0;
        if (down && !s_clientLastDown)
            Pulsar::TransitSync::RequestHostBridgeFromClient("F8 hotkey (client)");
        s_clientLastDown = down;
        return;
    }

    // auto co-location: drain a queued client bridge-request (sent when
    // the client's AutoFollow had to SKIP across a proc/cross-worldspace
    // boundary). Debounced so the duplicate transit broadcasts of one
    // fast-travel collapse into a single bridge. F8 below stays as the manual
    // fallback when this (or the engine autosave) doesn't fire.
    const std::uint64_t req = g_clientBridgeReqMs.exchange(0, std::memory_order_acq_rel);
    if (req != 0) {
        const std::uint64_t now  = ::GetTickCount64();
        const std::uint64_t last = g_lastClientBridgeFireMs.load(std::memory_order_acquire);
        if (last != 0 && now - last < kClientBridgeDebounceMs) {
            PLOG_INFO(
                "[PULSAR] [L.7] client bridge-request SKIPPED "
                "({}ms since last < {}ms debounce)"sv, now - last, kClientBridgeDebounceMs);
        } else {
            g_lastClientBridgeFireMs.store(now, std::memory_order_release);
            FireManualBridgeNow("client-request - auto co-location");
        }
    }

    static bool s_lastDown = false;
    const bool down = (::GetAsyncKeyState(a_vkey) & 0x8000) != 0;
    if (!down) {
        s_lastDown = false;
        return;
    }
    if (s_lastDown) return;  // edge-only
    s_lastDown = true;

    FireManualBridgeNow("F8 hotkey");
#endif
}

void OnGravJumpFired()
{
    const auto now = ::GetTickCount64();
    g_lastGravJumpFireMs.store(now, std::memory_order_release);
    // L.7 in-space POI fix - mark recent space-event so autosave-checker
    // bypasses world-key filter when the post-jump engine autosave lands.
    autosave_checker_::g_recentSpaceEventMs.store(now, std::memory_order_release);
}

void OnFarTravelFired()
{
    // Diagnostic only; actual arrival detection is poll-based now.
    PLOG_INFO("[PULSAR] [L.7 PFT] FarTravel observed (poll-driven trigger)"sv);
    // L.7 in-space POI fix - mark recent space-event so autosave-checker
    // bypasses world-key filter. POI traveling within the same system keeps
    // (inSpace=1, planetID) constant for both endpoints, so without this
    // override the post-travel autosave gets filtered as "POI/timer save".
    autosave_checker_::g_recentSpaceEventMs.store(
        ::GetTickCount64(), std::memory_order_release);
}

void OnRampDownFired()
{
    // Diagnostic only; RampDown doesn't fire for inter-system jumps so
    // can't be the primary trigger. Poll-driven detector handles it.
    PLOG_INFO("[PULSAR] [L.7 PFT] RampDown observed (poll-driven trigger)"sv);
}

void NotifyClientLoadComplete(std::uint8_t reason)
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || net.IsHost()) return;  // client → host only

    const std::uint64_t now = ::GetTickCount64();
    std::uint8_t buf[32]{};
    Pulsar::Network::PacketWriter w(
        Pulsar::Network::PKT_CLIENT_LOAD_COMPLETE, buf, sizeof(buf));
    w.Write(reason);
    w.Write(now);
    net.Send(buf, w.Size(), /*reliable*/ true);
    PLOG_INFO(
        "[PULSAR] [L.7 V8] client→host LOAD_COMPLETE sent (reason={})"sv,
        reason);
    NASLOG("L.7 V8 client load-complete reason=%u", reason);
}

void OnClientLoadCompletePacket(const std::uint8_t* data, std::size_t length)
{
    if (!data) return;
    Pulsar::Network::PacketReader r(data, length);
    if (!r.IsValid()) return;
    if (r.Type() != Pulsar::Network::PKT_CLIENT_LOAD_COMPLETE) return;
    std::uint8_t  reason = 0;
    std::uint64_t ts     = 0;
    if (!r.Read(reason)) return;
    if (!r.Read(ts)) return;
#if PULSAR_L7_FT_BRIDGE_ENABLED
    g_clientLoadCompleted.store(true, std::memory_order_release);

    // L.7 1.9 - first-ack checkpoint, NARROWED in 1.15. Originally dropped
    // ANY pending bridge on first ack (intent: dismiss the cell-init
    // Landing/RampDown bridges that fire during host save-load). Problem:
    // it ALSO killed legitimate post-handshake bridges - e.g. real Landing
    // after grav-jump, real TakeOff - that happened to be queued waiting
    // for the first ack to arrive. Observed 2026-05-05 23:48: real
    // Landing armed via 1.5 PFT-recent gate, queued (no ack yet), first
    // ack arrived 2s later, 1.9 killed the bridge, client never got the
    // landing save.
    //
    // Fix: only drop pending if it was armed within 30s of host save-load
    // (= cell-init artifact window). Pending events armed later are real
    // user transitions and must fire.
    if (!g_firstAckReceived.exchange(true, std::memory_order_acq_rel)) {
        g_firstAckMs.store(::GetTickCount64(), std::memory_order_release);
        std::lock_guard lock(g_pendingBridgeMutex);
        if (g_pendingBridge.valid) {
            const std::uint64_t now = ::GetTickCount64();
            const std::uint64_t pendingAge = now - g_pendingBridge.armedMs;
            // L.7 1.15 - pending armed within 30s window of host load is
            // likely cell-init artifact. Use StateProbe::GetEntryHookCount
            // to detect a recent load: if probe count incremented in the
            // last 30s, the pending is from that load's cell-init.
            const auto curProbe = Pulsar::WorldSync::StateProbe::GetEntryHookCount();
            const bool isCellInitArtifact = (pendingAge < 30000) &&
                                            (curProbe > 0);
            if (isCellInitArtifact) {
                PLOG_INFO(
                    "[PULSAR] [L.7 1.9] First ack - dropping pending bridge "
                    "(reason={}, age={}ms < 30s - likely cell-init artifact)"sv,
                    g_pendingBridge.reason, pendingAge);
                NASLOG("L.7 1.9 first-ack drop pending reason=%s age=%llu",
                       g_pendingBridge.reason.c_str(), pendingAge);
                g_pendingBridge.valid = false;
            } else {
                PLOG_INFO(
                    "[PULSAR] [L.7 1.9] First ack - KEEPING pending bridge "
                    "(reason={}, age={}ms >= 30s - real post-connect event, "
                    "will fire via 1.4 retry)"sv,
                    g_pendingBridge.reason, pendingAge);
                NASLOG("L.7 1.9 first-ack keep pending reason=%s age=%llu",
                       g_pendingBridge.reason.c_str(), pendingAge);
            }
        } else {
            PLOG_INFO("[PULSAR] [L.7 1.9] First ack - no pending bridge to drop"sv);
        }
    }
#endif
    PLOG_INFO(
        "[PULSAR] [L.7 V8] host RECV LOAD_COMPLETE reason={} ts={}"sv,
        reason, ts);
    NASLOG("L.7 V8 host RECV load-complete reason=%u", reason);
    // L.7 1.4 - pending-bridge drain happens in TickPendingBridgeRetry on
    // the game thread (this packet handler runs on network thread; calling
    // FireSaveBridge here freezes the host because BeginHostBridgeSaveTransfer
    // touches engine state from the wrong thread).
}

void OnFastTravelPacket(const std::uint8_t* data, std::size_t length)
{
    if (!data) return;
    Pulsar::Network::PacketReader r(data, length);
    if (!r.IsValid()) return;
    if (r.Type() != Pulsar::Network::PKT_FAST_TRAVEL_SYNC) return;

    Pulsar::Network::FastTravelSyncData payload{};
    if (!r.Read(payload)) {
        PLOG_WARN("[PULSAR] [L.7 PFT-RECV] failed to read payload"sv);
        return;
    }

    g_pendingFT.shipFormID.store(payload.shipFormID, std::memory_order_relaxed);
    g_pendingFT.destCellID.store(payload.destCellFormID, std::memory_order_relaxed);
    g_pendingFT.destWSID.store(payload.destWSFormID, std::memory_order_relaxed);
    g_pendingFT.destX.store(payload.destPos[0], std::memory_order_relaxed);
    g_pendingFT.destY.store(payload.destPos[1], std::memory_order_relaxed);
    g_pendingFT.destZ.store(payload.destPos[2], std::memory_order_relaxed);
    g_pendingFT.receivedAtMs.store(::GetTickCount64(), std::memory_order_relaxed);
    g_pendingFT.armed.store(true, std::memory_order_release);

    PLOG_INFO(
        "[PULSAR] [L.7 PFT-RECV] ship=0x{:08X} destCell=0x{:08X} destWS=0x{:08X} "
        "pos=({:.1f},{:.1f},{:.1f})"sv,
        payload.shipFormID, payload.destCellFormID, payload.destWSFormID,
        payload.destPos[0], payload.destPos[1], payload.destPos[2]);
}

// Forward decl so Tick() below can call it.
static void TickPendingTransition();
static void TickPendingBridgeRetry();
static void TickAutosaveChecker();  // L.7 2.0 - engine-autosave-driven bridge
static void TickLandingTracker();   // L.7 2.1 - explicit Landing → quicksave trigger
static void TickLandingDiagProbe(); // L.7 landing-cinematic instrumentation

void Tick()
{
    TickDeltaProbe();
    TickHostPFTArrivalPoll();
    // L.7 2.0 - TickHostPFTDeferredSend, TickPendingTransition disabled.
    // 2.1 - TickLandingTracker forces an engine save shortly after the
    // host actually lands (IsInSpace 1→0 + 10s settle), without waiting
    // for the host-exits-ship engine autosave.
    // diag probe runs alongside tracker while armed (1Hz log).
    TickLandingDiagProbe();
    TickLandingTracker();
    TickAutosaveChecker();
    TickPendingBridgeRetry();
    TickClientPFTApply();
}

// V14.2 - SEH-safe IsInSpace probe. Engine's flag is the cleanest test
// for a REAL surface↔space transition vs a spurious Landing/RampDown
// fired by door/POI cell-changes the engine misclassifies.
namespace detail_v142_ {
    static int CheckInSpace_Impl()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return -1;
        return player->IsInSpace(true) ? 1 : 0;
    }
    static int CheckInSpaceSafe()
    {
        __try { return CheckInSpace_Impl(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    // L.7 1.13 - capture (pcell, inSpace, shipCell) atomically. SEH-guarded
    // because shipCell deref through ship REFR can fault during cell-stream.
    static EngineSnapshot SnapshotEngineState()
    {
        EngineSnapshot snap{};
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return snap;
        __try {
            snap.pcell = player->parentCell ? player->parentCell->formID : 0;
            snap.inSpace = player->IsInSpace(true) ? 1 : 0;
            auto* ship = player->GetSpaceship(true);
            if (ship) {
                if (ship->parentCell) {
                    snap.shipCell = ship->parentCell->formID;
                }
                // Round to int so micro-jitter doesn't reset the change
                // timer. Cutscene-driven motion is on the order of metres.
                snap.shipPosX = static_cast<std::int32_t>(ship->data.location.x);
                snap.shipPosY = static_cast<std::int32_t>(ship->data.location.y);
                snap.shipPosZ = static_cast<std::int32_t>(ship->data.location.z);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return snap;
    }
}

// L.7 V14.1 / V14.2 - arm deferred transition fire. Sink calls this on
// TakeOff/Landing event. V14.2 adds an IsInSpace flip-check so spurious
// Landing/RampDown events (door transitions, POI fast-travel within
// planet) don't trigger a save-bridge. Real takeoff: 0→1, real landing:
// 1→0, anything else: skipped. Re-arm idempotent - multifire per real
// action collapses to one delayed fire 5s after the LAST event.
bool ArmTransitionFire(const char* a_reason)
{
#if !PULSAR_L7_FT_BRIDGE_ENABLED
    (void)a_reason;
    return false;
#else
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) return false;

    int curInSpace = detail_v142_::CheckInSpaceSafe();
    if (curInSpace < 0) return false;

    const auto now = ::GetTickCount64();

    // L.7 1.11 - suppress TakeOff arms within 30s of first ack. The
    // engine fires a spurious TakeOff sink ~14s after a host loads a
    // save-in-space (observed empirically; root cause is engine cell-
    // init scheduling). Without this gate, that spurious event fires a
    // redundant save-bridge that re-loads the same state on client.
    // Tied to the first-ack checkpoint (a real event, not a wall-clock
    // assumption). Landing has its own gate via 1.5 PFT-recent and
    // doesn't go through this suppression.
    if (a_reason && std::strcmp(a_reason, "TakeOff") == 0) {
        const auto firstAckMs = g_firstAckMs.load(std::memory_order_acquire);
        if (firstAckMs != 0 && (now - firstAckMs) < kFirstAckTakeOffSuppressMs) {
            PLOG_INFO(
                "[PULSAR] [L.7 1.11] TakeOff arm suppressed "
                "({}ms after first ack < {}ms - likely cell-init artifact)"sv,
                now - firstAckMs, kFirstAckTakeOffSuppressMs);
            NASLOG("L.7 1.11 TakeOff arm suppressed age=%llu", now - firstAckMs);
            return true;  // pretend success so broadcast still happens
        }
    }

    // L.7 1.7 - suppress duplicate arms when the engine fires the same
    // event multiple times during a single cinematic. After a successful
    // bridge fire for reason X, additional arms for X within
    // kReasonSuppressMs are no-ops. This prevents the double-loadscreen
    // pattern observed during landing cinematics (Landing event fires 3-4×
    // across animation phases, first fire OK, second slips through cool-
    // down expiry → second loadscreen).
    {
        const char* lastReason = g_lastFiredReason;
        const std::uint64_t lastMs = g_lastFiredReasonMs.load(std::memory_order_acquire);
        if (a_reason && lastReason &&
            std::strcmp(a_reason, lastReason) == 0 &&
            lastMs != 0 && (now - lastMs) < kReasonSuppressMs)
        {
            PLOG_INFO(
                "[PULSAR] [L.7 1.7] arm suppressed (reason={} fired {}ms "
                "ago < {}ms - cinematic re-fire dedup)"sv,
                a_reason, now - lastMs, kReasonSuppressMs);
            return true;  // pretend success so broadcast still happens
        }
    }

    // L.7 2.1 - for Landing events specifically, arm the explicit Landing
    // tracker which forces an engine save shortly after touchdown. This
    // gives client-follow timing of "shortly after host lands" instead of
    // "after host exits ship" (which is when the engine's natural
    // autosave fires).
    if (a_reason && std::strcmp(a_reason, "Landing") == 0 &&
        autosave_checker_::g_landingArmedMs.load(std::memory_order_acquire) == 0)
    {
        autosave_checker_::g_landingArmedMs.store(now, std::memory_order_release);
        autosave_checker_::g_landingInSpaceAtArm.store(curInSpace, std::memory_order_release);
        autosave_checker_::g_landingFlipMs.store(0, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] [L.7 2.1] Landing tracker armed - inSpace at arm={}, "
            "waiting for 1→0 flip + LoadingMenu close (min {}ms / max {}ms)"sv,
            curInSpace,
            autosave_checker_::kLandingMinPostFlipMs,
            autosave_checker_::kLandingMaxPostFlipMs);
    }

    // V15.3 - record inSpace AT ARM. Real-vs-spurious decision is deferred
    // to TickPendingTransition (settle later). The TakeOff event fires
    // BEFORE the engine transitions cells, so at-fire inSpace still
    // matches the pre-state. Only after settle has the engine actually
    // transitioned.
    g_pendingTransitionReason = a_reason;
    g_pendingTransitionInSpaceAtArm.store(curInSpace, std::memory_order_release);
    g_pendingTransitionArmMs.store(now, std::memory_order_release);

    // L.7 1.13 - capture engine baseline at arm. The state-stability check
    // in TickPendingTransition compares against this and waits for the
    // engine to actually transition + settle before firing.
    g_engineAtArm = detail_v142_::SnapshotEngineState();
    g_engineLast = g_engineAtArm;
    g_engineLastChangeMs.store(0, std::memory_order_release);
    g_engineChangedSinceArm.store(false, std::memory_order_release);

    // L.7 1.14 - capture newest .sfs filename at arm. If the engine writes
    // a NEW save after the cinematic completes, that's our most reliable
    // "transition done" signal - much stronger than state stability alone.
    g_armNewestSavePath = Pulsar::SaveScanner::GetMostRecentSavePath();
    PLOG_INFO(
        "[PULSAR] [L.7 V15.3] transition fire ARMED (reason={}) - "
        "inSpace at arm={} - re-check + decide in {}ms"sv,
        a_reason, curInSpace, kTransitionSettleMs);
    NASLOG("V15.3 transition ARMED reason=%s inSpaceAtArm=%d",
           a_reason, curInSpace);
    // Always treat as "real" for broadcast purposes - the actual
    // save-bridge fire is gated 5s later by the post-settle inSpace check
    // in TickPendingTransition. This way real takeoff/landing broadcasts
    // pass through immediately (auto-follow on client), and the heavy
    // save-bridge only fires if the post-settle check confirms a real
    // transition.
    return true;
#endif
}

// L.7 V14.1 / V15.3 - drain pending transition fire from Tick(). At settle
// time (5s after arm), re-check inSpace. If it FLIPPED since arm (real
// takeoff/landing), fire the save-bridge. If unchanged, classify as
// spurious (door/POI misclassified by engine as ship event) and skip.
static void TickPendingTransition()
{
#if !PULSAR_L7_FT_BRIDGE_ENABLED
    return;
#else
    auto armed = g_pendingTransitionArmMs.load(std::memory_order_acquire);
    if (armed == 0) return;
    const auto now = ::GetTickCount64();
    if (now - armed < kTransitionSettleMs) return;

    // L.7 1.14 - engine-autosave checkpoint REMOVED for TakeOff/Landing
    // (kept conceptually for PFT-arrival path which has its own flow).
    //
    // Reason: engine writes its autosave only AFTER host exits the ship
    // onto planet exterior. That's too late - coop expectation is "client
    // follows host as soon as ship lands" (mid-cinematic). For that we
    // need to trigger our own quicksave once the ship has settled (ship-
    // pos stable in 1.13 snapshot) but BEFORE host disembarks. The 1.13.2
    // shipPos tracking + stability gives that timing.
    //
    // Tested 2026-05-05 23:28: 1.14 engine-autosave fired Landing bridge
    // ~19s after arm, only when host exited ship - user waited too long
    // and called the client behaviour wrong.
    //
    // Re-entering ship caused client freeze (1.14.1 era), root cause not
    // pinned but tied to bridge firing on enter-ship event. Removing the
    // engine-autosave fast path side-steps that trigger.

    // L.7 1.13 - engine-state stability checkpoint with PLAYER-state focus.
    //
    // Track (pcell, inSpace, shipCell). The check has two phases:
    //   1. Player must have actually transitioned: pcell or inSpace flipped
    //      since arm. shipCell flickers during cinematics without the
    //      player actually relocating, so it doesn't count as "real
    //      transition" - only as "engine still running, keep waiting".
    //   2. Once player transitioned, wait kTransitionStableMs for the
    //      observed (pcell, inSpace, shipCell) tuple to stop changing.
    //      That's "cinematic complete, safe to serialize".
    //
    // For Landing specifically: also require the END state to be "host
    // actually on planet" (IsInSpace == 0). Host might be in the cockpit
    // descending - engine is busy but player still IsInSpace==1. Without
    // this gate the bridge fires mid-cinematic and the save captures host
    // still in space → client loads space state → "client landed in space
    // again" bug observed 2026-05-05 23:05.
    {
        const EngineSnapshot cur = detail_v142_::SnapshotEngineState();
        if (cur != g_engineLast) {
            g_engineLast = cur;
            g_engineLastChangeMs.store(now, std::memory_order_release);
        }
        // "Real" transition = player's primary state (pcell or inSpace)
        // changed. shipCell-only changes are cinematic flicker.
        const bool playerTransitioned =
            (cur.pcell   != g_engineAtArm.pcell)   ||
            (cur.inSpace != g_engineAtArm.inSpace);
        if (playerTransitioned) {
            g_engineChangedSinceArm.store(true, std::memory_order_release);
        }
        const bool changedSinceArm =
            g_engineChangedSinceArm.load(std::memory_order_acquire);
        const std::uint64_t lastChangeMs =
            g_engineLastChangeMs.load(std::memory_order_acquire);

        const char* reason = g_pendingTransitionReason
            ? g_pendingTransitionReason : "?";
        const bool isLandingReason =
            std::strcmp(reason, "Landing") == 0;

        if (!changedSinceArm) {
            // L.7 1.13.3 - player state didn't change since arm. Two cases:
            // (a) Engine already settled at landed state when sink fired
            //     (typical when Landing sink fires post-cinematic). 1.5
            //     PFT-recent gate handles this - fall through to V15.3
            //     code path below.
            // (b) Truly spurious sink (e.g. door event) - V15.3 + 1.5/1.6
            //     handle this too: inSpace flip check + PFT-recent +
            //     TakeOff-unconditional. If nothing matches there, the
            //     V15.3 SPURIOUS path skips fire.
            //
            // Either way the right behaviour is "fall through to V15.3",
            // not "abort and lose the bridge". So we just exit the 1.13
            // block without disarming and let the V15.3 logic run.
            PLOG_INFO(
                "[PULSAR] [L.7 1.13] no player-state change since arm "
                "(reason={}, elapsed={}ms) - falling through to V15.3"sv,
                reason, now - armed);
            // Don't disarm; fall through. But cap wait so we don't loop
            // forever - V15.3 will hit and either fire or skip.
        } else {

            // Wait for the full snapshot to be stable for kTransitionStableMs.
            if (lastChangeMs == 0 || (now - lastChangeMs) < kTransitionStableMs) {
                return;
            }

            // For Landing: require end-state IsInSpace==0.
            if (isLandingReason && cur.inSpace != 0) {
                if (now - armed >= kTransitionMaxWaitMs) {
                    PLOG_INFO(
                        "[PULSAR] [L.7 1.13] Landing aborted - host still in "
                        "space after {}ms"sv, now - armed);
                    NASLOG("L.7 1.13 Landing aborted still-in-space ms=%llu",
                           now - armed);
                    g_pendingTransitionArmMs.store(0, std::memory_order_release);
                    g_pendingTransitionInSpaceAtArm.store(-1, std::memory_order_release);
                }
                return;  // keep waiting for inSpace flip
            }

            PLOG_INFO(
                "[PULSAR] [L.7 1.13] engine settled (reason={}) - pcell {:#x}→{:#x} "
                "inSpace {}→{} - last change {}ms ago, arm-elapsed {}ms - "
                "proceeding to fire"sv,
                reason, g_engineAtArm.pcell, cur.pcell,
                g_engineAtArm.inSpace, cur.inSpace,
                now - lastChangeMs, now - armed);
            NASLOG("L.7 1.13 engine settled reason=%s arm-elapsed=%llu",
                   reason, now - armed);
        }
    }

    // Disarm before firing so a fresh sink call during settle doesn't
    // re-trigger us; if it does, ArmTransitionFire will rearm.
    g_pendingTransitionArmMs.store(0, std::memory_order_release);
    const char* reason = g_pendingTransitionReason
        ? g_pendingTransitionReason
        : "transition";

    // V15.3 post-settle flip check.
    int inSpaceAtArm = g_pendingTransitionInSpaceAtArm.exchange(
        -1, std::memory_order_acq_rel);
    int inSpaceNow = detail_v142_::CheckInSpaceSafe();

    if (inSpaceAtArm < 0 || inSpaceNow < 0) {
        PLOG_WARN(
            "[PULSAR] [L.7 V15.3] transition flip-check failed: "
            "atArm={} now={} - skipping (probe failure)"sv,
            inSpaceAtArm, inSpaceNow);
        return;
    }

    if (inSpaceAtArm == inSpaceNow) {
        // L.7 1.5 - if a PFT arrival was detected recently, the Landing
        // event is REAL even though inSpace didn't flip (host stayed in
        // cockpit during landing → ship interior cell's worldspace lags
        // behind the actual planet/space transition).
        const bool isLanding =
            reason && std::strcmp(reason, "Landing") == 0;
        const bool isTakeOff =
            reason && std::strcmp(reason, "TakeOff") == 0;
        const std::uint64_t pftArmed =
            g_lastPFTArmedMs.load(std::memory_order_acquire);
        const bool pftRecent =
            (pftArmed != 0) && (now - pftArmed < kPFTRecentLandingMs);
        if (isLanding && pftRecent) {
            PLOG_INFO(
                "[PULSAR] [L.7 1.5] Landing REAL via PFT-recent gate "
                "(inSpace stayed {}, PFT armed {}ms ago). Firing save-bridge."sv,
                inSpaceNow, now - pftArmed);
            NASLOG("L.7 1.5 Landing REAL via PFT-recent age=%llu",
                   now - pftArmed);
            FireSaveBridge(reason, /*a_triggerOwnSave=*/true);
            return;
        }
        // L.7 1.6 - TakeOff has no analog of PFT (no planet to detect at
        // departure), and IsInSpace stays 1 the whole time the host is in
        // cockpit (interior cell → space worldspace). The only signal we
        // have is the SpaceshipSink<TakeOff> event itself. False positives
        // (door/POI activations firing spurious TakeOff) cost one extra
        // save-bridge, gated by the 15s cooldown - much cheaper than the
        // current "miss every real TakeOff while host stays in cockpit"
        // failure mode. Fire unconditionally on TakeOff post-settle.
        if (isTakeOff) {
            PLOG_INFO(
                "[PULSAR] [L.7 1.6] TakeOff REAL (unconditional fire - "
                "inSpace unchanged is the normal in-cockpit case). "
                "Firing save-bridge."sv);
            NASLOG("L.7 1.6 TakeOff REAL unconditional");
            FireSaveBridge(reason, /*a_triggerOwnSave=*/true);
            return;
        }

        PLOG_INFO(
            "[PULSAR] [L.7 V15.3] {} SPURIOUS - inSpace unchanged "
            "(arm={}, now={}) post-settle. Engine fired event but "
            "no real surface↔space transition. Skipping save-bridge."sv,
            reason, inSpaceAtArm, inSpaceNow);
        NASLOG("V15.3 transition SPURIOUS reason=%s inSpace=%d",
               reason, inSpaceNow);
        return;
    }

    PLOG_INFO(
        "[PULSAR] [L.7 V15.3] {} REAL - inSpace flip {}→{} confirmed "
        "post-settle. Firing save-bridge."sv,
        reason, inSpaceAtArm, inSpaceNow);
    NASLOG("V15.3 transition REAL reason=%s flip %d->%d",
           reason, inSpaceAtArm, inSpaceNow);
    FireSaveBridge(reason, /*a_triggerOwnSave=*/true);
#endif
}

// L.7 V14 - public unified save-bridge entry. Replaces inline trigger in
// L.7 1.4 - game-thread drain of pending-bridge state armed by FireSaveBridge
// when it was called while clientReady=false. Once the LOAD_COMPLETE ack
// arrives (g_clientLoadCompleted=true), the next tick re-fires the bridge.
// Must run on game thread because FireSaveBridge → BeginHostBridgeSaveTransfer
// touches engine save-state.
static void TickPendingBridgeRetry()
{
#if !PULSAR_L7_FT_BRIDGE_ENABLED
    return;
#else
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) return;

    if (!g_clientLoadCompleted.load(std::memory_order_acquire)) return;

    // L.7 1.10 - wait for cooldown to expire before draining a pending
    // bridge that was armed by cooldown-skip. Without this, the retry would
    // tight-loop every tick (drain → fire → cooldown skip → re-arm) for
    // the entire cooldown window, spamming logs.
    const std::uint64_t lastFire = g_lastBridgeFireMs.load(std::memory_order_acquire);
    const std::uint64_t nowEarly = ::GetTickCount64();
    if (lastFire != 0 && (nowEarly - lastFire) < kBridgeCooldownMs) return;

    PendingBridge pending{};
    {
        std::lock_guard lock(g_pendingBridgeMutex);
        if (!g_pendingBridge.valid) return;
        pending = g_pendingBridge;
        g_pendingBridge.valid = false;
    }

    const std::uint64_t age = ::GetTickCount64() - pending.armedMs;
    if (age > kPendingBridgeMaxAgeMs) {
        PLOG_INFO(
            "[PULSAR] [L.7 1.4] pending bridge dropped (reason={}, age={}ms > {}ms)"sv,
            pending.reason, age, kPendingBridgeMaxAgeMs);
        NASLOG("L.7 1.4 bridge pending dropped reason=%s age=%llu",
               pending.reason.c_str(), age);
        return;
    }

    PLOG_INFO(
        "[PULSAR] [L.7 1.4] retrying pending bridge (reason={}, age={}ms)"sv,
        pending.reason, age);
    NASLOG("L.7 1.4 bridge pending retry reason=%s age=%llu",
           pending.reason.c_str(), age);
    FireSaveBridge(pending.reason.c_str(), pending.triggerOwnSave);
#endif
}

// L.7 2.0 - engine-autosave-driven bridge checker. Replaces the entire
// SpaceshipSink-based detection (V15.3 inSpace flip, 1.5 PFT-recent gate,
// 1.6 unconditional TakeOff, 1.13 state stability, 1.14 engine autosave
// in TickPendingTransition). One single source of truth: when the engine
// writes a new .sfs AND the host's world key has changed since the last
// bridge, fire bridge using the engine's just-written save.
//
// World key = (IsInSpace, planetID). Captures the meaningful transitions:
// Cross-system grav-jump: planetID changes → fire
// Landing (host exits ship onto planet): IsInSpace 1→0 → fire
// TakeOff: IsInSpace 0→1 OR planetID changes → fire
// POI on same planet (host stays on surface): both unchanged → skip
// Manual quicksave: world key unchanged → skip
// Auto-save timer: world key unchanged → skip
//
// Trade-off: client follows host on Landing only after host disembarks
// (engine writes autosave at exit-ship). UX delay 10-20s post-touchdown,
// but 100% reliable: engine itself decides when transition is complete.
namespace autosave_checker_ {
    struct WorldKey {
        bool          inSpace = false;
        std::uint32_t planetID = 0;
        bool operator==(const WorldKey& o) const noexcept {
            return inSpace == o.inSpace && planetID == o.planetID;
        }
        bool operator!=(const WorldKey& o) const noexcept { return !(*this == o); }
    };

    static WorldKey ReadWorldKey()
    {
        WorldKey k{};
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return k;
        __try {
            k.inSpace = player->IsInSpace(true);
            RE::TESForm* planet = CallGetCurrentPlanet(player);
            k.planetID = planet ? planet->formID : 0u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return k;
    }

    // L.7 host-pilot-state check for autosave bridge gate. Reads
    // actor+0x37c bit 4 (= "in vehicle" per FUN_141aeb6f0 RE 2026-05-06).
    // Saves taken while this bit is set on host bake the pilot lock into
    // the .sfs and propagate to client on load. Empirically verified
    // (initial-connect uses pre-takeoff autosave with bit=0, GravJump
    // autosave fires while bit=1 → client-locked save). SEH-guarded.
    static bool IsHostPiloting_Impl()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        auto* aBytes = reinterpret_cast<std::uint8_t*>(player);
        const std::uint32_t a37c = *reinterpret_cast<std::uint32_t*>(aBytes + 0x37c);
        return ((a37c >> 4) & 1) != 0;
    }
    static bool IsHostPiloting()
    {
        __try { return IsHostPiloting_Impl(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static std::string g_lastSeenSavePath;
    static WorldKey    g_lastBridgedKey;
    static bool        g_baselineCaptured = false;

    // L.7 2.1 - explicit Landing tracking. Engine writes its own autosave
    // only AFTER the host disembarks (~10-20s post-touchdown), which is
    // poor UX for coop sync. Track Landing sink event + watch IsInSpace
    // 1→0 flip + small settle buffer, then we TriggerHostQuicksave to
    // force engine to write a save NOW. The 2.0 autosave-checker above
    // then picks up the new save on the next tick and fires the bridge
    // through the standard path.
    //
    // Risk: triggering quicksave during the landing cinematic crashes
    // the host (mid-state corruption observed 2026-05-05). The
    // kPostFlipSettleMs delay after IsInSpace=0 mitigates by waiting for
    // the engine to commit the landed state.
    // Cross-block referenced - forward-declared near top of file. No
    // `static` so the forward-decl + body share linkage. kLandingPostFlipMs
    // is defined inline in the forward-decl block.
    std::atomic<std::uint64_t> g_landingArmedMs{ 0 };
    std::atomic<int>           g_landingInSpaceAtArm{ -1 };
    std::atomic<std::uint64_t> g_landingFlipMs{ 0 };
    std::atomic<std::uint64_t> g_manualForceArmedMs{ 0 };
    std::atomic<std::uint64_t> g_recentSpaceEventMs{ 0 };
    constexpr std::uint64_t    kLandingMinSettleMs = 5000;   // ignore arm noise
    constexpr std::uint64_t    kLandingMaxWaitMs   = 60000;  // give up
}

// =====================================================================
// L.7 Landing-cinematic instrumentation
// =====================================================================
//
// Goal: find a reliable "cinematic-end / safe-to-serialize" engine signal
// so TriggerHostQuicksave can fire WITHOUT crashing the host. Two probes:
//
// (a) LandedSetEvent diag sink - engine fires this somewhere in the
//     landing pipeline; if it lines up with "player gets control" it's
//     the gate we want. Sink does nothing but NASLOG the relative timing
//     vs the Landing-armed timestamp. Non-broadcasting (diag only).
//
// (b) TickLandingDiagProbe - runs every game-tick while landing-armed,
//     throttled to 1Hz, captures and logs candidate fields. Compare
//     log line that matches "regained control" wall-clock to identify
//     which field flipped at that moment.
//
// Probed candidates (matches task description):
// Player actorState1 / actorState2 (raw uint32 - analyse bits later)
// PlayerCamera current state (kFlight / kShipAction / kFurniture / …)
// UI menus open: LoadingMenu / HUDMenu / SpaceshipHud / TakeoffMenu
// parentCell formID + cellFlags
// IsInSpace flag (already captured but bundled here for correlation)
//
// All reads SEH-guarded - landing cinematic is a fragile state.
namespace landing_diag_ {

    static std::atomic<std::uint64_t> g_landedSetFireMs{ 0 };
    static std::atomic<std::uint64_t> g_lastDiagLogMs { 0 };
    constexpr std::uint64_t           kDiagLogIntervalMs = 1000;

    // RVA verified 2026-05-06 via versionlib REL::ID(141815) ->
    // 0x2988550 (LandedSetEvent::GetEventSource lazy-init wrapper).
    constexpr std::uintptr_t kRVA_GetLandedSetSrc = 0x2988550;

    struct LandedSetDiagSink : public RE::BSTEventSink<RE::Spaceship::LandedSetEvent> {
        RE::BSEventNotifyControl ProcessEvent(
            const RE::Spaceship::LandedSetEvent& /*e*/,
            RE::BSTEventSource<RE::Spaceship::LandedSetEvent>* /*src*/) override
        {
            const auto now = ::GetTickCount64();
            g_landedSetFireMs.store(now, std::memory_order_release);
            const auto armed =
                autosave_checker_::g_landingArmedMs.load(std::memory_order_acquire);
            const std::uint64_t deltaMs = (armed != 0) ? (now - armed) : 0;
            PLOG_INFO(
                "[PULSAR] [L.7] LandedSetEvent fired (t+{}ms after Landing arm)"sv,
                deltaMs);
            NASLOG("L.7 LandedSetEvent t+%llums", deltaMs);
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    static LandedSetDiagSink g_landedSetSink;

    void RegisterLandedSetSink()
    {
        using GetFn = void* (__fastcall*)();
        auto getFn = reinterpret_cast<GetFn>(
            REL::Offset{ kRVA_GetLandedSetSrc }.address());
        void* raw = nullptr;
        __try { raw = getFn(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { raw = nullptr; }
        if (!raw) {
            PLOG_WARN("[PULSAR] [L.7] LandedSetEvent source null"sv);
            return;
        }
        auto* src = reinterpret_cast<
            RE::BSTEventSource<RE::Spaceship::LandedSetEvent>*>(raw);
        __try { src->RegisterSink(&g_landedSetSink); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] [L.7] LandedSetEvent RegisterSink threw"sv);
            return;
        }
        PLOG_INFO(
            "[PULSAR] [L.7] LandedSetEvent diag sink registered "
            "(src=0x{:X})"sv, reinterpret_cast<std::uintptr_t>(raw));
    }

    struct DiagSnap {
        std::uint32_t actorS1      = 0;
        std::uint32_t actorS2      = 0;
        std::uint32_t cellID       = 0;
        std::uint32_t cellFlags    = 0;
        std::uint32_t cameraState  = 0xFFFFFFFFu;  // sentinel = unknown
        std::uint32_t menuCount    = 0;
        bool          menusVisible = false;
        bool          loadingMenu  = false;
        bool          hudMenu      = false;
        bool          spaceshipHud = false;
        bool          takeoffMenu  = false;
        int           inSpace      = -1;
    };

    // BSFixedString temporaries inside this body have non-trivial dtors,
    // which forbid SEH (C2712). Wrap in CaptureDiagSafe below.
    static DiagSnap CaptureDiag_Impl()
    {
        DiagSnap s{};
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            s.actorS1 = player->actorState1;
            s.actorS2 = player->actorState2;
            if (player->parentCell) {
                s.cellID    = player->parentCell->formID;
                s.cellFlags = *reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uint8_t*>(player->parentCell) + 0x48);
            }
            s.inSpace = player->IsInSpace(true) ? 1 : 0;
        }
        if (auto* cam = RE::PlayerCamera::GetSingleton()) {
            for (std::uint32_t i = 0; i < RE::CameraState::kTotal; ++i) {
                if (cam->QCameraEquals(static_cast<RE::CameraState>(i))) {
                    s.cameraState = i;
                    break;
                }
            }
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            s.menusVisible = ui->IsMenusVisible();
            s.menuCount    = static_cast<std::uint32_t>(ui->menuStack.size());
            s.loadingMenu  = ui->IsMenuOpen("LoadingMenu");
            s.hudMenu      = ui->IsMenuOpen("HUDMenu");
            s.spaceshipHud = ui->IsMenuOpen("SpaceshipHud");
            s.takeoffMenu  = ui->IsMenuOpen("TakeoffMenu");
        }
        return s;
    }

    static DiagSnap CaptureDiagSafe()
    {
        DiagSnap s{};
        __try { s = CaptureDiag_Impl(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return s;
    }

    // L.7 diag-driven gate replacement for the old fixed-timer
    // post-flip wait. Empirical RE 2026-05-06 (marker-landing log):
    //   t+10.4s post-arm: inSpace flips 1→0, LoadingMenu opens
    //   t+16.6s post-arm: LoadingMenu closes (cell-stream complete)
    //   t+20.4s post-arm: old 10s-fixed timer fires - safe but late
    // LoadingMenu closed = engine has finished cell-streaming the planet
    // surface = player is controllable = TriggerHostQuicksave safe.
    bool IsSafeQuicksaveGate()
    {
        const auto s = CaptureDiagSafe();
        return !s.loadingMenu;
    }

    static const char* CamStr(std::uint32_t st)
    {
        switch (st) {
            case RE::CameraState::kFirstPerson:     return "FP";
            case RE::CameraState::kThirdPerson:     return "TP";
            case RE::CameraState::kFlight:          return "Flight";
            case RE::CameraState::kShipFarTravel:   return "ShipFarTravel";
            case RE::CameraState::kShipAction:      return "ShipAction";
            case RE::CameraState::kShipTargeting:   return "ShipTargeting";
            case RE::CameraState::kShipCombatOrbit: return "ShipCombatOrbit";
            case RE::CameraState::kFurniture:       return "Furniture";
            case RE::CameraState::kTween:           return "Tween";
            case RE::CameraState::kPCTransition:    return "PCTransition";
            case RE::CameraState::kFreeWalk:        return "FreeWalk";
            case RE::CameraState::kFreeAdvanced:    return "FreeAdvanced";
            case RE::CameraState::kFreeFly:         return "FreeFly";
            case RE::CameraState::kFreeTethered:    return "FreeTethered";
            case RE::CameraState::kBleedout:        return "Bleedout";
            case RE::CameraState::kAutoVanity:      return "AutoVanity";
            case RE::CameraState::kVATS:            return "VATS";
            case RE::CameraState::kIronSights:      return "IronSights";
            case RE::CameraState::kVehicle:         return "Vehicle";
            case RE::CameraState::kDialogue:        return "Dialogue";
            case RE::CameraState::kWorkshopIsometric: return "WorkshopIso";
            case RE::CameraState::kPhotoMode:       return "PhotoMode";
            default:                                return "?";
        }
    }
}

// L.7 runs from Tick(); throttled to 1Hz; only emits while
// Landing-tracker is armed. Captures all candidate fields so post-test
// log analysis can correlate field-flip with "player regained control"
// wall-clock time.
static void TickLandingDiagProbe()
{
#if !PULSAR_L7_FT_BRIDGE_ENABLED
    return;
#else
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) return;

    const auto armed =
        autosave_checker_::g_landingArmedMs.load(std::memory_order_acquire);
    if (armed == 0) return;

    const auto now = ::GetTickCount64();
    const auto last =
        landing_diag_::g_lastDiagLogMs.load(std::memory_order_acquire);
    if (now - last < landing_diag_::kDiagLogIntervalMs) return;
    landing_diag_::g_lastDiagLogMs.store(now, std::memory_order_release);

    const auto s = landing_diag_::CaptureDiagSafe();
    PLOG_INFO(
        "[PULSAR] [L.7] t+{}ms inSpace={} cell=0x{:08X} flags=0x{:08X} "
        "actS1=0x{:08X} actS2=0x{:08X} cam={}({}) menus={}/{} "
        "[LD={} HUD={} SH={} TKO={}]"sv,
        now - armed, s.inSpace, s.cellID, s.cellFlags,
        s.actorS1, s.actorS2, s.cameraState, landing_diag_::CamStr(s.cameraState),
        s.menuCount, s.menusVisible ? 1 : 0,
        s.loadingMenu ? 1 : 0, s.hudMenu ? 1 : 0,
        s.spaceshipHud ? 1 : 0, s.takeoffMenu ? 1 : 0);
    NASLOG(
        "L.7 t+%llu inSpc=%d cell=%X flags=%X actS1=%X actS2=%X "
        "cam=%s(%u) menus=%u/%d LD=%d HUD=%d SH=%d TKO=%d",
        now - armed, s.inSpace, s.cellID, s.cellFlags,
        s.actorS1, s.actorS2,
        landing_diag_::CamStr(s.cameraState), s.cameraState,
        s.menuCount, s.menusVisible ? 1 : 0,
        s.loadingMenu ? 1 : 0, s.hudMenu ? 1 : 0,
        s.spaceshipHud ? 1 : 0, s.takeoffMenu ? 1 : 0);
#endif
}

// L.7 2.1 - explicit Landing tracker. Forces an engine save once the host
// has actually landed (IsInSpace flipped 1→0) plus a settle window, so
// client follows shortly after touchdown instead of waiting for the
// host-exits-ship engine autosave (10-20s later). After TriggerHostQuicksave
// fires, the next TickAutosaveChecker tick picks up the new save and
// dispatches the bridge through the normal 2.0 path.
static void TickLandingTracker()
{
#if !PULSAR_L7_FT_BRIDGE_ENABLED
    return;
#else
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) {
        autosave_checker_::g_landingArmedMs.store(0, std::memory_order_release);
        return;
    }
    const auto armed = autosave_checker_::g_landingArmedMs.load(std::memory_order_acquire);
    if (armed == 0) return;
    const auto now = ::GetTickCount64();

    // Wait minimum settle so the sink-time noise (multiple Landing sinks
    // fire across cinematic phases) doesn't immediately fire.
    if (now - armed < autosave_checker_::kLandingMinSettleMs) return;

    const int inSpaceAtArm =
        autosave_checker_::g_landingInSpaceAtArm.load(std::memory_order_acquire);
    const int inSpaceNow = detail_v142_::CheckInSpaceSafe();
    if (inSpaceAtArm < 0 || inSpaceNow < 0) {
        if (now - armed > autosave_checker_::kLandingMaxWaitMs) {
            autosave_checker_::g_landingArmedMs.store(0, std::memory_order_release);
            PLOG_INFO("[PULSAR] [L.7 2.1] Landing tracker abort - probe failure"sv);
        }
        return;
    }

    // For Landing we wait for IsInSpace 1→0 flip. If we armed at 0 already,
    // host was already on planet - likely a redundant sink, abort.
    if (inSpaceAtArm == 0) {
        autosave_checker_::g_landingArmedMs.store(0, std::memory_order_release);
        PLOG_INFO("[PULSAR] [L.7 2.1] Landing tracker disarmed - armed inSpace=0 (redundant)"sv);
        return;
    }

    if (inSpaceNow != 0) {
        // Still in space; cinematic running.
        if (now - armed > autosave_checker_::kLandingMaxWaitMs) {
            autosave_checker_::g_landingArmedMs.store(0, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] [L.7 2.1] Landing tracker abort - inSpace never flipped after {}ms"sv,
                now - armed);
        }
        return;
    }

    // inSpace flipped 1→0 - record the moment if not already.
    auto flipMs = autosave_checker_::g_landingFlipMs.load(std::memory_order_acquire);
    if (flipMs == 0) {
        autosave_checker_::g_landingFlipMs.store(now, std::memory_order_release);
        flipMs = now;
        PLOG_INFO(
            "[PULSAR] [L.7 2.1] Landing tracker - inSpace 1→0 detected, "
            "waiting min {}ms + LoadingMenu close (max {}ms) before quicksave"sv,
            autosave_checker_::kLandingMinPostFlipMs,
            autosave_checker_::kLandingMaxPostFlipMs);
    }

    // L.7 event-gated quicksave trigger. Empirical RE 2026-05-06
    // shows the engine opens LoadingMenu at the inSpace flip and closes
    // it once the planet-surface cell-stream is complete (= player is
    // controllable). LoadingMenu==0 is therefore the safe-to-serialize
    // signal. Replaces the old fixed kLandingPostFlipMs=10000ms timer
    // which was non-deterministic (could fire mid-stream on slow loads).
    const auto postFlip = now - flipMs;

    // Floor: always wait at least kLandingMinPostFlipMs so any LoadingMenu
    // that's about to open has time to register. Otherwise on fast loads
    // we'd fire before the menu even appears.
    if (postFlip < autosave_checker_::kLandingMinPostFlipMs) return;

    const bool gateOk = landing_diag_::IsSafeQuicksaveGate();

    // Ceiling: failsafe so we don't hang forever if the gate never
    // closes (engine quirk, unusual landing path). Fire with a warning.
    const bool failsafe = !gateOk &&
        (postFlip >= autosave_checker_::kLandingMaxPostFlipMs);

    if (!gateOk && !failsafe) return;  // wait for LoadingMenu to close

    // Disarm BEFORE triggering save so we don't double-fire.
    autosave_checker_::g_landingArmedMs.store(0, std::memory_order_release);
    autosave_checker_::g_landingFlipMs.store(0, std::memory_order_release);

    if (failsafe) {
        PLOG_WARN(
            "[PULSAR] [L.7 2.1] Landing tracker FAILSAFE fire - "
            "LoadingMenu still open after {}ms post-flip; triggering "
            "host autosave anyway (gate unreliable)"sv, postFlip);
        NASLOG("L.7 2.1 Landing tracker FAILSAFE fire post-flip=%llu", postFlip);
    } else {
        PLOG_INFO(
            "[PULSAR] [L.7 2.1] Landing tracker fire - gate OK "
            "(LoadingMenu closed) at {}ms post-flip, triggering host "
            "autosave (autosave-checker will pick it up)"sv, postFlip);
        NASLOG("L.7 2.1 Landing tracker fire post-flip=%llu (gate OK)", postFlip);
    }
    // L.7 non-blocking bit-OR autosave (bits 0x1+0x8). The synchron-
    // ous CustomSaveDirect path was tested earlier and freezes the host
    // when the engine's save-lock is engaged (BSService never completes).
    // Bit-OR no-ops silently if the lock blocks it - better than a freeze.
    // For cached-planet landings where neither path writes a file, user
    // either disembarks (engine's natural autosave fires) or presses the
    // manual-bridge hotkey (default F8) to retry.
    Pulsar::WorldSync::TriggerHostAutoSave();
#endif
}

static void TickAutosaveChecker()
{
#if !PULSAR_L7_FT_BRIDGE_ENABLED
    return;
#else
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) {
        autosave_checker_::g_baselineCaptured = false;
        return;
    }

    // Pre-first-ack: keep updating baseline so we never fire on cell-init
    // artifacts that happen during the host save-load before/during connect.
    // The handshake's initial save transfer covers the connect-time state.
    if (!g_firstAckReceived.load(std::memory_order_acquire)) {
        autosave_checker_::g_lastSeenSavePath =
            Pulsar::SaveScanner::GetMostRecentSavePath();
        autosave_checker_::g_lastBridgedKey =
            autosave_checker_::ReadWorldKey();
        autosave_checker_::g_baselineCaptured = true;
        return;
    }

    // Wait for client to ack the previous load before considering new
    // bridges. FireSaveBridge would also gate this, but we save the work.
    if (!g_clientLoadCompleted.load(std::memory_order_acquire)) return;

    if (!autosave_checker_::g_baselineCaptured) {
        autosave_checker_::g_lastSeenSavePath =
            Pulsar::SaveScanner::GetMostRecentSavePath();
        autosave_checker_::g_lastBridgedKey =
            autosave_checker_::ReadWorldKey();
        autosave_checker_::g_baselineCaptured = true;
        return;
    }

    // L.7 manual-force timeout sweep (runs even when no new save):
    // if F8 was pressed but no save landed within the timeout window,
    // log + clear the flag so a stale arming doesn't bypass the world-key
    // filter forever.
    {
        const auto armed = autosave_checker_::g_manualForceArmedMs.load(
            std::memory_order_acquire);
        if (armed != 0) {
            const auto now = ::GetTickCount64();
            if (now - armed > autosave_checker_::kManualForceTimeoutMs) {
                autosave_checker_::g_manualForceArmedMs.store(
                    0, std::memory_order_release);
                PLOG_WARN(
                    "[PULSAR] [L.7 MANUAL] timeout - no new save "
                    "appeared within {}ms; engine likely refused the "
                    "save (cell/state lock). Try again or move first."sv,
                    autosave_checker_::kManualForceTimeoutMs);
                NASLOG("L.7 MANUAL timeout - no save written");
            }
        }
    }

    const std::string newest = Pulsar::SaveScanner::GetMostRecentSavePath();
    if (newest.empty() || newest == autosave_checker_::g_lastSeenSavePath) {
        return;  // no new save written
    }

    // Engine wrote a new save. Always update lastSeenSavePath so we don't
    // re-detect the same file.
    const std::string oldSeen = autosave_checker_::g_lastSeenSavePath;
    autosave_checker_::g_lastSeenSavePath = newest;

    const auto curKey = autosave_checker_::ReadWorldKey();

    // L.7 manual-force fast-path: if F8 armed within the timeout
    // window, fire the bridge regardless of world-key change. Without
    // this, F8 on the same planet as the client gets filtered out below.
    const auto manualArmed = autosave_checker_::g_manualForceArmedMs.load(
        std::memory_order_acquire);
    if (manualArmed != 0) {
        autosave_checker_::g_manualForceArmedMs.store(
            0, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] [L.7 MANUAL] new save detected ({}) - "
            "force-firing bridge (bypassing world-key filter, "
            "inSpace={}, planet=0x{:08X})"sv,
            newest, curKey.inSpace ? 1 : 0, curKey.planetID);
        NASLOG("L.7 MANUAL force-fire bridge inSpace=%d planet=0x%08X",
               curKey.inSpace ? 1 : 0, curKey.planetID);
        autosave_checker_::g_lastBridgedKey = curKey;
        FireSaveBridge("manual-force", /*a_triggerOwnSave=*/false);
        return;
    }

    // L.7 PARK (2026-05-25) - do NOT auto-bridge the client while the HOST
    // is in space. Force-loading a save into the client mid-space-transition is
    // a HARD engine crash (BSReadWriteLock-on-null, Ghidra 0x1422CC350 - the D2/
    // D3/takeoff/dock CTDs all trace here). The save-bridge is correct for
    // GROUND co-location and fundamentally wrong for SPACE. Space-follow is
    // DEFERRED (to be rebuilt as "each player in own ship" without save-loads).
    // Ground join + item sync + landing/ground arrivals (curKey.inSpace==false)
    // still bridge. F8 manual-force (above) bypasses this; initial join is a
    // separate handshake path.
    if (curKey.inSpace) {
        PLOG_INFO(
            "[PULSAR] [L.7 PARK] host in space - space-follow bridge "
            "DISABLED (deferred; ground co-op intact). Skipping."sv);
        NASLOG("L.7 PARK skip space-follow bridge");
        autosave_checker_::g_lastBridgedKey = curKey;  // ack so a later landing re-fires
        autosave_checker_::g_recentSpaceEventMs.store(0, std::memory_order_release);
        return;
    }

    if (curKey == autosave_checker_::g_lastBridgedKey) {
        // (2026-06-24) - REMOVED the "recent space-event" fast-fire here.
        // It was meant for orbit-to-orbit POI travel (both inSpace=1, same
        // planetID), but the PARK guard above now returns for inSpace==true,
        // so this same-world-key branch only ever runs for GROUND states
        // (inSpace==false). There, a STALE space-event flag (e.g. from a landing
        // ~21s earlier) was falsely force-firing a bridge when the host merely
        // WALKED THROUGH THE SHIP HATCH onto/off a planet. Boarding via the hatch
        // needs NO save-load - both players just walk through the hatch - and
        // force-loading that boarding autosave on the client CTD'd it (dangling
        // slim-lock, 0x1422CC350 family). So on an unchanged world key: never
        // bridge; clear the flag and skip.
        autosave_checker_::g_recentSpaceEventMs.store(0, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] [L.7 2.0] new save detected ({}) - world key unchanged "
            "(inSpace={}, planet=0x{:08X}); ground POI / hatch-board - NO bridge "
            "[]"sv,
            newest, curKey.inSpace ? 1 : 0, curKey.planetID);
        NASLOG("L.7 2.0 autosave skip same-world (no fast-fire) inSpace=%d planet=0x%08X",
               curKey.inSpace ? 1 : 0, curKey.planetID);
        return;
    }

    // #docking - even with a CHANGED world key, skip the auto-bridge if the CLIENT is
    // ALREADY in the host's current cell. Case: host walks through a hatch into a
    // STATION (new world key, so the unchanged-key guard above misses it), but the
    // client already walked through the same hatch and sits in the same cell. Bridging
    // then reloads the client into the cell it's already in = double-load (test
    // 2026-06-25: client cell 0x0100849E, host transit → redundant reload). Host knows
    // the client's cell via PKT_POSITION (SetRemoteCellID). Manual F8 returned above,
    // so this only gates the auto path; re-evaluated each autosave, so a later divergence
    // still bridges.
    {
        std::uint32_t hostCell = 0;
        if (auto* p = RE::PlayerCharacter::GetSingleton(); p && p->parentCell)
            hostCell = p->parentCell->formID;
        const std::uint32_t clientCell =
            Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteCellID();
        if (hostCell != 0 && clientCell != 0 && hostCell == clientCell) {
            PLOG_INFO(
                "[PULSAR] [docking] autosave bridge SKIP - client already co-located "
                "in host cell 0x{:08X} (hatch walk-through, no double-load)"sv,
                hostCell);
            NASLOG("docking skip bridge co-located cell=0x%08X", hostCell);
            return;
        }
    }

    // L.7 host-pilot gate REVERTED 13:45 - interfered with normal
    // L.3 V4 pilot handover. Falling back to direct fire.

    PLOG_INFO(
        "[PULSAR] [L.7 2.0] engine autosaved + world key changed "
        "(inSpace {}→{}, planet 0x{:08X}→0x{:08X}) - firing bridge with new save"sv,
        autosave_checker_::g_lastBridgedKey.inSpace ? 1 : 0,
        curKey.inSpace ? 1 : 0,
        autosave_checker_::g_lastBridgedKey.planetID,
        curKey.planetID);
    NASLOG("L.7 2.0 autosave bridge inSpace=%d→%d planet=0x%08X→0x%08X",
           autosave_checker_::g_lastBridgedKey.inSpace ? 1 : 0,
           curKey.inSpace ? 1 : 0,
           autosave_checker_::g_lastBridgedKey.planetID,
           curKey.planetID);

    // L.7 normal world-key-changed bridge consumed the recent
    // space-event flag too. Without clearing here, a follow-up timer-
    // autosave within 30s would falsely match the in-space POI fast-path
    // (world key matches the just-bridged key + flag still armed) and
    // fire a redundant second bridge.
    autosave_checker_::g_recentSpaceEventMs.store(
        0, std::memory_order_release);
    autosave_checker_::g_lastBridgedKey = curKey;
    // triggerOwnSave=false → BeginHostBridgeSaveTransfer polls newest .sfs
    // (= the engine's save we just detected). No additional quicksave
    // triggered, so no risk of mid-cinematic state corruption.
    FireSaveBridge("autosave-detected", /*a_triggerOwnSave=*/false);
#endif
}

// TickHostPFTDeferredSend; reusable from SpaceshipSink<TakeOff/Landing/Dock>.
bool FireSaveBridge(const char* a_reason, bool a_triggerOwnSave)
{
#if !PULSAR_L7_FT_BRIDGE_ENABLED
    (void)a_reason; (void)a_triggerOwnSave;
    return false;
#else
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) return false;

    const auto now = ::GetTickCount64();

    const bool clientReady =
        g_clientLoadCompleted.load(std::memory_order_acquire);
    const std::uint64_t lastFire =
        g_lastBridgeFireMs.load(std::memory_order_acquire);
    const bool cooldownOk =
        (lastFire == 0) || ((now - lastFire) >= kBridgeCooldownMs);

    if (!clientReady) {
        // L.7 1.4 - record pending fire so we can retry once the ack
        // arrives. Latest reason wins (TakeOff arming after PFT-arrival
        // is fine - the most recent transition is what matters).
        {
            std::lock_guard lock(g_pendingBridgeMutex);
            g_pendingBridge.reason         = a_reason ? a_reason : "<unknown>";
            g_pendingBridge.triggerOwnSave = a_triggerOwnSave;
            g_pendingBridge.armedMs        = now;
            g_pendingBridge.valid          = true;
        }
        PLOG_INFO(
            "[PULSAR] [L.7 V14] save-bridge skipped (reason={}) - "
            "waiting for client load-complete ack (pending-retry armed)"sv,
            a_reason);
        NASLOG("L.7 1.4 bridge pending reason=%s", a_reason);
        return false;
    }
    if (!cooldownOk) {
        // L.7 1.10 - arm pending for retry after cooldown expires. Without
        // this, a second transition within the 15s cooldown window (typical
        // for short-haul TakeOff→Landing via star map) gets dropped on the
        // floor. TickPendingBridgeRetry re-fires once cooldown allows.
        {
            std::lock_guard lock(g_pendingBridgeMutex);
            g_pendingBridge.reason         = a_reason ? a_reason : "<unknown>";
            g_pendingBridge.triggerOwnSave = a_triggerOwnSave;
            g_pendingBridge.armedMs        = now;
            g_pendingBridge.valid          = true;
        }
        PLOG_INFO(
            "[PULSAR] [L.7 V14] save-bridge skipped (reason={}) - "
            "cooldown {}ms remaining (pending-retry armed)"sv,
            a_reason, kBridgeCooldownMs - (now - lastFire));
        NASLOG("L.7 1.10 bridge pending (cooldown) reason=%s remaining=%llu",
               a_reason ? a_reason : "?", kBridgeCooldownMs - (now - lastFire));
        return false;
    }

    // L.7 1.14.1 - DON'T send PKT_DOCK_FORWARD_BRIDGE_EXPECTED. That packet
    // is for the L.6 dock-bridge return flow; client's DockSync handler
    // calls SuppressNextResetTeleport on receipt. For L.7 fast-travel
    // bridges that suppression breaks's strategy-driven restore
    // (ResetTeleport stays armed → m_clientTeleported never reset →
    // PKT_POSITION can't trigger the apply path → client lands at engine's
    // raw post-load pos = host's pilot seat). Same root cause as 1.14
    // direct-suppress removal - also need to stop the indirect suppress.

    if (a_triggerOwnSave) {
        // TakeOff/Landing/Dock path: engine doesn't auto-save on these,
        // so we trigger our own quicksave first via bit-0x8 in
        // mgr+0x50. The polling logic in BeginHostBridgeSaveTransfer
        // (started right after this) waits for the new file to settle.
        PLOG_INFO(
            "[PULSAR] [L.7 V14] save-bridge fire (reason={}) - "
            "TriggerHostQuicksave first"sv, a_reason);
        Pulsar::WorldSync::TriggerHostQuicksave();
    } else {
        PLOG_INFO(
            "[PULSAR] [L.7 V14] save-bridge fire (reason={}) - "
            "polling existing newest .sfs (engine auto-saved)"sv,
            a_reason);
    }

    // L.7 1.14 - DON'T suppress the next ResetTeleport for bridge saves.
    // Pre-, suppress was needed because the bridge mechanism left the
    // client at host's saved position (and we didn't want to teleport
    // again). With/'s strategy-driven restore (kNone same-cell-skip
    // or kOffsetBehindHost cross-cell), we explicitly WANT the post-LOADED
    // apply path to run so the client either lands behind host (cross-cell)
    // or stays put for the user to manually unseat (same-cell). The V13g
    // packet-barrier already drops PKT_POSITION packets during the save-
    // load window, so there's no risk of stale-pos pending.
    net.BeginHostBridgeSaveTransfer("PULSAR_L7_Bridge");

    g_clientLoadCompleted.store(false, std::memory_order_release);
    g_lastBridgeFireMs.store(now, std::memory_order_release);
    // L.7 1.7 - record reason for ArmTransitionFire dedup.
    g_lastFiredReason = a_reason;
    g_lastFiredReasonMs.store(now, std::memory_order_release);
    NASLOG("L.7 V14 bridge fired reason=%s trig=%d", a_reason,
           a_triggerOwnSave ? 1 : 0);
    return true;
#endif
}

}  // namespace Pulsar::SpaceSync::FastTravel
