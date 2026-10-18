#include "PCH.h"
#include "TransitSync/TransitSync.h"

#include "Cell3DAttach.h"
#include "CellContentSync/CellContentSync.h"
#include "Config.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "Coop/DivergenceWatch.h"
#include "NASLog.h"

#include <RE/B/BSFixedString.h>
#include <RE/B/BSScriptUtil.h>
#include <RE/G/GameVM.h>
#include <RE/N/NiPoint.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::TransitSync {
namespace {

    // Suppress duplicate transit broadcasts within this window. The CellDiag
    // observation can fire twice (parentCell goes A -> 0 -> B during load),
    // and we don't want to broadcast the intermediate (cell=0) step.
    constexpr std::uint64_t kDuplicateSuppressMs = 500;

    // ---- Sender state ---------------------------------------------------
    static std::atomic<std::uint64_t> g_lastSendTick{ 0 };
    static std::atomic<std::uint32_t> g_lastSendDestCell{ 0 };

    // ---- Receiver state -------------------------------------------------
    struct PendingTransit {
        Network::TransitData data;
    };
    static std::mutex            g_recvMutex;
    static std::vector<PendingTransit> g_recvQueue;
    static std::atomic<std::uint32_t> g_lastPeerDestCell{ 0 };
    static std::atomic<std::uint32_t> g_lastPeerDestWorldspace{ 0 };

    // ---- K.2 Auto-Follow state -----------------------------------------
    static std::atomic<bool> g_followEnabled{ true };
    // Pending follow destination - set when peer's transit arrives, drained
    // by TickFollowApply on the game thread.
    struct PendingFollow {
        std::atomic<bool>          armed{ false };
        std::atomic<float>         x{ 0.0f }, y{ 0.0f }, z{ 0.0f }, rotZ{ 0.0f };
        std::atomic<std::uint32_t> destCell{ 0 };
        std::atomic<std::uint32_t> sourceDoor{ 0 };
        std::atomic<std::uint8_t>  isInterior{ 0 };
    };
    static PendingFollow g_pendingFollow;
    // Hotkey edge-detect for F7.
    static bool g_lastF7Down = false;

    // auto co-location. When AutoFollow has to SKIP a teleport because it
    // would cross a procedural-instance / worldspace boundary, the client asks
    // the host to save-bridge it instead (PKT_BRIDGE_REQUEST). Client-side
    // cooldown collapses the duplicate transit broadcasts of one fast-travel
    // into a single request. F8 remains the manual fallback.
    static std::atomic<std::uint64_t> g_lastBridgeReqSentMs{ 0 };
    constexpr std::uint64_t kBridgeReqCooldownMs = 20000;

