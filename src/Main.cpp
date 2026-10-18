#include "PCH.h"
#include "Plugin.h"
#include "Config.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "Network/Crypto.h"
#include "Companion/CompanionManager.h"
#include "Companion/InteractionManager.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "RemotePlayer/EquipSync.h"
#include "FlashlightSync/FlashlightSync.h"
#include "Debug/DropDebugHook.h"
#include "SaveScanner/SaveScanner.h"
#include "SaveScanner/SavePatcher.h"
#include "WorldSync/WorldSync.h"
#include "WorldSync/LootSync.h"
#include "WorldSync/DoorSync.h"
#include "WorldSync/LockSync.h"
#include "WorldSync/GrabSync.h"
#include "WorldSync/ActiSync.h"
#include "WorldSync/StateProbe.h"
#include "QuestSync/QuestSync.h"
#include "DialogSync/DialogSync.h"
#include "EventTrace/EventTrace.h"
#include "VoiceBridge/VoiceBridge.h"
#include "SpawnSync/SpawnSync.h"
#include "CellDiag.h"
#include "Cell3DAttach.h"
#include "TimeWeatherSync/TimeWeatherSync.h"
#include "TransitSync/TransitSync.h"
#include "SpaceSync/SpaceSync.h"
#include "SpaceSync/FastTravelSync.h"
#include "SpaceSync/FlightSync.h"
#include "SpaceSync/ShipCombatSync.h"
#include "SpaceSync/NPCShipProbe.h"
#include "SpaceSync/SpaceFlightProbe.h"
#include "SpaceSync/TakeoffProbe.h"
#include "SpaceSync/DockSync.h"
#include "CellContentSync/CellContentSync.h"
#include "Forms/SaveBuffer.h"
#include "InventorySync/InventorySync.h"
#include "InventorySync/DirectRestore.h"
#include "HookGuard.h"
#include "CrashGuard/CrashGuard.h"
#include "NPCSync/NPCSync.h"
#include "NPCSync/LocomotionApply.h"
#include "Coop/CoopMortality.h"
#include "Coop/KillXpSync.h"
#include "Coop/CombatTaming.h"
#include "VehicleSync/VehicleSync.h"
#include "CloneManager/CloneManager.h"
#include "Profile/ClientProfile.h"
#include "Lobby/LobbyHandoff.h"
#include "Debug/Buddy.h"
#include "Lobby/LobbyMenu.h"
#include "UI/ImGuiOverlay.h"
#include "NASLog.h"

#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include "Animation/BoneSync.h"

#include <RE/B/BGSInventoryItem.h>
#include <RE/B/BSFixedString.h>
#include <RE/U/UI.h>
#include <RE/U/UIMessageQueue.h>

#include <thread>
#include <atomic>
#include <windows.h>
#include <MinHook.h>