    static void RequestHostBridge(const char* a_why)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected() || net.IsHost()) return;  // client → host only
        const std::uint64_t now  = ::GetTickCount64();
        const std::uint64_t last = g_lastBridgeReqSentMs.load(std::memory_order_acquire);
        if (last != 0 && now - last < kBridgeReqCooldownMs) return;  // cooldown
        g_lastBridgeReqSentMs.store(now, std::memory_order_release);

        std::uint8_t buf[8]{};
        Pulsar::Network::PacketWriter w(Pulsar::Network::PKT_BRIDGE_REQUEST,
                                        buf, sizeof(buf));
        net.Send(w.Data(), w.Size(), /*reliable*/ true);
        PLOG_INFO(
            "[PULSAR] AutoFollow: requested host bridge for auto co-location ({})"sv,
            a_why);
        NASLOG("AutoFollow: bridge-request sent (%s)", a_why);
    }

    // Most-recently locally activated door + tick. Captured by LootSync's
    // HookedActivate; consumed by OnLocalTransit when a transit fires
    // shortly after. 30s window - verified that activation→loadMenu
    // CLOSED can take 2..8s (UI prompts, slow loads). The door is
    // CLEARED inside ConsumeRecentDoorActivation, so a generous window
    // doesn't risk leaking old doors into unrelated later transits.
    constexpr std::uint64_t kDoorActivationWindowMs = 30000;
    static std::atomic<std::uint32_t> g_lastLocalDoor{ 0 };
    static std::atomic<std::uint64_t> g_lastLocalDoorTick{ 0 };

    // SEH-wrapped SetPosition + angle write. C2712 forbids __try in
    // functions with C++ unwinding objects (BSFixedString destructors etc.).
    static void SafeSetPlayerPos(RE::PlayerCharacter* a_player,
                                  float a_x, float a_y, float a_z, float a_rotZ) noexcept
    {
        if (!a_player) return;
        __try {
            RE::NiPoint3A target(a_x, a_y, a_z);
            a_player->SetPosition(target, true);
            a_player->data.angle.z = a_rotZ;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // K.2 V3 - direct C++ vt[0x54] Activate call on a door, with re-entry
    // guard so our own LootSync hook doesn't capture the resulting
    // door-activate as a fresh local transit-trigger.
    thread_local bool t_inAutoFollowActivate = false;

    // K.2 V5 (2026-04-27) - ExtraDataList::GetByType resolver. Same primitive
    // LockSync uses to read REFR_LOCK; we read ExtraTeleport (kTeleport=0x29)
    // from a door REFR to extract its destinationRef for cross-cell follow.
    using GetExtraByTypeFn = void* (__fastcall*)(void* extraList, std::uint32_t type);
    static GetExtraByTypeFn g_getExtraByType = nullptr;

    // Lazy-resolve at first use. REL::ID 37878 - verified in LockSync.
    static GetExtraByTypeFn ResolveGetExtraByType()
    {
        if (g_getExtraByType) return g_getExtraByType;
        g_getExtraByType = reinterpret_cast<GetExtraByTypeFn>(
            REL::Relocation<std::uintptr_t>{ REL::ID(37878) }.address());
        return g_getExtraByType;
    }

    // ExtraTeleport struct layout (verified RE 2026-04-27, see
    // tools/extra_teleport_re.txt):
    //   Outer wrapper (0x20 bytes):
    //     +0x00  vftable (REL::ID 398830 → 0x144B23910)
    //     +0x08  next (BSExtraData linked-list)
    //     +0x12  type byte = 0x29
    //     +0x18  REFR_TELEPORT*  data  ← pointer to inner block
    //
    //   REFR_TELEPORT inner (0x28 bytes):
    //     +0x00  TESObjectREFR*  destDoorRef   ← what we want
    //     +0x08  uint32          flags
    //     +0x0C  float[3]        destPos
    //     +0x18  float[3]        destRot
    //     +0x24  uint8           destFlag
    //
    // Access path: door+0xC8 → GetByType(0x29) → +0x18 → +0x00 → REFR*
    constexpr std::uint32_t kExtraType_Teleport = 0x29;

    // Diagnostic - lets caller log which step failed.
    enum class TeleportDestResult : std::uint8_t {
        kOK = 0,
        kNoExtraList,
        kNoExtraTeleport,
        kNoInnerBlock,
        kNoDestRef,
        kSEH,
    };

    // SEH-wrapped read of door's extraTeleport.destinationRef. Returns null
    // if door has no teleport-data, type-tag mismatch, or any read error.
    static RE::TESObjectREFR* SafeGetTeleportDest(RE::TESObjectREFR* a_door,
                                                    TeleportDestResult& outRes) noexcept
    {
        outRes = TeleportDestResult::kSEH;
        if (!a_door) { outRes = TeleportDestResult::kNoExtraList; return nullptr; }
        auto getFn = ResolveGetExtraByType();
        if (!getFn) { outRes = TeleportDestResult::kNoExtraList; return nullptr; }
        RE::TESObjectREFR* destRef = nullptr;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_door);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) { outRes = TeleportDestResult::kNoExtraList; return nullptr; }
            void* extraTele = getFn(extraList, kExtraType_Teleport);
            if (!extraTele) { outRes = TeleportDestResult::kNoExtraTeleport; return nullptr; }
            // Two-step deref: first +0x18 is REFR_TELEPORT*, then +0x00
            // of that struct is the dest door REFR pointer.
            void* innerBlock = *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(extraTele) + 0x18);
            if (!innerBlock) { outRes = TeleportDestResult::kNoInnerBlock; return nullptr; }
            destRef = *reinterpret_cast<RE::TESObjectREFR**>(innerBlock);
            if (!destRef) { outRes = TeleportDestResult::kNoDestRef; return nullptr; }
            outRes = TeleportDestResult::kOK;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            outRes = TeleportDestResult::kSEH;
            destRef = nullptr;
        }
        return destRef;
    }

    static const char* TeleportDestResultStr(TeleportDestResult r)
    {
        switch (r) {
            case TeleportDestResult::kOK: return "OK";
            case TeleportDestResult::kNoExtraList: return "no-extraList";
            case TeleportDestResult::kNoExtraTeleport: return "no-extraTeleport";
            case TeleportDestResult::kNoInnerBlock: return "no-innerBlock";
            case TeleportDestResult::kNoDestRef: return "no-destRef";
            case TeleportDestResult::kSEH: return "SEH";
        }
        return "?";
    }

    // SEH-wrapped Papyrus DispatchStaticCall to PulsarFurnSync.DoPlayerFollowMoveTo
    // calls Game.GetPlayer().MoveTo(destRef) which loads the dest cell
    // (engine handles interior cell-load, exterior cross-grid-stream).
    // MoveTo is non-latent in Bethesda games (not in our latent-list per
    // memory feedback_papyrus_latent_natives.md).
    static bool DispatchPlayerFollowMoveTo(RE::TESObjectREFR* a_destRef)
    {
        if (!a_destRef) return false;
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return false;
        auto* vm = gameVM->GetVM();
        if (!vm) return false;

        RE::BSScript::Variable destVar;
        RE::BSScript::PackVariable(destVar, a_destRef);

        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(1);
            args[0] = destVar;
            return true;
        };

        RE::BSFixedString objName("PulsarFurnSync");
        RE::BSFixedString funcName("DoPlayerFollowMoveTo");
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        return vm->DispatchStaticCall(objName, funcName, argFn, callback, 0);
    }

    static bool SafeActivateDoor(RE::TESObjectREFR* a_door,
                                  RE::TESObjectREFR* a_player) noexcept
    {
        if (!a_door || !a_player) return false;
        bool ok = false;
        t_inAutoFollowActivate = true;
        __try {
            RE::OBJ_ACTIVATE_DATA data{};
            data.activatedRef = a_door;
            data.actionRef    = a_player;
            data.objectToGet  = nullptr;
            data.count        = 0;
            data.unk1C        = false;
            data.unk1D        = false;
            data.unk1E        = false;
            data.unk1F        = false;
            ok = a_door->Activate(data);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        t_inAutoFollowActivate = false;
        return ok;
    }

    // Classify a transit based on cell-flag + worldspace-change observations.
    // Best-effort heuristic - not perfect, but distinguishes the major cases
    // we care about for downstream auto-follow logic.
    std::uint8_t Classify(std::uint32_t prevWS, std::uint32_t newWS,
                           bool destIsInterior)
    {
        // Worldspace change while exterior → ship landing or grav-jump.
        // Distinguishing the two from CellDiag alone is tricky; for K.1
        // we mark both as kShipLanding/kGravJump combined under kShipLanding
        // (= 2). K.2 can refine via UI menu state observation.
        if (prevWS != 0 && newWS != 0 && prevWS != newWS && !destIsInterior) {
            return 3;  // kShipLanding (placeholder; K.2 may split to kGravJump)
        }
        if (destIsInterior) {
            return 0;  // kInteriorEntry
        }
        // Exterior → exterior in same worldspace: could be fast-travel
        // (large position delta) or just grid-cell streaming. K.1 lumps as
        // kExteriorReturn. K.2 can compare distance to refine.
        return 1;  // kExteriorReturn
    }

    bool IsHostOrClientConnected()
    {
        return Pulsar::Network::GetSingleton().IsConnected();
    }

}  // namespace

// =====================================================================
// Public API
// =====================================================================

void Init()
{
    PLOG_INFO("[PULSAR] TransitSync K.1 init"sv);
}

void OnLocalTransit(std::uint32_t prevCellFormID,
                     std::uint32_t newCellFormID,
                     std::uint32_t prevWorldspaceFormID,
                     std::uint32_t newWorldspaceFormID,
                     float x, float y, float z, float rotZ,
                     bool destIsInterior)
{
    if (newCellFormID == 0) return;  // skip transit-to-null-cell intermediates
    if (!IsHostOrClientConnected()) return;

    // De-dupe within window - same dest cell.
    std::uint64_t now = ::GetTickCount64();
    std::uint32_t lastDest = g_lastSendDestCell.load(std::memory_order_acquire);
    std::uint64_t lastTick = g_lastSendTick.load(std::memory_order_acquire);
    if (lastDest == newCellFormID && (now - lastTick) < kDuplicateSuppressMs) {
        return;
    }
    g_lastSendDestCell.store(newCellFormID, std::memory_order_release);
    g_lastSendTick.store(now, std::memory_order_release);

    Pulsar::Network::TransitData td{};
    td.transitType       = Classify(prevWorldspaceFormID, newWorldspaceFormID,
                                     destIsInterior);
    td.isInterior        = destIsInterior ? 1 : 0;
    td.sourceCellFormID  = prevCellFormID;
    td.destCellFormID    = newCellFormID;
    td.worldspaceFormID  = newWorldspaceFormID;
    td.destX = x; td.destY = y; td.destZ = z;
    td.destRotZ = rotZ;
    td.sourceDoorFormID  = ConsumeRecentDoorActivation();

    // Reliable: must not be lost - receiver decides what to do based on it.
    std::uint8_t buf[64]{};
    Pulsar::Network::PacketWriter w(Pulsar::Network::PKT_TRANSIT, buf, sizeof(buf));
    w.Write(td);
    Pulsar::Network::GetSingleton().Send(w.Data(), w.Size(), true);

    PLOG_INFO(
        "[PULSAR] TransitSync SEND type={} src=0x{:08X} dst=0x{:08X} ws=0x{:08X} "
        "interior={} pos=({:.0f},{:.0f},{:.0f}) door=0x{:08X}"sv,
        td.transitType, td.sourceCellFormID, td.destCellFormID,
        td.worldspaceFormID, td.isInterior ? 1 : 0,
        td.destX, td.destY, td.destZ, td.sourceDoorFormID);
    NASLOG("TransitSync: SEND type=%u dst=0x%08X door=0x%08X int=%u",
           td.transitType, td.destCellFormID, td.sourceDoorFormID,
           td.isInterior ? 1u : 0u);

    // K.6 V0: enumerate the cell's REFRs after the transit broadcast. Logs
    // only - no batch packet yet. CellContentSync filters host-only itself.
    Pulsar::CellContentSync::OnHostCellChanged(prevCellFormID, newCellFormID);
}