// noexcept helper - C2712 forbids __try in functions with C++ unwind objects.
// BSFixedString constructed/passed OUTSIDE __try to avoid the same restriction.
// Used to gate teleport / SetPosition while the engine's loading screen is open.
static bool RawIsMenuOpen(RE::UI* ui, const RE::BSFixedString& name) noexcept
{
    if (!ui) return false;
    __try { return ui->IsMenuOpen(name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeIsLoadMenuOpen()
{
    static const RE::BSFixedString kLoadingMenu("LoadingMenu");
    return RawIsMenuOpen(RE::UI::GetSingleton(), kLoadingMenu);
}

// L.7/after a save-load teleport apply, the engine often leaves
// the client in `kShipAction` (cockpit-pilot camera) because the host's save
// captured them piloting. ForceFirstPerson sets the camera state to FP.
// SEH-guarded because the camera state pointers are touchy right after a
// save-load.
//
// L.7 note: an earlier attempt did `ForceThirdPerson(); ForceFirstPerson();`
// to simulate a mouse-wheel toggle, on the theory that scrolling cleans up
// extra state. User reported that made things WORSE - even mouse-wheel
// stopped working. Reverted back to single ForceFirstPerson.
// L.7 cross-cell guard helper for the post-load teleport path.
// Engine MoveTo (RVA 0x14191EFB0, see memory/reference_engine_moveto.md)
// switches the player's parentCell to dest's cell via the cell-streamer.
// Extracted to its own function so the SEH wrap doesn't collide with
// C++ object unwinding in the larger tick block (C2712).
namespace Pulsar {
    static bool SafeEngineMoveTo(RE::TESObjectREFR* a_self,
                                  RE::TESObjectREFR* a_dest) noexcept
    {
        if (!a_self || !a_dest) return false;
        using Fn = bool(__fastcall*)(RE::TESObjectREFR*, RE::TESObjectREFR*);
        const auto addr = REL::Offset{ 0x191EFB0 }.address();
        if (!addr) return false;
        auto fn = reinterpret_cast<Fn>(addr);
        __try { return fn(a_self, a_dest); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // L.7 resolve TESObjectCELL from formID via TESForm::LookupByID.
    // Returns nullptr if formID is unknown to client (host plugin not loaded)
    // OR the form exists but isn't a cell. Caller falls back to kNone in
    // that case to avoid cross-cell fall.
    static RE::TESObjectCELL* SafeLookupCellByID(std::uint32_t a_formID) noexcept
    {
        if (a_formID == 0) return nullptr;
        __try {
            auto* form = RE::TESForm::LookupByID(a_formID);
            if (!form) return nullptr;
            if (form->GetFormType() != RE::FormType::kCELL) return nullptr;
            return static_cast<RE::TESObjectCELL*>(form);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // L.7 pin actor->parentCell to target cell. Mirror of the existing
    // RemotePlayer::SafePinParentCell pattern. Raw write at offset 0xB0;
    // SEH-wrapped because the actor may be racing with engine cell-stream.
    static bool SafePinActorParentCell(RE::Actor* a_actor,
                                       RE::TESObjectCELL* a_cell) noexcept
    {
        if (!a_actor || !a_cell) return false;
        __try {
            if (a_actor->parentCell != a_cell) a_actor->parentCell = a_cell;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

static void ForceFirstPersonSafe()
{
    auto* cam = RE::PlayerCamera::GetSingleton();
    if (!cam) return;
    __try { cam->ForceFirstPerson(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        PLOG_WARN("[PULSAR] ForceFirstPerson threw - camera state untouched");
    }
}

// L.7 diag - dump engine pilot/UI state so we can see what's actually
// keeping the client locked post-load. Empirical 11:54-11:55: menu count
// stayed 0 and cam-state stayed 0 throughout the "stuck" window, so the
// lock isn't menu- or camera-state. Adding actorState1/2 (bit-flags),
// spaceship pointer, and IsInSpace to find the engine's "I'm piloting"
// flag we're missing. Compare values BEFORE the user presses E vs AFTER -
// the bit that flips on E is likely the lock.
//
// SEH-guarded because UI::menuStack and player ptrs can be mid-mutation
// during cell-stream.
static void DumpOpenMenus_Impl(const char* a_tag)
{
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return;
    auto* cam = RE::PlayerCamera::GetSingleton();
    std::uint32_t camIdx = 0xFFFFFFFFu;
    if (cam) {
        for (std::uint32_t i = 0; i < RE::CameraState::kTotal; ++i) {
            if (cam->QCameraEquals(static_cast<RE::CameraState>(i))) {
                camIdx = i; break;
            }
        }
    }

    // Player engine state - actorState1/2 are bit-flags incl sit/getup,
    // weapon-drawn, etc. spaceshipPtr non-null + inSpace==1 means player
    // is INSIDE a spaceship cell (not necessarily piloting). pilotFID is
    // the spaceship's *active* pilot Actor - that's the binding we need
    // to clear when client is locked. furnH is the furniture handle from
    // AIProcess (player+0x228 → +0x8 → +0x428).
    std::uint32_t actorS1 = 0, actorS2 = 0;
    std::uint32_t spaceshipFID = 0;
    std::uint32_t furnH = 0;
    // L.7 RE'd 2026-05-06 via Ghidra:
    //   actor+0x98 → sub-process; if +0x2e=='A' (0x41), +0x20 bit6 = pilot
    //     (per FUN_142119910 - the pilot-validation gate)
    //   actor+0x37c bit4 = "in vehicle" flag
    //     (per FUN_141aeb6f0 - vehicle-release wrapper)
    void*         subProc = nullptr;
    std::uint8_t  subKind = 0;
    std::uint32_t subFlags = 0;
    std::uint32_t actor37c = 0;
    int           inSpace = -1;
    float         px = 0.0f, py = 0.0f, pz = 0.0f;
    if (auto* p = RE::PlayerCharacter::GetSingleton()) {
        actorS1 = p->actorState1;
        actorS2 = p->actorState2;
        if (auto* ship = p->GetSpaceship(true)) {
            spaceshipFID = ship->formID;
            // NOTE - GetSpaceshipPilot is REL::ID(0) placeholder in
            // CommonLibSF AND per memory feedback_commonlibsf_zero_id_lookup
            // the actual ID 173834 (RVA 0x341FF20) is a 4-arg BSComponentDB2
            // query, NOT a simple zero-arg getter. Calling via raw RVA with
            // wrong signature corrupts the stack. Drop the pilot read here
            // the spaceship FID + furn handle are enough signal.
        }
        inSpace = p->IsInSpace(true) ? 1 : 0;
        px = p->data.location.x;
        py = p->data.location.y;
        pz = p->data.location.z;
        // Furniture handle via the same offset path FlightSync uses for
        // unseat-loop (player+0x228 → AIProcess; +0x8 → MiddleHigh; +0x428).
        auto* aBytes = reinterpret_cast<std::uint8_t*>(p);
        if (auto* proc = *reinterpret_cast<std::uint8_t**>(aBytes + 0x228)) {
            if (auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x08)) {
                furnH = *reinterpret_cast<std::uint32_t*>(mh + 0x428);
            }
        }
        // L.7 pilot-flag fields (RE-verified 2026-05-06 via Ghidra
        // FUN_142119910 + FUN_141aeb6f0).
        subProc = *reinterpret_cast<void**>(aBytes + 0x98);
        if (subProc) {
            auto* sb = reinterpret_cast<std::uint8_t*>(subProc);
            subKind  = *reinterpret_cast<std::uint8_t*>(sb + 0x2e);
            subFlags = *reinterpret_cast<std::uint32_t*>(sb + 0x20);
        }
        actor37c = *reinterpret_cast<std::uint32_t*>(aBytes + 0x37c);
    }
    const bool pilotBit = (subKind == 'A') && ((subFlags >> 6) & 1) != 0;
    const bool vehicleBit = ((actor37c >> 4) & 1) != 0;
    PLOG_INFO(
        "[PULSAR] [L.7 {}] menu={} cam={} actS1=0x{:08X} actS2=0x{:08X} "
        "ship=0x{:08X} furn=0x{:08X} inSpace={} pos=({:.1f},{:.1f},{:.1f}) "
        "subProc=0x{:X} kind=0x{:02X}({}) subFlg=0x{:08X} a37c=0x{:08X} "
        "pilot={} vehicle={}",
        a_tag, ui->menuStack.size(), camIdx,
        actorS1, actorS2, spaceshipFID, furnH, inSpace, px, py, pz,
        reinterpret_cast<std::uintptr_t>(subProc), subKind,
        (subKind >= 0x20 && subKind < 0x7F) ? static_cast<char>(subKind) : '?',
        subFlags, actor37c,
        pilotBit ? 1 : 0, vehicleBit ? 1 : 0);

    for (std::uint32_t i = 0; i < ui->menuStack.size(); ++i) {
        auto& ptr = ui->menuStack[i];
        if (!ptr) {
            PLOG_INFO("[PULSAR] [L.7 {}]   [{}] <null>", a_tag, i);
            continue;
        }
        auto* raw = reinterpret_cast<RE::IMenu*>(ptr.get());
        const char* name = raw ? raw->menuName.c_str() : "<null-menu>";
        PLOG_INFO("[PULSAR] [L.7 {}]   [{}] '{}'", a_tag, i, name);
    }
}
static void DumpOpenMenus(const char* a_tag = "apply")
{
    __try { DumpOpenMenus_Impl(a_tag); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        PLOG_WARN("[PULSAR] DumpOpenMenus threw");
    }
}

// Periodic post-apply menu probe. Arms when teleport applies; runs once
// per second for kPostApplyDumpWindowMs and clears. Catches any menu the
// engine re-opens after our ForceCloseSpaceshipHud - that re-open is the
// suspected root cause of the "stuck in ship UI" symptom.
static std::atomic<std::uint64_t> g_postApplyDumpStartMs{ 0 };
static std::atomic<std::uint64_t> g_postApplyDumpLastMs { 0 };
static constexpr std::uint64_t    kPostApplyDumpWindowMs = 30000;
static constexpr std::uint64_t    kPostApplyDumpIntervalMs = 1000;

static void TickPostApplyMenuDump()
{
    const auto start = g_postApplyDumpStartMs.load(std::memory_order_acquire);
    if (start == 0) return;
    const auto now = ::GetTickCount64();
    if (now - start > kPostApplyDumpWindowMs) {
        g_postApplyDumpStartMs.store(0, std::memory_order_release);
        return;
    }
    const auto last = g_postApplyDumpLastMs.load(std::memory_order_acquire);
    if (now - last < kPostApplyDumpIntervalMs) return;
    g_postApplyDumpLastMs.store(now, std::memory_order_release);

    char tag[32];
    std::snprintf(tag, sizeof(tag), "t+%llus", (now - start) / 1000ULL);
    DumpOpenMenus(tag);
}

// L.7 partial - even after unseat clears the chair binding and
// ForceFirstPerson breaks the camera state, the SpaceshipHud menu layer
// stays open. That menu routes player input to ship-controls and renders
// the cockpit/steering overlay. Symptom: pilotDiag shows seated=0, furn=0,
// active=0, but hud != 0 and player cannot walk on foot. Posting kHide
// to the SpaceshipHud menu via UIMessageQueue cleanly tears it down.
static void RawCloseMenu(RE::UIMessageQueue* q,
                         const RE::BSFixedString& name) noexcept
{
    if (!q) return;
    __try { q->AddMessage(name, RE::UI_MESSAGE_TYPE::kHide); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void ForceCloseSpaceshipHud()
{
    static const RE::BSFixedString kSpaceshipHud("SpaceshipHud");
    RawCloseMenu(RE::UIMessageQueue::GetSingleton(), kSpaceshipHud);
}

// L.7 re-enabled 13:55 for bit-clear ONLY (NOT actS1 upper nibble
// that gave camera at foot-height issues). Empirically verified bits
// that flip when the engine sets the pilot lock state from a save:
// actor+0x37c bit 4 = "in vehicle" (per FUN_141aeb6f0 RE)
// actorState2 bit 12 (correlated with the lock)
// One-shot clear during the post-teleport-apply path. The previous bug
// where pilot handover broke for normal sits was the 30s RETRY-LOOP in
// TickPostLoadUnseat (clearing furniture handle every 250ms even on
// user-initiated sits). That's reverted to 1-shot now. This bit-clear
// runs only once per save-bridge-load → safe.
static void ClearPilotBindingImpl(RE::Actor* a)
{
    auto* aBytes = reinterpret_cast<std::uint8_t*>(a);
    auto* a37c = reinterpret_cast<volatile std::uint32_t*>(aBytes + 0x37c);
    const std::uint32_t prev37c = *a37c;
    const std::uint32_t next37c = prev37c & ~static_cast<std::uint32_t>(0x10);
    *a37c = next37c;

    const std::uint32_t prevS2 = a->actorState2;
    const std::uint32_t nextS2 = prevS2 & ~static_cast<std::uint32_t>(0x1000);
    a->actorState2 = nextS2;

    PLOG_INFO("[PULSAR] [L.7] cleared lock bits - a37c: 0x{:08X}->0x{:08X}, actS2: 0x{:08X}->0x{:08X}",
              prev37c, next37c, prevS2, nextS2);
}
static void ClearPilotBindingSafe(RE::Actor* a)
{
    if (!a) return;
    __try { ClearPilotBindingImpl(a); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        PLOG_WARN("[PULSAR] ClearPilotBinding threw");
    }
}

// L.7 public hook for VehicleSync to call when the host's clone
// transitions seated→unseated on this client (= host stood up from
// pilot chair). Fires three back-to-back diag snapshots to capture the
// state change with millisecond resolution, plus a marker log so we can
// align with the periodic 1Hz dumps. Defined extern-C-ish so VehicleSync
// can declare-and-call without pulling in Main.cpp's anon namespace.
namespace Pulsar::Diag293 {
    void OnHostCloneUnseated()
    {
        PLOG_INFO("[PULSAR] [L.7] === HOST UNSEATED - capturing diag triplet ===");
        DumpOpenMenus("host-unseat-pre");
        // The unseat may settle over a few ticks. Capture once now, then
        // schedule two more in the periodic diag system at +250ms and +500ms.
        // Simplest impl: just re-arm the periodic dump's last-log timestamp
        // so the next tick fires immediately, and bump start so we get a
        // fresh 30s window from this moment.
        const auto now = ::GetTickCount64();
        g_postApplyDumpStartMs.store(now, std::memory_order_release);
        g_postApplyDumpLastMs.store(0, std::memory_order_release);
    }
}

using namespace std::string_view_literals;

static std::atomic<bool> g_running{ false };
static std::thread g_networkThread;
static Pulsar::Config g_config;
namespace Pulsar { const Config& GetConfig() { return g_config; } }   // accessor for other modules

// Phase I - Cell-transition position-sync quiet window.
// State lives in Pulsar::Cell3DAttach so RemotePlayerManager + others can
// query it. Main.cpp drives the loadMenu open/close observations from the
// permanent task tick.

// Player pitch - read from MovementHandlerAgentPlayerControls via interface lookup
// Hook on MovementHandlerAgentPlayerControls::Update (REL::ID 49491)
// Reads param_1+0x20 as accumulated pitch candidate
std::atomic<float> g_playerPitch{0.0f};  // global - read by NetworkHost.cpp

using PlayerCtrlUpdate_t = void(__fastcall*)(void*, void*);
static PlayerCtrlUpdate_t s_origPlayerCtrlUpdate = nullptr;

static void __fastcall HookedPlayerCtrlUpdate(void* param_1, void* param_2)
{
    // Call original first
    s_origPlayerCtrlUpdate(param_1, param_2);

    // HookGuard: bail before any PULSAR-side work during save-load chaos.
    if (!Pulsar::HookGuard::IsSafe()) return;

    // Scan BOTH param_1 and param_2 for pitch values
    if (param_1 && param_2) {
        uintptr_t p2 = reinterpret_cast<uintptr_t>(param_2);

        // Read player pitch graph variables FROM the engine update thread
        // (FillAnimationData reads on a different thread where values might be cleared)
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            float dirPitch = 0.0f, gPitch = 0.0f, camPitch = 0.0f;
            RE::BSFixedString vDP("DirectAtPitchVar"), vP("Pitch"), vCP("fCameraPitch");
            player->GetGraphVariableImplFloat(vDP, dirPitch);
            player->GetGraphVariableImplFloat(vP, gPitch);
            player->GetGraphVariableImplFloat(vCP, camPitch);

            float best = (dirPitch != 0.0f) ? dirPitch :
                         (gPitch != 0.0f) ? gPitch :
                         (camPitch != 0.0f) ? camPitch : 0.0f;
            if (best != 0.0f) {
                g_playerPitch.store(best);
            }

            static uint64_t s_lastLog = 0;
            if (GetTickCount64() - s_lastLog > 2000) {
                s_lastLog = GetTickCount64();
                PLOG_INFO("[PULSAR] HOOK_PITCH: dirP={:.3f} pitch={:.3f} camP={:.3f} angX={:.3f} stored={:.3f}"sv,
                    dirPitch, gPitch, camPitch, player->data.angle.x, g_playerPitch.load());
            }
        }
    }
}

static void InstallPlayerCtrlHook()
{
    static REL::Relocation<void*> funcAddr{ REL::ID(49491) };
    void* target = funcAddr.get();
    MH_STATUS status = MH_CreateHook(target, &HookedPlayerCtrlUpdate,
        reinterpret_cast<LPVOID*>(&s_origPlayerCtrlUpdate));
    if (status == MH_OK) {
        MH_EnableHook(target);
        PLOG_INFO("[PULSAR] MovementHandler hook installed (REL::ID 49491)"sv);
    } else {
        PLOG_WARN("[PULSAR] MovementHandler hook failed: {}"sv, static_cast<int>(status));
    }
}

static void NetworkLoop()
{
    auto& network = Pulsar::Network::GetSingleton();
    while (g_running) {
        network.Update();
        Pulsar::Voice::VoiceBridge::Service();  // drain voice loopback (mic/settings)
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

SFSE_PLUGIN_VERSION = []() noexcept {
    SFSE::PluginVersionData data{};

    data.PluginVersion(Plugin::Version);
    data.PluginName(Plugin::Name);
    data.AuthorName(Plugin::Author);
    data.UsesAddressLibrary(true);
    data.IsLayoutDependent(true);
    data.CompatibleVersions({ REL::Version{ 1, 16, 244, 0 } });  // 244 port: raw RVAs remapped to 1.16.244

    return data;
}();

// Plain-C helper - raw SEH-safe read of occupiedFurniture handle. Can't live
// inside FillAnimationData (C2712: __try vs BSFixedString destructors).
static uint32_t ReadPlayerFurnHandle(RE::Actor* act)
{
    __try {
        if (!act) return 0;
        auto* a = reinterpret_cast<uint8_t*>(act);
        auto* proc = *reinterpret_cast<uint8_t**>(a + 0x228);
        if (!proc) return 0;
        auto* mh = *reinterpret_cast<uint8_t**>(proc + 0x08);
        if (!mh) return 0;
        return *reinterpret_cast<uint32_t*>(mh + 0x428);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// SEH-safe: is the player in a death life-state right now? +0xF8 holds the
// life-state bits (mask 0x1E0000); any non-zero death value means the engine
// is mid death/reload teardown and the anim graph is unsafe to read. Used to
// gate all anim-var/bone SENDS so the both-down game-over death (which puts the
// player into a real death state BEFORE the loadMenu opens, i.e. while
// IsQuiet() is still false) can't drive a pure-virtual GetGraphVariable call
// (client crash 22:43, frame +0x377E90).
static bool PlayerInDeathOrLoadTeardown(RE::Actor* a_player)
{
    __try {
        if (!a_player) return true;
        const std::uint32_t lifeBits =
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(a_player) + 0xF8)
            & 0x1E0000u;
        return lifeBits != 0;  // 0 = alive; anything else = death/bleedout state
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;  // can't read → treat as unsafe
    }
}

static void FillAnimationData(RE::Actor* a_actor, Pulsar::Network::PositionData& a_pos)
{
    if (!a_actor) return;

    RE::BSFixedString speedVar("Speed");
    RE::BSFixedString dirVar("Direction");
    RE::BSFixedString idleLocoVar("iSyncIdleLocomotion");
    RE::BSFixedString sprintVar("iSyncSprintState");
    RE::BSFixedString relaxVar("iSyncRelaxReadySneak");
    RE::BSFixedString stoppedVar("IsCompletelyStopped");
    RE::BSFixedString moveStartVar("bPlayerMoveStartActive");

    RE::BSFixedString lookVar("LookHeadingOffset");

    a_actor->GetGraphVariableImplFloat(speedVar, a_pos.speed);
    a_actor->GetGraphVariableImplFloat(dirVar, a_pos.direction);
    a_actor->GetGraphVariableImplFloat(lookVar, a_pos.lookHeading);

    int32_t idleLoco = 0, sprint = 0, relax = 0;
    bool stopped = false, moveStart = false;

    a_actor->GetGraphVariableImplInt(idleLocoVar, idleLoco);
    a_actor->GetGraphVariableImplInt(sprintVar, sprint);
    a_actor->GetGraphVariableImplInt(relaxVar, relax);
    a_actor->GetGraphVariableImplBool(stoppedVar, stopped);
    a_actor->GetGraphVariableImplBool(moveStartVar, moveStart);

    // 2026-04-20: capture fire-state graph vars for shooting sync
    RE::BSFixedString gunActionsVar("iSyncGunActions");
    RE::BSFixedString gunDownVar("iSyncGunDown");
    int32_t gunActions = 0, gunDown = 0;
    a_actor->GetGraphVariableImplInt(gunActionsVar, gunActions);
    a_actor->GetGraphVariableImplInt(gunDownVar, gunDown);
    a_pos.vars.iSyncGunActions = gunActions;
    a_pos.vars.iSyncGunDown    = gunDown;

    // T124 cleanup 2026-04-25: removed reads for FacingBackward, IsBoostPack*,
    // IsTurning, FullBodyTurn, TurnDeltaSmoothed, iSyncStrafeState - host-diag
    // confirmed these are NOT BOUND on the player graph. Boost is driven by
    // iSyncJumpState=3, backward by Direction angle. iSyncTurnState kept -
    // confirmed flipping 0/1/2.
    RE::BSFixedString turnStateVar("iSyncTurnState");
    a_actor->GetGraphVariableImplInt(turnStateVar, a_pos.vars.iSyncTurnState);

    // 2026-04-25: host aim state for weapon ADS pose sync. Bound on player
    // only when weapon is drawn - flips to 1 on RMB-aim, 0 when released.
    RE::BSFixedString sightedVar("iSyncSighted");
    int32_t hostSighted = 0;
    a_actor->GetGraphVariableImplInt(sightedVar, hostSighted);

    // Weapon state: actorState2 bits 0-1 (proven to work for player)
    // Also read Actor+0xF8 for comparison (Ghidra IsWeaponDrawn uses this for NPCs)
    uint8_t weaponState = static_cast<uint8_t>(a_actor->actorState2 & 0x3);
    uint32_t flags0xF8 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(a_actor) + 0xF8);
    bool engineWeapDrawn = ((flags0xF8 & 0x1E0000) == 0x60000);
    // Use actorState2 with debounce (engine check doesn't work for 1st-person player)
    static bool s_weaponDrawnSticky = false;
    static uint64_t s_sheatheStart = 0;
    if (weaponState >= 2) {
        s_weaponDrawnSticky = true;
        s_sheatheStart = 0;
    } else if (s_weaponDrawnSticky && weaponState == 0) {
        uint64_t now = GetTickCount64();
        if (s_sheatheStart == 0) s_sheatheStart = now;
        if (now - s_sheatheStart > 50) {
            s_weaponDrawnSticky = false;
        }
    }

    // Sneak detection via real engine IsSneaking (REL::ID 100547, RVA 0x18E7660)
    // REL::ID 151014 in IDs.h was WRONG (always returns true, no Actor param)
    using IsSneaking_t = bool(*)(RE::Actor*);
    static REL::Relocation<IsSneaking_t> realIsSneaking{ REL::ID(100547) };
    bool isSneaking = realIsSneaking(a_actor);

    if (isSneaking) {
        relax = 2;  // Sneak takes priority for crouch animation
    } else if (s_weaponDrawnSticky) {
        relax = 1;  // Weapon drawn (standing)
    }

    // Player pitch - captured by PlayerCameraHandler hook (REL::ID 82252)
    float foundPitch = g_playerPitch.load();
    a_pos.vars.lookPitch = foundPitch;
    a_pos.rotX = foundPitch;

    // Debug log (seated flag added 2026-04-22 for VehicleSync diagnosis).
    static uint64_t s_lastFillLog = 0;
    uint64_t fillNow = GetTickCount64();
    if (fillNow - s_lastFillLog > 3000) {
        s_lastFillLog = fillNow;
        bool seatedDiag = (ReadPlayerFurnHandle(a_actor) != 0);
        PLOG_INFO("[PULSAR] FillAnim: spd={:.1f} dir={:.1f} as2weap={} relax={} snk={} seated={} pitch={:.3f}"sv,
            a_pos.speed, a_pos.direction, weaponState, relax, isSneaking, seatedDiag, foundPitch);
    }

    // Seated flag - bit 10. Non-zero occupiedFurniture handle = seated.
    bool localSeated = (ReadPlayerFurnHandle(a_actor) != 0);

    // Bits 11-13 reclaimed 2026-04-25 (T124): FacingBackward/IsBoostPack* not
    // bound on player graph - always 0. Boost now signalled via jumpState=3.
    // Bit 11 reused for hostSighted (aim/ADS state) so clone can mirror pose
    // even when host is aiming without firing.
    a_pos.animFlags =
        (static_cast<uint16_t>(idleLoco & 0x3))        |
        (static_cast<uint16_t>(sprint & 0x3)   << 2)   |
        (static_cast<uint16_t>(relax & 0x7)    << 4)   |
        (static_cast<uint16_t>(stopped ? 1 : 0) << 7)  |
        (static_cast<uint16_t>(moveStart ? 1 : 0) << 8) |
        (static_cast<uint16_t>(s_weaponDrawnSticky ? 1 : 0) << 9) |
        (static_cast<uint16_t>(localSeated ? 1 : 0) << 10) |
        (static_cast<uint16_t>(hostSighted != 0 ? 1 : 0) << 11) |
        // bit 12 - helmet flashlight on (player+0xEB0 != null)
        (static_cast<uint16_t>(Pulsar::FlashlightSync::IsLocalOn() ? 1 : 0) << 12);

    // Fill AnimVarSnapshot with all graph variables
    a_pos.vars.speed      = a_pos.speed;
    a_pos.vars.direction  = a_pos.direction;
    a_pos.vars.lookHeading = a_pos.lookHeading;  // LookHeadingOffset from graph
    a_pos.vars.lookPitch   = foundPitch;

    RE::BSFixedString turnDeltaVar("TurnDelta");
    RE::BSFixedString curSpeedVar("CurrentSpeed");
    a_actor->GetGraphVariableImplFloat(turnDeltaVar, a_pos.vars.turnDelta);
    a_actor->GetGraphVariableImplFloat(curSpeedVar, a_pos.vars.currentSpeed);

    int32_t iState = 0;
    RE::BSFixedString iStateVar("iState");
    a_actor->GetGraphVariableImplInt(iStateVar, iState);

    int32_t jumpState = 0;
    RE::BSFixedString jumpVar("iSyncJumpState");
    a_actor->GetGraphVariableImplInt(jumpVar, jumpState);

    a_pos.vars.iState = iState;
    a_pos.vars.iSyncIdleLocomotion = idleLoco;
    a_pos.vars.iSyncSprintState    = sprint;
    a_pos.vars.iSyncRelaxReadySneak = relax;
    a_pos.vars.iSyncJumpState       = jumpState;
    a_pos.vars.boolFlags = (stopped ? 1 : 0) | (moveStart ? 2 : 0);
}

static uint32_t GetEquippedWeaponFormID(RE::Actor* a_actor)
{
    uint32_t weaponID = 0;
    a_actor->ForEachEquippedItem([&](const RE::BGSInventoryItem& item) -> RE::BSContainer::ForEachResult {
        if (item.object && item.object->IsWeapon()) {
            weaponID = item.object->GetFormID();
            return RE::BSContainer::ForEachResult::kStop;
        }
        return RE::BSContainer::ForEachResult::kContinue;
    });
    return weaponID;
}

// Client: send local player position to host at max 60Hz.
// The game-thread task fires up to 1000x/sec; without throttling the client
// floods the host with 300+ position packets/sec (measured 314/sec in a procedural
// cell session) which drowns both ENet and host game-thread processing.
static void ClientSendPosition()
{
    static uint64_t s_lastSend = 0;
    constexpr uint64_t kMinIntervalMs = 16;   // ~60Hz cap, matches host send rate
    uint64_t now = GetTickCount64();
    if (now - s_lastSend < kMinIntervalMs) return;
    s_lastSend = now;

    // Phase I - same gate as the host broadcast path. Skip while client's
    // own loadMenu is open + grace. Otherwise stale transition coords
    // get pushed to host → host applies → host's clone-of-client pingpongs.
    if (Pulsar::Cell3DAttach::IsQuiet()) return;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    // Both-down game-over: the player enters a real death state before its
    // loadMenu opens → FillAnimationData's GetGraphVariable would crash.
    if (PlayerInDeathOrLoadTeardown(player)) return;

    auto& pos = player->data.location;
    auto& ang = player->data.angle;

    Pulsar::Network::PositionData posData{};
    posData.x = pos.x;
    posData.y = pos.y;
    posData.z = pos.z;
    posData.rotX = ang.x;
    posData.rotY = ang.y;
    posData.rotZ = ang.z;
    // Phase I - sender parentCell for cross-cell detection at receiver.
    posData.cellFormID = player->parentCell ? player->parentCell->formID : 0u;
    FillAnimationData(player, posData);

    uint8_t buf[128]{};
    Pulsar::Network::PacketWriter writer(Pulsar::Network::PKT_POSITION, buf, sizeof(buf));
    writer.Write(posData);
    Pulsar::Network::GetSingleton().Send(writer.Data(), writer.Size(), false);  // unreliable, matches host behavior
    // Bone sending handled in game thread task (shared for host+client)
}

static std::atomic<bool> g_handshakeSent{ false };

// Client: send handshake once connected
static void ClientSendHandshake()
{
    if (g_handshakeSent) return;
    // L.7 V9.4 - wait for ENet's OnConnect callback (peer fully established).
    // IsConnected() returns true the moment enet_host_connect creates a peer
    // even though it's still CONNECTING; sends in that window get silently
    // dropped, which broke handshake on auto-reconnect after the L.7 V9.3
    // resync flow.
    if (!Pulsar::Network::GetSingleton().IsFullyConnected()) return;

    // M.3 - wait until client has loaded a save (player exists in a cell)
    // before sending handshake. This achieves two things:
    //   1. M.3 pre-bridge snapshot has player state to capture (solo items)
    //   2. L.7 pre-load snapshot has player+cell to capture (position TP)
    // Without this gate, auto-connect fires handshake from main-menu within
    // ~5s of plugin init, well before the user can pick their save -
    // resulting in client running with host's items + missed snapshots.
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !p->parentCell) {
            static std::atomic<bool> s_waitingLogged{ false };
            if (!s_waitingLogged.exchange(true)) {
                PLOG_INFO(
                    "[PULSAR] Handshake holding - waiting for client to load a save "
                    "(player.parentCell == null). Load your save from the main menu."sv);
            }
            return;
        }
    }

    Pulsar::Network::HandshakeData hs{};
    // The real plugin version: the host refuses a different one (Network/VersionCompat.h).
    hs.versionMajor = static_cast<uint8_t>(Plugin::Version.major());
    hs.versionMinor = static_cast<uint8_t>(Plugin::Version.minor());
    hs.versionPatch = static_cast<uint8_t>(Plugin::Version.patch());
    hs.dlcFlags = 0x03;
    const char* name = g_config.playerName.empty() ? "Player2" : g_config.playerName.c_str();
    strncpy_s(hs.playerName, sizeof(hs.playerName), name, _TRUNCATE);

    uint8_t buf[64]{};
    Pulsar::Network::PacketWriter writer(Pulsar::Network::PKT_HANDSHAKE, buf, sizeof(buf));
    writer.Write(hs);
    Pulsar::Network::GetSingleton().Send(writer.Data(), writer.Size());

    g_handshakeSent = true;
    Pulsar::WorldSync::BeginWaitingForSave();
    // M.3 pre-bridge snapshot is driven by TickPreBridgeSnapshot which
    // observes (state=WAITING_FOR_SAVE + player loaded) - fires whether
    // the player loaded before or after this handshake send.
    PLOG_INFO("[PULSAR] Client handshake sent (name='{}')"sv, name);
    NASLOG("Client handshake sent (name=%s)", name);
}

static std::atomic<bool> g_profileSent{ false };
static uint64_t g_profileSendDelay = 0;  // wait a few seconds for game to load

// Client: send full profile after handshake so Host can restore inventory on clone
static void ClientSendProfile()
{
    if (g_profileSent) return;
    if (!g_handshakeSent) return;

    // Wait 5 seconds after handshake for inventory to populate
    if (g_profileSendDelay == 0) {
        g_profileSendDelay = GetTickCount64();
        return;
    }
    if (GetTickCount64() - g_profileSendDelay < 5000) return;

    // Load previously-captured client profile from disk.
    // We cannot snapshot the current player here because after WorldSync loads
    // the host save, the "current player" is the HOST character, not the client.
    // The profile on disk reflects the client playing as themselves (captured
    // periodically while not in host's world - see below).
    Pulsar::Profile::ProfileData profile{};
    bool loaded = false;
    if (!g_config.playerName.empty()) {
        loaded = Pulsar::Profile::Load(g_config.playerName, profile);
    }
    if (!loaded) {
        // First-time connect (no disk profile yet) - fall back to current player snapshot.
        // This only works if the user loaded their own save before PULSAR connected;
        // otherwise the profile will be empty and the host clone starts without items.
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        // #poison-guard (overnight 2026-06-24): NEVER snapshot the live player once a
        // bridge has applied the host's save this session - the "current player" is
        // then the HOST character, so the snapshot would send the host's loadout to
        // the host as "the client's profile" and poison the host's clone-of-client.
        // Send an empty profile instead (clone starts itemless, recoverable) rather
        // than host items. The clean disk profile (TickCleanProfileCapture) is the
        // real source once it exists.
        if (Pulsar::InventorySync::DirectRestore::HasBridgedThisSession()) {
            PLOG_WARN("[PULSAR] ClientProfile: no disk profile AND bridged this session "
                      "- refusing live snapshot (would be HOST items); sending empty "
                      "[#poison]"sv);
        } else {
            profile = Pulsar::Profile::SnapshotFromPlayer();
            PLOG_INFO("[PULSAR] ClientProfile: no disk profile, using current player snapshot"sv);
        }
    }
    // Fill in config data from Launcher (overrides whatever disk/snapshot had)
    if (!g_config.playerName.empty()) profile.playerName = g_config.playerName;
    profile.playerLevel = g_config.playerLevel;
    profile.raceName = g_config.raceName;
    profile.gender = g_config.gender;

    // Build packet
    using namespace Pulsar::Network;
    ClientProfileHeader header{};
    strncpy_s(header.playerName, sizeof(header.playerName),
        profile.playerName.empty() ? "Player2" : profile.playerName.c_str(), _TRUNCATE);
    header.playerLevel = profile.playerLevel;
    header.weaponFormID = profile.weaponFormID;
    header.armorCount = static_cast<uint8_t>(
        std::min(profile.armorFormIDs.size(), static_cast<size_t>(MAX_EQUIP_SLOTS)));
    for (uint8_t i = 0; i < header.armorCount; ++i) {
        header.armorFormIDs[i] = profile.armorFormIDs[i];
    }
    header.inventoryCount = static_cast<uint16_t>(profile.inventory.size());

    size_t payloadSize = sizeof(ClientProfileHeader) + profile.inventory.size() * sizeof(ProfileItemNet);
    size_t totalSize = HEADER_SIZE + payloadSize;
    std::vector<uint8_t> pktBuf(totalSize);

    PacketWriter pktWriter(PKT_CLIENT_PROFILE, pktBuf.data(), pktBuf.size());
    pktWriter.Write(header);
    for (auto& item : profile.inventory) {
        ProfileItemNet net{ item.formID, item.count };
        pktWriter.Write(net);
    }
    GetSingleton().Send(pktWriter.Data(), pktWriter.Size(), true);

    g_profileSent = true;
    PLOG_INFO("[PULSAR] Client profile sent: {} items, weapon=0x{:08X}, {} armor"sv,
        profile.inventory.size(), profile.weaponFormID, profile.armorFormIDs.size());
    NASLOG("Client profile sent: %zu items", profile.inventory.size());
    // Do NOT save profile here: at this point the current player is the host
    // character, not the client. Saving would overwrite the real client profile.
}

// BGSStoryTeller::queuedStartQuests push-back caused host crashes at save-load
// transition (engine writes to same array concurrently → race). Reverted to rely
// on the esp's "Start Game Enabled" DNAM flag instead. If we need runtime force-
// start in the future, use IVirtualMachine::SendEvent to trigger a custom event
// on an already-bound script - that's safer than touching BGSStoryTeller directly.

static void OnSFSEMessage(SFSE::MessagingInterface::Message* a_msg)
{
    // Install hooks early
    static bool s_hooksInstalled = false;
    if (!s_hooksInstalled && a_msg->type == SFSE::MessagingInterface::kPostLoad) {
        s_hooksInstalled = true;
        MH_Initialize();  // safe to call multiple times

        // BISECT FIX 2026-05-06 15:25 - VehicleSync's SafeSetMotionType(2/Dynamic)
        // calls were the local-sit blocker. State-map updates kept (so
        // RemotePlayer's seated-skip-position-apply works), motionType change
        // skipped. FlightSync still OFF (separate bisect; broke 2nd-player sit).
        Pulsar::CrashGuard::Init();
        InstallPlayerCtrlHook();
        Pulsar::Debug::InstallDropDebugHook();
        Pulsar::NPCSync::Init();
        Pulsar::QuestSync::Init();
        Pulsar::DialogSync::Init();
        Pulsar::EventTrace::Init();   // dev-only quest/script/spawn/dialogue trace
        Pulsar::Voice::VoiceBridge::Init();  // proximity voice loopback socket
        Pulsar::SpawnSync::Init();    // host-authoritative NPC spawn replication
        Pulsar::VehicleSync::Init();
        Pulsar::WorldSync::Loot::Init();
        Pulsar::WorldSync::Door::Init();
        Pulsar::WorldSync::Lock::Init();
        Pulsar::WorldSync::Grab::Init();
        Pulsar::WorldSync::Acti::Init();
        Pulsar::WorldSync::StateProbe::Init();
        Pulsar::CloneManager::Init();
        Pulsar::Cell3DAttach::Init();
        Pulsar::TransitSync::Init();
        Pulsar::SpaceSync::Init();
        Pulsar::SpaceSync::FastTravel::Init();
        Pulsar::TakeoffProbe::Init();
        Pulsar::DockSync::Init();
        Pulsar::SpaceFlightProbe::Init();
        // BISECT - re-enabled 15:40 to test if VehicleSync's fix also unbreaks
        // FlightSync's 2nd-player-sit regression (likely same root cause).
        Pulsar::FlightSync::Init();
        Pulsar::ShipCombatSync::Init();   // ship-combat sync (slice1 read/log)
        Pulsar::NPCShipProbe::Init();
        Pulsar::CellContentSync::Init();
        Pulsar::Forms::Init();
        Pulsar::InventorySync::Init();
        Pulsar::Coop::Mortality::Init();
        Pulsar::Coop::KillXp::Init();   // co-op kill-XP-sharing
        // ImGui overlay disabled - using external launcher instead
        // Pulsar::UI::InitOverlay();
    }
    if (a_msg->type == SFSE::MessagingInterface::kPostLoad) {
        auto* taskInterface = SFSE::GetTaskInterface();
        if (taskInterface) {
            // Both sides need: CompanionManager (Vasco = other player) + network position sending
            taskInterface->AddPermanentTask([]() {
                // Deferred hooks (need player to exist)
                Pulsar::Debug::InstallDeferredHooks();
                // LootSync TryLateInit removed 2026-04-25 - REL::ID(107155)
                // crashes whether called at kPostLoad or game-thread tick.
                // PlayerCharacter-exists gate was insufficient; singleton is
                // never reliably ready. Switching to Actor::PickUpObject
                // vtable hook (slot 0x15B) instead of event subscription.

                // Read player pitch from fCameraPitch (only works in game thread!)
                {
                    auto* pitchPlayer = RE::PlayerCharacter::GetSingleton();
                    if (pitchPlayer) {
                        float camP = 0.0f;
                        RE::BSFixedString vCP("fCameraPitch");
                        pitchPlayer->GetGraphVariableImplFloat(vCP, camP);
                        if (camP != 0.0f) {
                            g_playerPitch.store(camP);
                        }
                    }
                }

                // Phase I - drive Cell3DAttach loadMenu observation. Cheap.
                {
                    const bool curLoadMenu = SafeIsLoadMenuOpen();
                    Pulsar::Cell3DAttach::SetLoadMenuState(curLoadMenu);

                    // co-op no-menu-pause: keep the world simulating while a partner
                    // is in a menu. RE confirmed the engine sets RE::Main::isGameMenuPaused
                    // (Main+0x448) on a pausing menu open and ~150 subsystem updates each
                    // zero their frame delta when it is set, freezing all NPCs/timers. The
                    // setter is edge-triggered (MenuPauseChangeEvent fires only on the flip),
                    // so forcing the flag false each tick holds it cleared while the menu is
                    // open. ONLY +0x448 is touched (the +0x2b/+0x2c/+0x2e siblings are
                    // quit/reset and must stay untouched). Skipped during LoadingMenu /
                    // post-load grace (IsQuiet) where a nonzero delta mid cell-stream is
                    // unsafe, same reason as the guards below.
                    if (g_config.coopNoMenuPause &&
                        Pulsar::Network::GetSingleton().IsConnected() &&
                        !Pulsar::Cell3DAttach::IsQuiet())
                    {
                        if (auto* main = RE::Main::GetSingleton()) {
                            auto* plMP = RE::PlayerCharacter::GetSingleton();
                            if (plMP && plMP->parentCell) {     // only in active gameplay
                                main->isGameMenuPaused = false;
                            }
                        }
                    }

                    // one-shot co-op time + weather sync (client requests once on
                    // join, host replies, client applies). Self-gated (co-op + in-world).
                    Pulsar::TimeWeatherSync::Tick();
                    static bool     s_prevLoadMenu = false;
                    static uint64_t s_clientReApplyAt = 0;
                    static uint64_t s_loadTickAtOpen = 0;
                    ///both-down UAF guard: a LOCAL game-over reload (both
                    // players downed -> forced death -> save-load) frees+recreates all
                    // actors. The network-save-receipt path nulls the clone ptr, but a
                    // local reload did NOT -> ProcessPending's weapon-draw tick deref'd
                    // the freed clone (dump-confirmed). Invalidate the clone on the
                    // loadMenu RISING edge so nothing touches the freed object post-load.
                    if (curLoadMenu && !s_prevLoadMenu) {
                        Pulsar::RemotePlayer::GetRemotePlayerManager().InvalidateCloneOnSaveLoad();
                        s_loadTickAtOpen = Pulsar::WorldSync::GetLastLoadCompletedTick();
                        // suppress clean-snapshot caching during the reload window (live
                        // state becomes the host-bridged save) so the recovery snapshot
                        // can't be poisoned by a host-state capture.
                        Pulsar::Profile::NotifySaveLoadStarted();
                    }
                    // Phase C robustness - on the client loadMenu FALLING edge schedule a
                    // re-apply of the client's captured profile so his items overwrite the
                    // host-bridged save. The normal OnLoadCompleted path only fires when the
                    // WorldSync load tick ADVANCES (network save-bridge). A LOCAL both-down
                    // game-over reload does NOT advance it -> OnLoadCompleted misses it and
                    // the client kept host items. So we only fire here when the tick did NOT
                    // advance across this loadMenu cycle (i.e. exactly the case OnLoadCompleted
                    // misses) -> no double-restore on normal fast-travel/interiors. Uses the
                    // gains-aware RequestApply (restores captured pre-bridge state, not a
                    // frozen baseline). Settle delay lets the player + cell finish loading.
                    if (!curLoadMenu && s_prevLoadMenu && !g_config.isHost &&
                        Pulsar::Network::GetSingleton().IsConnected()) {
                        s_clientReApplyAt = ::GetTickCount64() + 5000;
                    }
                    s_prevLoadMenu = curLoadMenu;
                    if (s_clientReApplyAt != 0 && !curLoadMenu &&
                        ::GetTickCount64() >= s_clientReApplyAt) {
                        s_clientReApplyAt = 0;
                        const std::uint64_t tickNow = Pulsar::WorldSync::GetLastLoadCompletedTick();
                        const bool tickAdvanced = tickNow != s_loadTickAtOpen;
                        auto* plRA = RE::PlayerCharacter::GetSingleton();
                        const bool cellOk = plRA && plRA->parentCell;
                        if (!tickAdvanced && cellOk) {
                            PLOG_INFO("[PULSAR] Phase-C: client post-load re-apply FIRE "
                                      "(local reload, OnLoadCompleted missed; loadTick {} unchanged)"sv,
                                      tickNow);
                            // Re-SEED from the DB baseline then restore. Plain RequestApply
                            // fails here: the DirectRestore cache is empty post-reload (a
                            // prior bridge restore consumed it) -> RestoreClientState skips.
                            Pulsar::Profile::ReapplyCachedDbProfile();
                        } else {
                            PLOG_INFO("[PULSAR] Phase-C: client post-load re-apply SKIP "
                                      "(tickAdvanced={} cellOk={}; OnLoadCompleted handles bridge loads)"sv,
                                      tickAdvanced, cellOk);
                        }
                    }
                }

                // Host: send own position + animation state to client (moved here
                // from NetworkHost::Update - FillAnimationData touches the animation
                // graph which is not thread-safe off the game thread). Rate-limited
                // to ~60Hz to match the client-side rate.
                //
                // Phase I (2026-04-27): SKIP broadcast while engine LoadingMenu
                // is open + 1.5s post-close grace. Player.data.location during
                // transition is mid-stream / undefined; broadcasting it caused
                // clone cell-flip pingpong on the client (verified 20:11:08
                // logs). The receiver also gates this independently.
                if (g_config.isHost &&
                    Pulsar::Network::GetSingleton().IsConnected() &&
                    !Pulsar::Cell3DAttach::IsQuiet())
                {
                    static uint64_t s_lastHostSend = 0;
                    uint64_t hpNow = GetTickCount64();
                    if (hpNow - s_lastHostSend >= 16) {
                        s_lastHostSend = hpNow;
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        if (player && !PlayerInDeathOrLoadTeardown(player)) {
                            auto& loc = player->data.location;
                            auto& ang = player->data.angle;
                            if (loc.x != 0.0f || loc.y != 0.0f || loc.z != 0.0f) {
                                Pulsar::Network::PositionData pos{};
                                pos.x = loc.x; pos.y = loc.y; pos.z = loc.z;
                                pos.rotX = ang.x; pos.rotY = ang.y; pos.rotZ = ang.z;
                                // Phase I - sender's parentCell so receiver
                                // can detect cross-cell + suppress apply.
                                pos.cellFormID = player->parentCell ? player->parentCell->formID : 0u;
                                FillAnimationData(player, pos);
                                uint8_t buf[128]{};
                                Pulsar::Network::PacketWriter w(
                                    Pulsar::Network::PKT_POSITION, buf, sizeof(buf));
                                w.Write(pos);
                                Pulsar::Network::GetSingleton().Send(w.Data(), w.Size(), false);
                            }
                        }
                    }
                }

                // Client: apply pending teleport to host position (moved here - SetPosition
                // triggers physics-settle which is game-thread-only).
                // GUARD: defer SetPosition while engine's LoadingMenu is open.
                // Crash 2026-04-26 22:21: post-save-resync teleport fired DURING
                // loadMenu OPEN → SetPosition mid-cell-stream crashed client.
                // For connect-time this also helps - never teleport while engine
                // is actively streaming a cell.
                if (!g_config.isHost) {
                    bool inLoadMenu   = SafeIsLoadMenuOpen();
                    auto lastLoadTick = Pulsar::WorldSync::GetLastLoadCompletedTick();
                    auto suspendedAt  = Pulsar::HookGuard::GetSuspendedAtTick();
                    auto nowTick      = ::GetTickCount64();
                    // postLoadSettled requires a load that happened AFTER our
                    // most recent Suspend, plus 5s settle. Without the "after
                    // suspend" check, a stale load tick from connect-time would
                    // satisfy the gate immediately when Suspend re-triggers
                    // for a new save-resync (verified bug 2026-04-26 23:36).
                    bool freshLoadAfterSuspend =
                        (lastLoadTick > suspendedAt) ||
                        (suspendedAt == 0 && lastLoadTick > 0);
                    bool postLoadSettled = freshLoadAfterSuspend &&
                                           (nowTick - lastLoadTick > 5000);
                    // AUTO-RESUME hooks once: new LOADED happened, settle done,
                    // loadMenu closed.
                    if (!inLoadMenu && postLoadSettled &&
                        !Pulsar::HookGuard::IsSafe())
                    {
                        Pulsar::HookGuard::Resume();
                        PLOG_INFO("[PULSAR] HookGuard: resumed after save-load settle"sv);
                    }
                    if (!inLoadMenu && postLoadSettled &&
                        !Pulsar::FlightSync::IsPostLoadUnseatInProgress())
                    {
                        float tx, ty, tz, trz;
                        if (Pulsar::Network::GetSingleton().ConsumePendingTeleport(tx, ty, tz, trz)) {
                            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                                std::uint32_t prevCell = player->parentCell
                                    ? player->parentCell->formID : 0;
                                float prevX = player->data.location.x;
                                float prevY = player->data.location.y;
                                float prevZ = player->data.location.z;

                                // L.7/strategy-driven restore. Same-
                                // cell match (= shared ship interior) → kNone =
                                // skip teleport entirely; user holds E manually
                                // to unseat. Cell-mismatch and missing-snapshot
                                // paths fall through to OFFSET-BEHIND-HOST.
                                RE::NiPoint3 hostPos(tx, ty, tz);
                                auto plan = Pulsar::WorldSync::DetermineRestoreStrategy(
                                    player, hostPos, trz);

                                if (plan.mode == Pulsar::WorldSync::RestoreMode::kNone) {
                                    PLOG_INFO(
                                        "[PULSAR] [TELE-DIAG] SKIP teleport (kNone) "
                                        "- same-cell, engine load placed us; user "
                                        "must hold E ~2s to unseat. cell=0x{:08X} "
                                        "pos=({:.0f},{:.0f},{:.0f})"sv,
                                        prevCell, prevX, prevY, prevZ);
                                    NASLOG("TELE-DIAG SKIP kNone cell=0x%08X pos=(%.0f,%.0f,%.0f)",
                                           prevCell, prevX, prevY, prevZ);
                                    Pulsar::WorldSync::ConsumePreLoadSnapshot();
                                } else {
                                    // L.7 cross-cell guard. If host's
                                    // clone is in a different cell than ours,
                                    // SetPosition alone places us at host's
                                    // X/Y but inside our OLD cell (typically
                                    // client's ship interior) - no floor
                                    // mesh → player falls through. Engine
                                    // MoveTo(self, dest) switches cell via
                                    // the cell-streamer first; then we
                                    // SetPosition for the behind-host offset.
                                    auto* hostClone = Pulsar::RemotePlayer::
                                        GetRemotePlayerManager().GetRemoteActor();
                                    std::uint32_t hostCloneCell =
                                        (hostClone && hostClone->parentCell)
                                            ? hostClone->parentCell->formID
                                            : 0;

                                    // L.7 bridge-failed manual TP.
                                    // Engine bridge load did NOT switch cells,
                                    // hostClone may also still be in client's
                                    // cell (engine reset both). Resolve host
                                    // cell via formID, pin clone to it, so
                                    // EngineMoveTo(player, hostClone) actually
                                    // crosses to host's cell. If cell lookup
                                    // fails, abort TP entirely (fall back to
                                    // kNone semantics - no fall through floor).
                                    bool abortTP = false;
                                    if (plan.mode == Pulsar::WorldSync::
                                            RestoreMode::kBridgeFailedManualTP &&
                                        plan.targetCellID != 0)
                                    {
                                        auto* hostCell = Pulsar::
                                            SafeLookupCellByID(plan.targetCellID);
                                        bool cellPinOk = false;
                                        if (hostCell && hostClone) {
                                            cellPinOk = Pulsar::
                                                SafePinActorParentCell(
                                                    hostClone, hostCell);
                                            if (cellPinOk) {
                                                hostCloneCell = plan.targetCellID;
                                            }
                                        }
                                        PLOG_INFO(
                                            "[PULSAR] [L.7] pin hostClone "
                                            "parentCell=0x{:08X} lookup={} pin={}"sv,
                                            plan.targetCellID,
                                            hostCell ? "ok" : "NULL",
                                            cellPinOk ? "ok" : "fail");
                                        NASLOG("L7-302 pin cell=0x%08X lookup=%s pin=%s",
                                               plan.targetCellID,
                                               hostCell ? "ok" : "NULL",
                                               cellPinOk ? "ok" : "fail");
                                        if (!hostCell) {
                                            PLOG_WARN(
                                                "[PULSAR] [L.7] cell "
                                                "0x{:08X} unresolved - "
                                                "abort TP, fallback kNone."sv,
                                                plan.targetCellID);
                                            NASLOG("L7-302 cell-unresolved fallback-kNone");
                                            abortTP = true;
                                        }
                                    }

                                    bool moveToOk = false;
                                    if (!abortTP && hostClone &&
                                        hostCloneCell != 0 &&
                                        hostCloneCell != prevCell)
                                    {
                                        moveToOk = Pulsar::SafeEngineMoveTo(
                                            static_cast<RE::TESObjectREFR*>(player),
                                            static_cast<RE::TESObjectREFR*>(hostClone));
                                        PLOG_INFO(
                                            "[PULSAR] [TELE-DIAG] MoveTo(player, hostClone) "
                                            "from cell=0x{:08X} → 0x{:08X} = {}"sv,
                                            prevCell, hostCloneCell,
                                            moveToOk ? "ok" : "FAILED");
                                    }

                                    if (!abortTP) {
                                        RE::NiPoint3A applyPos(plan.pos.x, plan.pos.y, plan.pos.z);
                                        player->SetPosition(applyPos, true);
                                        player->data.angle.z = plan.rotZ;
                                    }

                                    const char* modeStr = "FALLBACK-host-pos";
                                    switch (plan.mode) {
                                    case Pulsar::WorldSync::RestoreMode::kOffsetBehindHost:
                                        modeStr = "OFFSET-BEHIND-HOST";
                                        break;
                                    case Pulsar::WorldSync::RestoreMode::kBridgeFailedManualTP:
                                        modeStr = "BRIDGE-FAILED-MANUAL-TP";
                                        break;
                                    case Pulsar::WorldSync::RestoreMode::kOwnShipMoveTo:
                                        modeStr = "OWN-SHIP-MOVETO";  // future
                                        break;
                                    default:
                                        break;
                                    }

                                    if (!abortTP) {
                                        PLOG_INFO(
                                            "[PULSAR] [TELE-DIAG] APPLIED Player teleport ({}) "
                                            "from cell=0x{:08X} pos=({:.0f},{:.0f},{:.0f}) → "
                                            "({:.0f},{:.0f},{:.0f}) rotZ={:.2f} mt={}"sv,
                                            modeStr, prevCell, prevX, prevY, prevZ,
                                            plan.pos.x, plan.pos.y, plan.pos.z, plan.rotZ,
                                            moveToOk ? 1 : 0);
                                        NASLOG("TELE-DIAG APPLIED %s from cell=0x%08X pos=(%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f) mt=%d",
                                               modeStr, prevCell, prevX, prevY, prevZ,
                                               plan.pos.x, plan.pos.y, plan.pos.z, moveToOk ? 1 : 0);
                                    }

                                    Pulsar::WorldSync::ConsumePreLoadSnapshot();
                                }
                            }
                        }
                    }
                }

                Pulsar::CellDiag::Tick(g_config.isHost);
                Pulsar::TransitSync::Tick();
                Pulsar::Lobby::TickApplyProfile();   // Teil 2C: apply staged profile-in once
                Pulsar::Lobby::TickAutoSave();        // auto-save: drain capture request (game thread)
                Pulsar::Lobby::TickCleanProfileCapture(); // A unification: maintain clean DB profile pre-bridge
                // Native Scaleform LobbyMenu removed: blocked (no loadable SWF),
                // replaced by the external PulsarLobby.exe companion tool. The
                // TryRegisterMenuOnce()/ToggleLobbyMenu()/F7 path is dead - dropped.
                TickPostApplyMenuDump();
                Pulsar::TransitSync::TickFollowHotkey(g_config.hotkeyAutoFollow);
                Pulsar::TransitSync::TickFollowApply();
                Pulsar::SpaceSync::FastTravel::TickManualBridgeHotkey(g_config.hotkeyManualBridge);

                // Solo testing aid: F9 toggles an AI teammate (refused while connected).
                Pulsar::Buddy::Tick(g_config.hotkeyBuddy, g_config.buddyBaseFormID);

                // Teil 1 profile - F10 = save loadout snapshot, F11 = restore it.
                // (edge-triggered). Demo: F10, drop/sell items, F11 -> items return.
                {
                    static bool s_f10Down = false;
                    const bool now = (GetAsyncKeyState(g_config.hotkeySaveCharacter) & 0x8000) != 0;
                    if (now && !s_f10Down) {
                        Pulsar::Profile::SaveProfileBlobTest();
                        Pulsar::Lobby::WriteProfileOut();   // Teil 2C: stage profile-out for companion
                    }
                    s_f10Down = now;
                }

                {
                    static bool s_f11Down = false;
                    const bool now = (GetAsyncKeyState(g_config.hotkeyRestoreCharacter) & 0x8000) != 0;
                    if (now && !s_f11Down) {
                        Pulsar::Profile::RestoreProfileBlobTest();
                    }
                    s_f11Down = now;
                }

                // DIAG (dev-only) - F6 dumps player headlamp vs clone light fields.
                if (g_config.devHotkeys) {
                    static bool s_f6Down = false;
                    const bool now = (GetAsyncKeyState(g_config.hotkeyDevFlashlightDump) & 0x8000) != 0;
                    if (now && !s_f6Down) Pulsar::FlashlightSync::DumpLights();
                    s_f6Down = now;
                }

                // L.7 Path B Phase 1 - F9 round-trip test on the
                // newest save. Decompresses + recompresses + writes
                // sidecar "<savename>_pulsar_rt.sfs" so the user can
                // load it from the in-game save menu and verify it
                // works. Edge-triggered (one fire per press).
                if (g_config.devHotkeys) {   // dev-only: save-format round-trip probe
                    static bool s_rtKeyDown = false;
                    const bool now = (GetAsyncKeyState(g_config.hotkeySaveRoundTrip) & 0x8000) != 0;
                    if (now && !s_rtKeyDown) {
                        s_rtKeyDown = true;
                        std::string newest = Pulsar::SaveScanner::GetMostRecentSavePath();
                        if (newest.empty()) {
                            PLOG_WARN("[PULSAR] [SavePatcher RT] no save found"sv);
                        } else {
                            PLOG_INFO("[PULSAR] [SavePatcher RT] testing {}"sv, newest);
                            std::vector<std::uint8_t> decomp;
                            Pulsar::SaveScanner::BcpsMeta meta{};
                            if (Pulsar::SaveScanner::LoadAndDecompressAll(newest, decomp, meta)) {
                                std::vector<std::uint8_t> recomp;
                                if (Pulsar::SaveScanner::RecompressToBcps(decomp, meta, recomp)) {
                                    // Engine's save-list parser expects:
                                    //   Save{N}_{hash}_{num}_{YYYYMMDDhhmmss}_{slot}_{major}_{minor}.sfs
                                    // Save9999 didn't show - try with a
                                    // smaller N (highest existing + 1) and
                                    // also bump the date+saveNumber to
                                    // current time so the engine doesn't
                                    // dedupe with the source save.
                                    auto srcPath = std::filesystem::path(newest);
                                    std::string srcStem = srcPath.stem().string();
                                    // Split srcStem by '_'. Keep parts[1..]
                                    // (everything after the original
                                    // type+N prefix), then mutate the
                                    // saveNumber and datetime fields.
                                    std::vector<std::string> parts;
                                    {
                                        std::size_t s = 0;
                                        while (s < srcStem.size()) {
                                            std::size_t e = srcStem.find('_', s);
                                            if (e == std::string::npos) e = srcStem.size();
                                            parts.push_back(srcStem.substr(s, e - s));
                                            s = e + 1;
                                        }
                                    }
                                    // Expected layout: [0]=Save{N}, [1]=hash,
                                    //   [2]=saveNumber (000XXX), [3]=datetime,
                                    //   [4]=slot, [5]=major, [6]=minor.
                                    std::string newStem;
                                    if (parts.size() >= 7) {
                                        // Bump saveNumber by adding 1000
                                        // (always > existing). Use 6-digit
                                        // zero-padded.
                                        int saveNum = 999000;
                                        try { saveNum = std::stoi(parts[2]) + 1000; }
                                        catch (...) {}
                                        char numBuf[16]{};
                                        std::snprintf(numBuf, sizeof(numBuf),
                                                      "%06d", saveNum);

                                        // Build current datetime YYYYMMDDhhmmss
                                        SYSTEMTIME st{};
                                        ::GetLocalTime(&st);
                                        char dtBuf[24]{};
                                        std::snprintf(dtBuf, sizeof(dtBuf),
                                                      "%04u%02u%02u%02u%02u%02u",
                                                      st.wYear, st.wMonth, st.wDay,
                                                      st.wHour, st.wMinute, st.wSecond);

                                        newStem = std::string("Save500") +
                                                  "_" + parts[1] +
                                                  "_" + numBuf +
                                                  "_" + dtBuf +
                                                  "_" + parts[4] +
                                                  "_" + parts[5] +
                                                  "_" + parts[6];
                                    } else {
                                        // Fallback: just prefix.
                                        newStem = "Save500_pulsar_rt_" + srcStem;
                                    }
                                    auto outPath = srcPath;
                                    outPath.replace_filename(newStem + srcPath.extension().string());

                                    std::ofstream f(outPath, std::ios::binary);
                                    if (f.is_open()) {
                                        f.write(reinterpret_cast<const char*>(recomp.data()),
                                                static_cast<std::streamsize>(recomp.size()));
                                        f.close();
                                        PLOG_INFO(
                                            "[PULSAR] [SavePatcher RT] wrote {} ({} bytes)"sv,
                                            outPath.string(), recomp.size());
                                    } else {
                                        PLOG_WARN(
                                            "[PULSAR] [SavePatcher RT] can't write {}"sv,
                                            outPath.string());
                                    }
                                }
                                // Round-trip integrity (decompress→recompress
                                // →decompress yields same payload).
                                Pulsar::SaveScanner::RoundTripTest(newest);
                            }
                        }
                    } else if (!now) {
                        s_rtKeyDown = false;
                    }
                }

                // L.7 Path B Phase 2 - F11 diff the two newest
                // manual saves on a background thread so the engine
                // watchdog doesn't kill us during the ~hundreds-of-ms
                // diff computation. Edge-triggered, single-shot per
                // press, busy-flag prevents re-entry.
                if (g_config.devHotkeys) {   // dev-only: save-diff probe
                    static bool s_diffKeyDown = false;
                    static std::atomic<bool> s_diffBusy{ false };
                    const bool now = (GetAsyncKeyState(g_config.hotkeySaveDiff) & 0x8000) != 0;
                    if (now && !s_diffKeyDown && !s_diffBusy.load()) {
                        s_diffKeyDown = true;
                        s_diffBusy.store(true);
                        std::string savesDir = Pulsar::SaveScanner::GetSavesDirectory();
                        if (savesDir.empty()) {
                            PLOG_WARN("[PULSAR] [SaveDiff] saves dir not found"sv);
                        } else {
                            // Pick the two newest manual saves: stem
                            // starts with "Save" but NOT "Save500_"
                            // (our F10 round-trip test output) AND not
                            // Auto/Quick/Exitsave (engine auto-saves
                            // can fire mid-test and pollute the pair).
                            struct E { std::filesystem::path p;
                                       std::filesystem::file_time_type t; };
                            std::vector<E> manual;
                            for (auto& it : std::filesystem::directory_iterator(savesDir)) {
                                if (!it.is_regular_file()) continue;
                                if (it.path().extension() != ".sfs") continue;
                                const std::string stem = it.path().stem().string();
                                if (stem.rfind("Save500_", 0) == 0) continue;
                                if (stem.rfind("Autosave", 0) == 0) continue;
                                if (stem.rfind("Quicksave", 0) == 0) continue;
                                if (stem.rfind("Exitsave", 0) == 0) continue;
                                if (stem.rfind("Save", 0) != 0) continue;
                                manual.push_back({ it.path(), it.last_write_time() });
                            }
                            if (manual.size() < 2) {
                                PLOG_WARN(
                                    "[PULSAR] [SaveDiff] need >=2 manual saves "
                                    "(found {}). Make two manual saves first: "
                                    "STAND in cockpit, then PILOT seat, then F11."sv,
                                    manual.size());
                            } else {
                                std::sort(manual.begin(), manual.end(),
                                    [](const E& x, const E& y) { return x.t > y.t; });

                                // Sanity: warn if the two newest are >5min apart
                                // (likely not a controlled STAND/PILOT pair).
                                const auto delta = std::chrono::duration_cast<std::chrono::minutes>(
                                    manual[0].t - manual[1].t).count();
                                if (delta > 5) {
                                    PLOG_WARN(
                                        "[PULSAR] [SaveDiff] two newest manual saves "
                                        "are {} min apart - diff will be NOISY. "
                                        "Make a fresh STAND+PILOT pair within 1 min "
                                        "of each other."sv,
                                        delta);
                                }

#ifndef PULSAR_HARDENED
                                std::filesystem::path outDir("<diag>");
                                std::error_code ec;
                                std::filesystem::create_directories(outDir, ec);
                                auto outFile = outDir / "save_diff_pilot.txt";
                                std::string aPath = manual[1].p.string();
                                std::string bPath = manual[0].p.string();
                                std::string outStr = outFile.string();
                                PLOG_INFO(
                                    "[PULSAR] [SaveDiff] dispatching to bg thread: A={} B={}"sv,
                                    manual[1].p.filename().string(),
                                    manual[0].p.filename().string());
                                std::thread(
                                    [aPath, bPath, outStr]() {
                                        PLOG_INFO("[PULSAR] [SaveDiff bg] thread started"sv);
                                        if (Pulsar::SaveScanner::DiffTwoSaves(
                                                aPath, bPath, outStr)) {
                                            PLOG_INFO(
                                                "[PULSAR] [SaveDiff bg] DONE - see {}"sv,
                                                outStr);
                                        } else {
                                            PLOG_WARN("[PULSAR] [SaveDiff bg] FAILED"sv);
                                        }
                                        s_diffBusy.store(false);
                                    }).detach();
#else
                                s_diffBusy.store(false);   // dev-only SaveDiff dump compiled out
#endif
                            }
                        }
                    } else if (!now) {
                        s_diffKeyDown = false;
                    }
                }
                Pulsar::SpaceSync::TickFollowApply();
                Pulsar::TakeoffProbe::Tick();
                Pulsar::DockSync::Tick();
                Pulsar::SpaceFlightProbe::Tick();
                Pulsar::FlightSync::Tick();
                Pulsar::ShipCombatSync::Tick();   // ship-combat sync (slice1 read/log)
                Pulsar::NPCShipProbe::Tick();
                Pulsar::CellContentSync::Tick();
                Pulsar::Forms::TickProbe(g_config.isHost);
                Pulsar::Forms::DrainPending();
                Pulsar::InventorySync::TickPreBridgeSnapshot();
                Pulsar::InventorySync::DrainPending();
                // L.7 V3 - periodic capture (rate-limited 5s) so the
                // cache is always fresh when OnSaveReceived fires. Also
                // force-captures on the false→true player-ready transition
                // so a connect attempted right after save-load already has
                // a populated cache. Cheap when player not ready.
                Pulsar::InventorySync::DirectRestore::TickPeriodicCapture();
                // V4.2 - deferred post-restore inventory diff (+5s/+10s).
                // No-op until a restore arms it. Catches other sync modules
                // re-applying host state after our restore completes.
                Pulsar::InventorySync::DirectRestore::TickPostRestoreWatch();
                // V4.5 - F9 diagnostic: dump live inventory (formIDs + counts +
                // raw dataSize) on demand. Fire while the inventory menu shows
                // host "ghosts" to prove whether they are in the container or
                // a UI-cache artifact. Edge-triggered (one fire per press).
                // (Was F12 but Steam grabs F12 for screenshots - moved to F9.)
                if (g_config.devHotkeys) {   // dev-only: live-inventory dump
                    static bool s_dumpKeyDown = false;
                    const bool dn = (GetAsyncKeyState(g_config.hotkeyDevDiag) & 0x8000) != 0;
                    if (dn && !s_dumpKeyDown) {
                        s_dumpKeyDown = true;
                        Pulsar::InventorySync::DirectRestore::DumpLiveInventory("F9");
                    } else if (!dn) {
                        s_dumpKeyDown = false;
                    }
                }
                Pulsar::NPCSync::OnGameThreadTick();
                Pulsar::Coop::Mortality::OnGameThreadTick();
                Pulsar::Coop::KillXp::OnGameThreadTick();   // drain shared kill-XP
                Pulsar::Coop::CombatTaming::Tick();         // tame combat AI in co-op
                if (!g_config.isHost) {
                    auto _lastLoadTickCS = Pulsar::WorldSync::GetLastLoadCompletedTick();
                    auto _suspAtCS       = Pulsar::HookGuard::GetSuspendedAtTick();
                    auto _nowTickCS      = ::GetTickCount64();
                    bool _freshCS = (_lastLoadTickCS > _suspAtCS) ||
                                    (_suspAtCS == 0 && _lastLoadTickCS > 0);
                    bool _settledCS = _freshCS &&
                                      (_nowTickCS - _lastLoadTickCS > 5000);
                    if (_settledCS && !SafeIsLoadMenuOpen()) {
                        Pulsar::NPCSync::ApplyPendingRemoteState();
                        Pulsar::InventorySync::OnLoadCompleted();
                    }
                }
                Pulsar::VehicleSync::OnGameThreadTick();
                Pulsar::WorldSync::Loot::DrainPendingPickups();
                Pulsar::WorldSync::Loot::TickContainerPolling();
                Pulsar::WorldSync::Loot::TickKeySync();   // key/keycard mirror
                Pulsar::WorldSync::Door::DrainPending();
                Pulsar::WorldSync::Lock::DrainPending();
                Pulsar::WorldSync::Grab::Tick();
                Pulsar::WorldSync::Grab::DrainPending();

                Pulsar::WorldSync::Acti::DrainPending();
                Pulsar::DialogSync::DrainPending();
                Pulsar::SpawnSync::DrainPending();   // enable host-spawned NPCs

                // BISECT TEST DISABLED - C++-spawned clone writes PulsarSpawn=2.0
                // to the GLOBs, which starves the Papyrus-driven client clone
                // (RemotePlayerManager). Only re-enable when the Papyrus path is
                // off, or after CloneManager takes over the RemotePlayer role.

                Pulsar::RemotePlayer::GetRemotePlayerManager().ProcessPending();

                // Legacy: interactions (drop sync etc.)
                Pulsar::Companion::GetInteractionManager().ProcessPending();

                // Bone sync: send bone transforms at ~20fps (game thread safe).
                // Gate on IsQuiet() + death/teardown like the position sends -
                // ReadBones touches the player's anim graph and pure-virtual-
                // crashed during the both-down reload (client 22:43) because
                // this path had NO transition gate at all.
                if (Pulsar::Network::GetSingleton().IsConnected()
                    && !Pulsar::Cell3DAttach::IsQuiet()) {
                    static uint64_t s_lastBoneSend = 0;
                    static Pulsar::BoneSync::BoneCache s_gameBoneCache;
                    static bool s_boneLogOnce = false;
                    uint64_t boneNow = GetTickCount64();
                    // Bone sync throttled from 50ms (20Hz) → 100ms (10Hz).
                    // Halves the rate of BSAutoReadLock acquisitions against
                    // engine's loadedData; lower rate reduces the window where
                    // our game-thread bone-read overlaps with render-thread
                    // bone-iteration. Suspected contributing factor to Task #91
                    // host crash (+0x30B350E render-thread NULL deref).
                    if (boneNow - s_lastBoneSend >= 100) {
                        s_lastBoneSend = boneNow;
                        if (!s_boneLogOnce) {
                            s_boneLogOnce = true;
                            PLOG_INFO("[PULSAR] Bone send: first attempt..."sv);
                        }
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        if (player && !PlayerInDeathOrLoadTeardown(player)) {
                            Pulsar::BoneSync::BoneFrame frame{};
                            if (Pulsar::BoneSync::ReadBones(player, s_gameBoneCache, frame)) {
                                Pulsar::Network::BoneData pkt{};
                                std::memcpy(pkt.rotations, &frame.rotations, sizeof(frame.rotations));
                                pkt.hipsPos[0] = frame.hipsPosX;
                                pkt.hipsPos[1] = frame.hipsPosY;
                                pkt.hipsPos[2] = frame.hipsPosZ;
                                uint8_t boneBuf[512]{};
                                Pulsar::Network::PacketWriter boneWriter(Pulsar::Network::PKT_BONES, boneBuf, sizeof(boneBuf));
                                boneWriter.Write(pkt);
                                Pulsar::Network::GetSingleton().Send(boneWriter.Data(), boneWriter.Size());
                            }
                        }
                    }
                }

                // F.2-v2 (2026-06-11): live worn-equipment sync. RE-ENABLED
                // 2026-06-19 for ARMOR ONLY. The original crash was the WEAPON
                // equip-apply mutating the clone anim graph → per-tick anim-sync
                // pure-virtual-crash (worst during both-down double reload). Armor
                // (biped-slot visuals) doesn't touch the locomotion graph, so it's
                // safe. Weapon send + apply stay gated off via EquipSync::kSyncWeapons
                // (EquipSync.h) until a safe weapon graph re-init exists. The send
                // only transmits ARMO; the apply skips DoEquipWeapon.
                Pulsar::RemotePlayer::EquipSync::TickSendLocalEquipment();

                // Sarah Morgan tracking (host only, every 2s) - reference for animation variables
                if (g_config.isHost) {
                    static uint64_t s_lastSarahLog = 0;
                    static constexpr RE::TESFormID SARAH_REF_ID = 0x00005986;
                    uint64_t sarahNow = GetTickCount64();
                    if (sarahNow - s_lastSarahLog >= 2000) {
                        s_lastSarahLog = sarahNow;
                        auto* sarahForm = RE::TESForm::LookupByID(SARAH_REF_ID);
                        auto* sarah = sarahForm ? sarahForm->As<RE::Actor>() : nullptr;
                        if (sarah) {
                            float sSpeed = 0, sDir = 0, sTurn = 0, sLook = 0, sCurSpeed = 0;
                            int32_t sState = 0, sIdleLoco = 0, sSprint = 0, sRelax = 0;
                            bool sStopped = false, sMoveStart = false;

                            RE::BSFixedString vSpeed("Speed"), vDir("Direction"),
                                              vTurn("TurnDelta"), vLook("LookHeadingOffset"),
                                              vCurSpeed("CurrentSpeed"),
                                              vState("iState"), vIdleLoco("iSyncIdleLocomotion"),
                                              vSprint("iSyncSprintState"), vRelax("iSyncRelaxReadySneak"),
                                              vStopped("IsCompletelyStopped"), vMoveStart("bPlayerMoveStartActive");

                            sarah->GetGraphVariableImplFloat(vSpeed, sSpeed);
                            sarah->GetGraphVariableImplFloat(vDir, sDir);
                            sarah->GetGraphVariableImplFloat(vTurn, sTurn);
                            sarah->GetGraphVariableImplFloat(vLook, sLook);
                            sarah->GetGraphVariableImplFloat(vCurSpeed, sCurSpeed);
                            sarah->GetGraphVariableImplInt(vState, sState);
                            sarah->GetGraphVariableImplInt(vIdleLoco, sIdleLoco);
                            sarah->GetGraphVariableImplInt(vSprint, sSprint);
                            sarah->GetGraphVariableImplInt(vRelax, sRelax);
                            sarah->GetGraphVariableImplBool(vStopped, sStopped);
                            sarah->GetGraphVariableImplBool(vMoveStart, sMoveStart);

                            // Pitch-related variables from Ghidra RE
                            float sPitch = 0, sCamPitch = 0, sPitchDelta = 0;
                            float sDirectPitch = 0, sAimPitch = 0;
                            bool sHeadTrack = false;
                            RE::BSFixedString vpPitch("Pitch"), vpCamPitch("fCameraPitch"),
                                              vpPitchD("PitchDelta"), vpDirectP("DirectAtPitchVar"),
                                              vpAimP("AimPitchCurrent"), vpHT("bHeadTrackingActive");
                            sarah->GetGraphVariableImplFloat(vpPitch, sPitch);
                            sarah->GetGraphVariableImplFloat(vpCamPitch, sCamPitch);
                            sarah->GetGraphVariableImplFloat(vpPitchD, sPitchDelta);
                            sarah->GetGraphVariableImplFloat(vpDirectP, sDirectPitch);
                            sarah->GetGraphVariableImplFloat(vpAimP, sAimPitch);
                            sarah->GetGraphVariableImplBool(vpHT, sHeadTrack);

                            uint8_t sarahAS2 = static_cast<uint8_t>(sarah->actorState2 & 0x3);
                            PLOG_INFO("[PULSAR] SARAH: Spd={:.1f} Dir={:.2f} iState={} relax={} "
                                      "angX={:.3f} Pitch={:.3f} camP={:.3f} pitchD={:.3f} "
                                      "dirP={:.3f} aimP={:.3f} HT={}"sv,
                                      sSpeed, sDir, sState, sRelax,
                                      sarah->data.angle.x, sPitch, sCamPitch, sPitchDelta,
                                      sDirectPitch, sAimPitch, sHeadTrack);
                        }

                        // Also log player actorState1 to find sneak bits
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        if (player) {
                            int32_t pRelax = 0;
                            RE::BSFixedString pRelaxVar("iSyncRelaxReadySneak");
                            player->GetGraphVariableImplInt(pRelaxVar, pRelax);
                            PLOG_INFO("[PULSAR] PLAYER: as1=0x{:08X} as2=0x{:08X} relax={}"sv,
                                      player->actorState1, player->actorState2, pRelax);
                        }
                    }
                }

                if (!g_config.isHost) {
                    // Client: send handshake + profile + position to host
                    ClientSendHandshake();
                    Pulsar::WorldSync::ProcessPending();
                    if (g_handshakeSent && Pulsar::WorldSync::GetState() == Pulsar::WorldSync::State::LOADED) {
                        ClientSendProfile();
                        ClientSendPosition();
                    }

                    // Periodic profile capture: only while the client is playing in THEIR OWN
                    // save - i.e. before WorldSync has queued the host save load. Once we hit
                    // LOADING/LOADED, the current player is the host character, and snapshotting
                    // would overwrite the real client profile with host data.
                    auto wsState = Pulsar::WorldSync::GetState();
                    bool inHostWorld = (wsState == Pulsar::WorldSync::State::LOADING ||
                                        wsState == Pulsar::WorldSync::State::LOADED);
                    if (!inHostWorld) {
                        static uint64_t s_lastOwnSave = 0;
                        uint64_t profNow = GetTickCount64();
                        if (profNow - s_lastOwnSave >= 30000) {
                            s_lastOwnSave = profNow;
                            auto* player = RE::PlayerCharacter::GetSingleton();
                            if (player) {
                                auto profile = Pulsar::Profile::SnapshotFromPlayer();
                                // Only persist if the player actually has gear - protects
                                // against capturing a blank main-menu/initialisation state.
                                if (!profile.inventory.empty() || !profile.armorFormIDs.empty() ||
                                    profile.weaponFormID != 0) {
                                    if (!g_config.playerName.empty()) profile.playerName = g_config.playerName;
                                    profile.playerLevel = g_config.playerLevel;
                                    profile.raceName = g_config.raceName;
                                    profile.gender = g_config.gender;
                                    Pulsar::Profile::Save(profile);
                                }
                            }
                        }
                    }

                    // Detect disconnect → reset sync flags so next connect re-runs handshake.
                    // NOTE: do NOT save profile on disconnect - the player is still the host
                    // character at this point, so a snapshot would corrupt the real profile.
                    static bool s_wasConnected = false;
                    bool connected = Pulsar::Network::GetSingleton().IsConnected();
                    if (s_wasConnected && !connected) {
                        PLOG_INFO("[PULSAR] Disconnect detected"sv);
                        g_handshakeSent = false;
                        g_profileSent = false;
                        g_profileSendDelay = 0;
                        Pulsar::WorldSync::Reset();
                    }
                    s_wasConnected = connected;
                }
            });
            PLOG_INFO("[PULSAR] Game thread tasks registered ({})"sv, g_config.isHost ? "HOST" : "CLIENT");
        }
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);

    PLOG_INFO("[PULSAR] Plugin loaded! BUILD {} {} [condeval-seh-toctou-v1]"sv, __DATE__, __TIME__);

    // Teil 1 profile codec - pure-logic round-trip self-check (logs PASS/FAIL).
    Pulsar::Profile::RunSelfTest();

    // E2E crypto primitive self-test (ECDH agree + AES-256-GCM round-trip +
    // tamper-reject). Proves the secure channel works on THIS machine before we rely on
    // it. Sanitized DIAG so testers' logs confirm it.
    if (Pulsar::Network::CryptoChannel::SelfTest())
        PLOG_DIAG("[PULSAR] co-op: crypto self-test PASSED"sv);
    else
        PLOG_DIAG("[PULSAR] co-op: crypto self-test FAILED"sv);

    g_config = Pulsar::LoadConfig();

    // KANDIDAT 2 (de-risk): apply the NPC IMovementDirectControl toggle from
    // the ini (client-side experiment; runs in TickLerp).
    Pulsar::NPCSync::SetNpcDirectControl(g_config.npcDirectControl);
    Pulsar::NPCSync::SetNpcNoImc(g_config.npcNoImc);
    Pulsar::NPCSync::SetNpcCombatAiLive(g_config.npcCombatAiLive);

    // Teil 2C - lobby handoff: if the companion staged a handoff file, it drives
    // host/client role + connect target. No-op (config unchanged) if absent.
    {
        auto ho = Pulsar::Lobby::ReadHandoff();
        if (ho.found) {
            PLOG_INFO("[PULSAR] [LobbyHandoff] role={} host={}:{} v{}"sv,
                      ho.role, ho.hostIP, ho.port, ho.version);
            if (ho.role == "client") {
                g_config.isHost = false;
                if (!ho.hostIP.empty()) g_config.hostIP = ho.hostIP;
                if (ho.port != 0)       g_config.port   = ho.port;
            } else if (ho.role == "host") {
                g_config.isHost = true;
            }
            // Relay mode (NAT-free): arm the singleton before StartHost/Connect.
            // Empty relay/session → SetRelay is a no-op (direct/LAN path).
            if (!ho.relaySession.empty() && !ho.relayAddr.empty()) {
                Pulsar::Network::GetSingleton().SetRelay(ho.relayAddr, ho.relaySession);
                PLOG_INFO("[PULSAR] [Relay] handoff relay {} session set"sv, ho.relayAddr);
            }
            // E2E relay-auth secret (anti-MITM): arm before connect. Empty = none; only
            // applied when e2eRelayAuth is on (see NetworkHost OnConnect).
            if (!ho.cryptoSecret.empty()) {
                Pulsar::Network::GetSingleton().SetAuthSecret(ho.cryptoSecret);
                PLOG_INFO("[PULSAR] [Relay] handoff E2E auth secret set ({} chars)"sv, ho.cryptoSecret.size());
            }
        }
    }

    // Init NAS logging (non-fatal if NAS unreachable)
    PLOG_INFO("[PULSAR] Initializing NAS log..."sv);
    Pulsar::NASLog::Get().Init(g_config.isHost ? "HOST" : "CLIENT");
    NASLOG("Plugin loaded, mode=%s", g_config.isHost ? "HOST" : "CLIENT");
    PLOG_INFO("[PULSAR] NAS log initialized"sv);

    // Register for SFSE messages
    auto* messaging = SFSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(OnSFSEMessage);
    }

    auto& network = Pulsar::Network::GetSingleton();

    // issue #1 root-cause backstop: refuse to start co-op networking if another
    // PULSAR instance is already live in this Windows session. A leftover
    // Starfield.exe (killed on the loading screen) becomes a SECOND game instance
    // on the next host/join; that instance hits the StartHost port-in-use fallback,
    // self-connects over LAN as a DIRECT peer, and trips the host's "DIRECT peer ->
    // stop relay keepalive" path, which kills the real remote friend's relay session
    // (endless reason-0 reconnect loop). The tool + companion already close leftover
    // processes before launch; this session-scoped named mutex is the last line of
    // defense that also covers a manual double-launch outside the tool. A second
    // instance still loads and plays; it just stays OUT of the network so it can
    // never form a conflicting co-op session. Fails OPEN on mutex API error.
    static HANDLE s_coopInstanceMutex = ::CreateMutexW(nullptr, TRUE, L"Local\\PULSAR_CoopSingleInstance");
    const bool secondInstance = (s_coopInstanceMutex != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS);
    if (secondInstance) {
        PLOG_WARN(
            "[PULSAR] Another PULSAR co-op instance is already running in this Windows "
            "session. Co-op DISABLED for THIS instance to prevent a double session "
            "(issue #1). Close the extra game (check Task Manager for a leftover "
            "Starfield.exe) and relaunch from the tool."sv);
        NASLOG("Second instance detected - co-op disabled (issue #1 guard)");
        return true;   // plugin still loads; the game runs, just without co-op
    }

    if (g_config.isHost) {
        PLOG_INFO("[PULSAR] Trying HOST on port {}"sv, g_config.port);
        if (network.StartHost(g_config.port)) {
            PLOG_INFO("[PULSAR] === Running as HOST ==="sv);
            NASLOG("=== Running as HOST on port %d ===", g_config.port);
            g_running = true;
            g_networkThread = std::thread(NetworkLoop);
            g_networkThread.detach();
        } else {
            // Port already in use - auto-fallback to client
            PLOG_INFO("[PULSAR] Port {} already in use, falling back to CLIENT"sv, g_config.port);
            g_config.isHost = false;
            if (network.Connect(g_config.hostIP.c_str(), g_config.port)) {
                PLOG_INFO("[PULSAR] === Running as CLIENT ==="sv);
                NASLOG("=== Fallback to CLIENT ===");
                g_running = true;
                g_networkThread = std::thread(NetworkLoop);
                g_networkThread.detach();
            }
        }
    } else {
        PLOG_INFO("[PULSAR] Starting as CLIENT, connecting to {}:{}"sv, g_config.hostIP, g_config.port);
        NASLOG("Starting as CLIENT, connecting to %s:%d", g_config.hostIP.c_str(), g_config.port);
        if (network.Connect(g_config.hostIP.c_str(), g_config.port)) {
            PLOG_INFO("[PULSAR] === Running as CLIENT ==="sv);
            NASLOG("=== Connected as CLIENT ===");
            g_running = true;
            g_networkThread = std::thread(NetworkLoop);
            g_networkThread.detach();
        }
    }

    return true;
}