void NoteLocalDoorActivation(std::uint32_t a_doorFormID)
{
    if (a_doorFormID == 0) return;
    // K.2 V3 - re-entry guard: while we're servicing peer's auto-follow by
    // calling Activate ourselves, our own LootSync hook fires on that same
    // door. Don't capture it as a fresh local transit - that would broadcast
    // back to peer and ping-loop.
    if (t_inAutoFollowActivate) return;
    g_lastLocalDoor.store(a_doorFormID, std::memory_order_release);
    g_lastLocalDoorTick.store(::GetTickCount64(), std::memory_order_release);
}

bool IsInAutoFollowActivate()
{
    return t_inAutoFollowActivate;
}

std::uint32_t ConsumeRecentDoorActivation()
{
    std::uint32_t door = g_lastLocalDoor.load(std::memory_order_acquire);
    if (door == 0) return 0;
    std::uint64_t doorTick = g_lastLocalDoorTick.load(std::memory_order_acquire);
    std::uint64_t now = ::GetTickCount64();
    if ((now - doorTick) > kDoorActivationWindowMs) {
        return 0;  // too stale, not the door that drove this transit
    }
    // Clear so we don't reuse the same door for an unrelated transit later.
    g_lastLocalDoor.store(0, std::memory_order_release);
    return door;
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    // PacketHeader (3 bytes: 1 type + 2 size) precedes payload at offset 0.
    if (!data || length < Pulsar::Network::HEADER_SIZE +
                            sizeof(Pulsar::Network::TransitData)) {
        PLOG_WARN("[PULSAR] TransitSync RECV: short packet ({} bytes)"sv, length);
        return;
    }
    Pulsar::Network::TransitData td{};
    std::memcpy(&td, data + Pulsar::Network::HEADER_SIZE, sizeof(td));

    {
        std::lock_guard lk(g_recvMutex);
        g_recvQueue.push_back({ td });
    }
    g_lastPeerDestCell.store(td.destCellFormID, std::memory_order_release);
    g_lastPeerDestWorldspace.store(td.worldspaceFormID, std::memory_order_release);
}

void Tick()
{
    // Game-thread drain. K.1 only logs.
    std::vector<PendingTransit> drained;
    {
        std::lock_guard lk(g_recvMutex);
        if (g_recvQueue.empty()) return;
        drained.swap(g_recvQueue);
    }

    for (const auto& p : drained) {
        PLOG_INFO(
            "[PULSAR] TransitSync RECV type={} src=0x{:08X} dst=0x{:08X} "
            "ws=0x{:08X} interior={} pos=({:.0f},{:.0f},{:.0f}) - peer transitioned"sv,
            p.data.transitType, p.data.sourceCellFormID, p.data.destCellFormID,
            p.data.worldspaceFormID, p.data.isInterior ? 1 : 0,
            p.data.destX, p.data.destY, p.data.destZ);
        NASLOG("TransitSync: RECV type=%u dst=0x%08X ws=0x%08X int=%u",
               p.data.transitType, p.data.destCellFormID,
               p.data.worldspaceFormID, p.data.isInterior ? 1u : 0u);

        // K.2 - arm pending auto-follow with the most recent peer dest.
        // The actual teleport happens in TickFollowApply on game thread.
        //
        // HOST never auto-follows: the host is the world anchor; the client
        // co-locates to the host via the bridge + PKT_POSITION teleport. If the
        // host follows the client's transits it gets teleported away whenever
        // the client transitions - most visibly during the client's JOIN/bridge
        // sequence, which broadcasts transits. Observed 2026-06-18 17:43:51:
        // host yanked from its outpost (cell 0xFF01C2E3 -499,363) to 0xFF013AE1
        // (-682,534) the moment the client joined ("host an den Cell-Rand
        // geschleudert"). Client still follows the host (default on).
        if (g_followEnabled.load(std::memory_order_acquire) &&
            !Pulsar::Network::GetSingleton().IsHost()) {
            g_pendingFollow.x.store(p.data.destX, std::memory_order_release);
            g_pendingFollow.y.store(p.data.destY, std::memory_order_release);
            g_pendingFollow.z.store(p.data.destZ, std::memory_order_release);
            g_pendingFollow.rotZ.store(p.data.destRotZ, std::memory_order_release);
            g_pendingFollow.destCell.store(p.data.destCellFormID,
                                            std::memory_order_release);
            g_pendingFollow.sourceDoor.store(p.data.sourceDoorFormID,
                                              std::memory_order_release);
            g_pendingFollow.isInterior.store(p.data.isInterior,
                                              std::memory_order_release);
            g_pendingFollow.armed.store(true, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] AutoFollow armed: dst=0x{:08X} pos=({:.0f},{:.0f},{:.0f}) door=0x{:08X}"sv,
                p.data.destCellFormID, p.data.destX, p.data.destY, p.data.destZ,
                p.data.sourceDoorFormID);
        }
    }
}

std::uint32_t GetLastPeerDestCellFormID()
{
    return g_lastPeerDestCell.load(std::memory_order_acquire);
}

std::uint32_t GetLastPeerDestWorldspaceFormID()
{
    return g_lastPeerDestWorldspace.load(std::memory_order_acquire);
}

// =====================================================================
// K.2 Auto-Follow API
// =====================================================================

bool IsFollowEnabled()
{
    return g_followEnabled.load(std::memory_order_acquire);
}

void SetFollow(bool a_on)
{
    bool prev = g_followEnabled.exchange(a_on, std::memory_order_acq_rel);
    if (prev != a_on) {
        PLOG_INFO("[PULSAR] AutoFollow: {} (was {})"sv,
                  a_on ? "ON" : "OFF", prev ? "ON" : "OFF");
        NASLOG("AutoFollow: %s", a_on ? "ON" : "OFF");
    }
}

void ToggleFollow()
{
    SetFollow(!g_followEnabled.load(std::memory_order_acquire));
}

void TickFollowHotkey(int a_vkey)
{
    // Edge-detect the configured key (default 0x76 = VK_F7). Polled from
    // the game-thread tick so double-fire is avoided naturally.
    bool down = (::GetAsyncKeyState(a_vkey) & 0x8000) != 0;
    if (down && !g_lastF7Down) {
        ToggleFollow();
    }
    g_lastF7Down = down;
}

void TickFollowApply()
{
    if (!g_pendingFollow.armed.load(std::memory_order_acquire)) return;

    // Don't teleport while we are mid-loadMenu / cell-stream - Cell3DAttach
    // tracks that. Wait for quiet, then apply.
    if (Pulsar::Cell3DAttach::IsQuiet() || Pulsar::Cell3DAttach::WasInLoadMenu()) {
        return;
    }

    // Disarm first so we don't re-fire even if the apply has hiccups.
    g_pendingFollow.armed.store(false, std::memory_order_release);

    if (!g_followEnabled.load(std::memory_order_acquire)) {
        // User toggled off between arm and apply - drop.
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    float x   = g_pendingFollow.x.load(std::memory_order_acquire);
    float y   = g_pendingFollow.y.load(std::memory_order_acquire);
    float z   = g_pendingFollow.z.load(std::memory_order_acquire);
    float rZ  = g_pendingFollow.rotZ.load(std::memory_order_acquire);
    std::uint32_t dst       = g_pendingFollow.destCell.load(std::memory_order_acquire);
    std::uint32_t doorForm  = g_pendingFollow.sourceDoor.load(std::memory_order_acquire);

    // K.2 V5 (2026-04-27) - preferred path: read door's ExtraTeleport,
    // extract destination door REFR (= door on the OTHER side), call
    // Player.MoveTo(destRef) via Papyrus shim. MoveTo handles:
    // exterior cell-grid streaming (engine moves player to dest pos)
    // interior cell-load (engine loads dest cell, places player at
    //     dest door's position)
    // MoveTo is NativeFunction6 (NOT latent) - verified RE.
    //
    // Skip-V5 when:
    // doorForm is 0 (transit was UI-driven, e.g. StarMap board-ship)
    // doorForm is FF-prefix (procedural door, per-machine layout -
    //     destDoorRef on our side points to OUR proc-cell's door, not peer's)
    bool doorIsProcedural = ((doorForm >> 24) & 0xFFu) == 0xFFu;
    if (doorForm != 0 && !doorIsProcedural) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* doorForm_ = RE::TESForm::LookupByID(doorForm);
        auto* doorRef = doorForm_ ? doorForm_->As<RE::TESObjectREFR>() : nullptr;
        if (player && doorRef) {
            TeleportDestResult tdr;
            auto* destRef = SafeGetTeleportDest(doorRef, tdr);
            if (destRef) {
                std::uint32_t destFormID = 0;
                __try { destFormID = *reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uint8_t*>(destRef) + 0x28); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                PLOG_INFO(
                    "[PULSAR] AutoFollow: V5 Player.MoveTo(destRef={:p} formID=0x{:08X}) door=0x{:08X} dst-cell=0x{:08X}"sv,
                    static_cast<void*>(destRef), destFormID, doorForm, dst);
                NASLOG("AutoFollow: V5 MoveTo door=0x%08X destForm=0x%08X",
                       doorForm, destFormID);
                bool ok = DispatchPlayerFollowMoveTo(destRef);
                if (ok) {
                    PLOG_INFO("[PULSAR] AutoFollow: V5 dispatch returned true - engine queued MoveToFunctor"sv);
                    return;  // engine handles cell-load + teleport on next frame
                }
                PLOG_WARN(
                    "[PULSAR] AutoFollow: V5 dispatch returned false - falling through"sv);
            } else {
                PLOG_INFO(
                    "[PULSAR] AutoFollow: V5 - door 0x{:08X} extraTeleport extract FAILED ({}), falling through"sv,
                    doorForm, TeleportDestResultStr(tdr));
            }
        } else {
            PLOG_WARN(
                "[PULSAR] AutoFollow: door 0x{:08X} not resolvable - fall through"sv,
                doorForm);
        }
    }

    // K.2 V6 (2026-04-27) - Worldspace-mismatch follow (takeoff / landing).
    // Takeoff and landing keep the player in the same cockpit cell
    // (e.g. 0x05100136) but change worldspace (planet ↔ space). Position-
    // sync alone makes the clones look like they're in same cell, but
    // engine-state diverges: host actually flying in space while client
    // still on planet docked.
    //
    // Detection: peer's worldspaceFormID (from PKT_TRANSIT) != our local
    // player's parentCell.cellWorldspace formID. If different and the
    // remote actor is a valid REFR, MoveTo it - engine swaps client to
    // peer's worldspace alongside the cell-load.
    {
        std::uint32_t peerWS = 0;
        // Recover peer worldspace from last transit (we stored it earlier).
        peerWS = g_lastPeerDestWorldspace.load(std::memory_order_acquire);
        auto* localPlayer = RE::PlayerCharacter::GetSingleton();
        std::uint32_t localWS = 0;
        if (localPlayer && localPlayer->parentCell &&
            localPlayer->parentCell->cellWorldspace)
        {
            localWS = localPlayer->parentCell->cellWorldspace->formID;
        }
        if (peerWS != 0 && localWS != 0 && peerWS != localWS) {
            // (2026-06-23) - do NOT follow the host INTO space on takeoff.
            // The takeoff signature is an interior dest reached via fast-travel
            // (ship cockpit, no door). Following it (here via clone MoveTo, or
            // the bridge further below) drags the client into space, where
            // the only co-location mechanism (save-load bridge) hits an engine
            // null-lock CTD and, once aborted, strands the client (every later
            // bridge aborts too - observed 2026-06-23: client never followed
            // back on landing/F8). Landing (exterior dest) is unaffected.
            const bool takeoffIntoSpace =
                g_pendingFollow.isInterior.load(std::memory_order_acquire) != 0 &&
                g_pendingFollow.sourceDoor.load(std::memory_order_acquire) == 0;
            if (!Pulsar::GetConfig().coopSpaceFollow && takeoffIntoSpace) {
                PLOG_INFO(
                    "[PULSAR] AutoFollow: V6 SKIP takeoff-into-space "
                    "(coopSpaceFollow=0) peerWS=0x{:08X} localWS=0x{:08X}"sv,
                    peerWS, localWS);
                NASLOG("AutoFollow: V6 skip takeoff-into-space");
                return;
            }
            auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager()
                              .GetRemoteActor();
            if (clone) {
                std::uint32_t cloneRefID = 0;
                __try {
                    cloneRefID = *reinterpret_cast<std::uint32_t*>(
                        reinterpret_cast<std::uint8_t*>(clone) + 0x28);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                PLOG_INFO(
                    "[PULSAR] AutoFollow: V6 worldspace-change peerWS=0x{:08X} "
                    "localWS=0x{:08X} → MoveTo clone refID=0x{:08X}"sv,
                    peerWS, localWS, cloneRefID);
                NASLOG("AutoFollow: V6 ws-change peerWS=0x%08X localWS=0x%08X",
                       peerWS, localWS);
                bool ok = DispatchPlayerFollowMoveTo(
                    reinterpret_cast<RE::TESObjectREFR*>(clone));
                if (ok) {
                    PLOG_INFO("[PULSAR] AutoFollow: V6 dispatch returned true"sv);
                    return;
                }
                PLOG_WARN("[PULSAR] AutoFollow: V6 dispatch returned false"sv);
            } else {
                PLOG_INFO(
                    "[PULSAR] AutoFollow: V6 ws-change but no clone available - fall through"sv);
            }
        }
    }

    // K.2 V1 fallback - SetPosition to peer's destination coordinates.
    //
    // Three SKIP conditions:
    //   1. Peer dest is INTERIOR - SetPosition can't load interior cells.
    //   2. WE are currently in an INTERIOR cell - SetPosition to outdoor
    //      coords does NOT unload the interior; the engine keeps us in the
    //      same cell at impossible coords → we fall through floor / out
    //      of bounds for ~30s until we walk to a door manually.
    //      (Verified 2026-04-27 22:26-22:27: client teleported from inside
    //      ship to outdoor coords, stayed in ship cell, fell through floor
    //      until user walked to door manually 32s later.)
    //   3. Peer dest is PROCEDURAL (FF-prefix) - coordinates are valid only
    //      inside peer's procedural-cell instance. SetPosition does not
    //      switch cells across procedural boundaries; we'd plop into our
    //      own different proc-cell at coords that mean nothing physically.
    //      (Verified 2026-04-28 18:24: host cell=0xFF0195AE, client cell=
    //      0xFF0196AD, both at host-coords (-557,389,9) but in different
    //      proc-instances - sahen sich NICHT.)
    //
    // SetPosition only safely works when BOTH sides are exterior AND the
    // dest cell is a static (non-FF) FormID.
    bool destIsInterior = g_pendingFollow.isInterior.load(std::memory_order_acquire) != 0;
    bool destIsProcedural = ((dst >> 24) & 0xFFu) == 0xFFu;
    bool localIsInterior   = false;
    bool localIsProcedural = false;   ///our parentCell is an FF proc instance
    std::uint32_t localCellID = 0;
    std::uint32_t localWS      = 0;
    {
        auto* localPlayer = RE::PlayerCharacter::GetSingleton();
        if (localPlayer && localPlayer->parentCell) {
            localIsInterior   = localPlayer->parentCell->IsInterior();
            localCellID       = localPlayer->parentCell->formID;
            localIsProcedural = ((localCellID >> 24) & 0xFFu) == 0xFFu;
            if (localPlayer->parentCell->cellWorldspace) {
                localWS = localPlayer->parentCell->cellWorldspace->formID;
            }
        }
    }
    std::uint32_t peerWS = g_lastPeerDestWorldspace.load(std::memory_order_acquire);
    if (destIsInterior) {
        // (2026-06-21): an interior reached via a DOOR (srcDoor != 0) is
        // walkable - the client can use the same door (and V5 MoveTo already
        // tried it above). But an interior reached via FAST-TRAVEL/teleport
        // (srcDoor == 0 - e.g. the host map-fast-travels into a SHIP COCKPIT in
        // space, cell 0x05100136) is NOT walkable: "walk over manually" strands
        // the client and forces a manual F8. Fall back to the host save-bridge
        // for auto co-location, exactly like the procedural case below.
        // Regression: this path never requested a bridge, so space/cockpit
        // fast-travels never auto-followed.
        std::uint32_t srcDoor = g_pendingFollow.sourceDoor.load(std::memory_order_acquire);
        if (srcDoor == 0) {
            // (2026-06-23) - interior-via-fast-travel is the takeoff/space
            // signature. Off for alpha: bridging the client here drags it into
            // space, where the save-load CTDs / strands (see V6 note above and
            // reference_crash_22ce9c6.md). Stay put; the client follows once the
            // host is back on foot in an exterior cell.
            if (!Pulsar::GetConfig().coopSpaceFollow) {
                PLOG_INFO(
                    "[PULSAR] AutoFollow: SKIP - interior fast-travel dest 0x{:08X} "
                    "(takeoff/space); coopSpaceFollow=0, staying put."sv, dst);
                NASLOG("AutoFollow: skip interior-fasttravel (space-follow off) dst=0x%08X", dst);
                return;
            }
            PLOG_INFO(
                "[PULSAR] AutoFollow: SKIP - peer dest 0x{:08X} is interior via "
                "fast-travel (no door); not walkable. Requesting host bridge "
                "(F8 = manual fallback)."sv,
                dst);
            NASLOG("AutoFollow: interior-no-door -> bridge dst=0x%08X", dst);
            RequestHostBridge("dest-interior-fasttravel");   // auto co-location
            return;
        }
        PLOG_INFO(
            "[PULSAR] AutoFollow: SKIP - peer dest 0x{:08X} is interior (door=0x{:08X}); "
            "SetPosition can't load interior cells. Walk over manually."sv,
            dst, srcDoor);
        NASLOG("AutoFollow: skip interior dst=0x%08X", dst);
        return;
    }
    if (destIsProcedural) {
        // PLANET fast-travel: the host map-jumped to a procedural planet surface. Per
        // user request (2026-06-27) the client fast-travels INDEPENDENTLY on planets,
        // so do NOT auto-drag it across the proc boundary - just stay put. F8 remains the
        // manual "regroup at host". coopPlanetFastTravelFollow=1 restores the old auto-co-location.
        if (!Pulsar::GetConfig().coopPlanetFastTravelFollow) {
            PLOG_INFO(
                "[PULSAR] AutoFollow: SKIP - peer dest 0x{:08X} is a planet surface (proc); "
                "client stays independent on planets (F8 = manual regroup)."sv, dst);
            NASLOG("AutoFollow: planet dest -> independent (no drag) dst=0x%08X", dst);
            return;
        }
        PLOG_INFO(
            "[PULSAR] AutoFollow: SKIP - peer dest 0x{:08X} is procedural "
            "(FF-prefix); cross-instance coords meaningless (each player has "
            "own proc-cell instance). Requesting host bridge (F8 = manual "
            "fallback)."sv,
            dst);
        NASLOG("AutoFollow: skip procedural dst=0x%08X", dst);
        RequestHostBridge("dest-procedural");   // auto co-location (save-bridge, like join)
        return;
    }
    if (localIsInterior) {
        PLOG_INFO(
            "[PULSAR] AutoFollow: SKIP - we're in an interior cell; cannot "
            "SetPosition cross-cell to outdoor (would fall through floor). "
            "Walk to a door manually."sv);
        NASLOG("AutoFollow: skip local-interior dst=0x%08X", dst);
        return;
    }
    ///(2026-06-20) - WE are in a PROCEDURAL cell (our parentCell is an
    // FF-prefix proc instance) while the peer's dest is a STATIC cell. A raw
    // SetPosition to the static cell's absolute coords gets resolved INSIDE our
    // own procedural worldspace - the engine then streams us across proc tiles
    // (observed Y jumping -13413..+31625) and dumps us at the cell EDGE, never
    // at the host. SetPosition cannot cross a proc-instance → static-cell
    // boundary. Only the save-bridge (F8 / host auto-bridge) co-locates here.
    // Verified 2026-06-20: host fast-travelled proc-planet 0xFF015B4E → vanilla
    // settlement 0x00014CB6; client (on proc 0xFF015C4D) was flung to (-475,1899)
    // vs host (-475,2138). (Root cause of "client landet an der Cell-Rand".)
    if (localIsProcedural && !destIsProcedural) {
        // We are ON a planet (proc cell). Per user request the client fast-travels
        // INDEPENDENTLY on planets, so don't auto-bridge it off-planet to the host's
        // static dest - stay put (F8 = manual regroup). coopPlanetFastTravelFollow=1
        // restores the old auto-co-location.
        if (!Pulsar::GetConfig().coopPlanetFastTravelFollow) {
            PLOG_INFO(
                "[PULSAR] AutoFollow: SKIP - we're on a planet (proc 0x{:08X}); "
                "client stays independent on planets (F8 = manual regroup)."sv, localCellID);
            NASLOG("AutoFollow: on-planet -> independent (no drag) local=0x%08X", localCellID);
            return;
        }
        PLOG_INFO(
            "[PULSAR] AutoFollow: SKIP - we're in a PROCEDURAL cell 0x{:08X}; "
            "SetPosition to static dst 0x{:08X} would resolve in our proc "
            "worldspace and fling us to the cell edge. Requesting host bridge "
            "(F8 = manual fallback)."sv,
            localCellID, dst);
        NASLOG("AutoFollow: skip local-procedural local=0x%08X dst=0x%08X",
               localCellID, dst);
        RequestHostBridge("local-procedural");   // auto co-location
        return;
    }
    ///cross-worldspace follow. SetPosition coordinates are
    // worldspace-relative; teleporting to peer coords while we are in a
    // different worldspace lands us at the wrong physical spot. (V6 above
    // already tries a clone MoveTo for this case; if it fell through there is
    // no safe SetPosition - defer to the save-bridge.)
    if (peerWS != 0 && localWS != 0 && peerWS != localWS) {
        PLOG_INFO(
            "[PULSAR] AutoFollow: SKIP - cross-worldspace (peerWS=0x{:08X} "
            "localWS=0x{:08X}); SetPosition coords are worldspace-relative and "
            "would land us wrong. Requesting host bridge (F8 = manual fallback)."sv,
            peerWS, localWS);
        NASLOG("AutoFollow: skip cross-ws peerWS=0x%08X localWS=0x%08X",
               peerWS, localWS);
        RequestHostBridge("cross-worldspace");   // auto co-location
        return;
    }

    // Skip if we already happen to be at peer's destination - within a
    // generous 30u radius - avoids unnecessary teleport pops if both walked
    // to the same exterior cell normally.
    auto& cur = player->data.location;
    float dx = cur.x - x, dy = cur.y - y, dz = cur.z - z;
    if ((dx*dx + dy*dy + dz*dz) < (30.0f * 30.0f)) {
        PLOG_INFO(
            "[PULSAR] AutoFollow: skip - already near peer ({:.0f},{:.0f},{:.0f})"sv,
            x, y, z);
        return;
    }

    PLOG_INFO(
        "[PULSAR] AutoFollow: teleporting to peer dst=0x{:08X} ({:.0f},{:.0f},{:.0f})"sv,
        dst, x, y, z);
    NASLOG("AutoFollow: teleport dst=0x%08X", dst);

    // Use the engine's SetPosition with abFlush=true to trigger Havok
    // teleport + cell-streaming at the new coords. Same primitive that
    // already powers connect-time teleport in the client.
    SafeSetPlayerPos(player, x, y, z, rZ);
}

// ---- Client regroup requests (F8 + automatic space-divergence watch) ----

namespace {
    Pulsar::Coop::DivergenceWatch g_divergenceWatch;
}

void RequestHostBridgeFromClient(const char* a_why)
{
    RequestHostBridge(a_why);
}

void NotifyBridged()
{
    g_divergenceWatch.OnBridged(::GetTickCount64());
}

void TickAutoRegroup(bool a_enabled)
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || net.IsHost()) {
        g_divergenceWatch.ResetSession();   // next session starts with a fresh cap
        return;
    }
    if (!a_enabled) return;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    const std::uint32_t localCell = player->parentCell ? player->parentCell->formID : 0;
    const bool inSpace = player->IsInSpace(true);
    auto& remote = Pulsar::RemotePlayer::GetRemotePlayerManager();
    const std::uint64_t now = ::GetTickCount64();
    if (g_divergenceWatch.Tick(now, inSpace, localCell, remote.GetRemoteCellID(),
                               remote.GetRemoteCellAtMs())) {
        PLOG_DIAG("[PULSAR] co-op: out of sync with the host in space for 20 s "
                  "(local cell 0x{:08X}, host cell 0x{:08X}) - asking the host to regroup "
                  "({} of 3 this session)"sv,
                  localCell, remote.GetRemoteCellID(), g_divergenceWatch.Requests());
        RequestHostBridge("auto regroup - space divergence");
    }
}

}  // namespace Pulsar::TransitSync
