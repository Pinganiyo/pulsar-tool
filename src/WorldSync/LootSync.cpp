#include "PCH.h"
#include "VehicleSync/VehicleSync.h"
#include "WorldSync/LootSync.h"
#include "WorldSync/DoorSync.h"      // F.3: door-activation hand-off
#include "WorldSync/LockSync.h"      // F.3.b: lock-state-flip hand-off
#include "WorldSync/ActiSync.h"      // F.7: kACTI activator hand-off

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/NPCSync.h"         // GetActiveCellsSnapshot()
#include "NPCSync/PluginLookup.h"    // FormIDToPluginName / ResolveCrossPlugin
#include "RemotePlayer/RemotePlayerManager.h"  // GetRemoteActor()
#include "TransitSync/TransitSync.h"           // K.2 V2: door-activation note
#include "SpaceSync/TakeoffProbe.h"            // L.6 V1.2: dock-probe activate log
#include "SpaceSync/DockSync.h"                // L.6 V3 Phase 3f: station-activate intercept
#include "Coop/CoopMortality.h"                // Way-A: revive via clone-activate

#include <windows.h>
#include <MinHook.h>
#include <REL/Relocation.h>

#include <RE/A/Actor.h>
#include <RE/T/TESForm.h>
#include <RE/B/BSPointerHandle.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/F/FormTypes.h>
#include <RE/B/BSTEvent.h>

#include <cmath>
#include <cstring>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::WorldSync::Loot {
namespace {

    // ---------------------------------------------------------------------
    // F.1.b (2026-04-24): MinHook TESObjectREFR::Activate - the core helper
    // that every engine activation path funnels through (Papyrus, AI
    // packages, E-key input, combat-reflex pickup). Free function, so one
    // hook catches all sources regardless of per-class vtable overrides.
    //
    // REL::ID 63380 @ RVA 0xB47920 verified via Ghidra RE:
    //   Papyrus "Activate"/"ObjectReference" native registration lives at
    //   FUN_1420a9330 (REL::ID 118826). Its handler FUN_14206a8c0 builds
    //   a 32-byte arg struct and calls FUN_140b48040(self, &args) - that
    //   is this hook target.
    //
    // Signature: bool __fastcall(TESObjectREFR* self, ActivateArgs* args)
    // See reference_activate_re.md for the ActivateArgs layout.
    // ---------------------------------------------------------------------

    struct ActivateArgs {
        RE::TESObjectREFR* activator;   // 0x00
        uint64_t           unused1;      // 0x08
        uint64_t           unused2;      // 0x10
        uint32_t           countOrFlag;  // 0x18
        uint8_t            flag1c;       // 0x1C  (Papyrus abDefaultProcessingOnly)
        uint8_t            flag1d;       // 0x1D
        uint8_t            flag1e;       // 0x1E
        uint8_t            flag1f;       // 0x1F
    };
    static_assert(sizeof(ActivateArgs) == 0x20, "ActivateArgs must be 32 bytes");

    using ActivateFn = bool (__fastcall*)(RE::TESObjectREFR*, ActivateArgs*);
    ActivateFn g_origActivate = nullptr;

    // ---------------------------------------------------------------------
    // F.1.d (2026-04-24): TESObjectREFR::DropObject core, vtable slot 0x8B
    // at REL::ID 63579 (RVA 0xB66680). Called by DropObjectFunctor (Papyrus
    // latent path) AND by direct call sites in the gameplay layer. The UI
    // inventory-menu drop path is expected to funnel here too - this is
    // why we hook the core rather than the Papyrus handler (latent-only).
    //
    // Args struct passed as param_3 (inferred from DropObjectFunctor::run):
    //   +0x00  Form*  item           // TESBoundObject being dropped
    //   +0x08  void*  extraList?     // NiPointer / extra-data smart ptr
    //   +0x10  int*   reserved
    //   +0x18  int    = -1           // handle/marker sentinel
    //   +0x1C  int    count          // stack count
    //   +0x20  int    = 3            // flags / reason-code
    //   +0x28  u64    = 0            // padding
    // ---------------------------------------------------------------------
    using DropCoreFn = void (__fastcall*)(RE::TESObjectREFR*, void*, void*);
    DropCoreFn g_origDropCore = nullptr;

    // Inner REFR-spawn primitive - the function drop-core eventually calls to
    // actually create the REFR in the cell. REL::ID 63591 @ RVA 0xB68F10
    // (FUN_140b69630). Signature refined after crash analysis (2026-04-25):
    // the Ghidra decompile had `param_2` as `undefined4 *` - it's a pointer
    // to a 32-bit TESObjectHandle, NOT a TESObjectREFR**. Previous version
    // treated it as a REFR** and crashed dereferencing the handle value as
    // a pointer.
    //
    //   void __fastcall spawn(
    //     TESObjectREFR* self,      // owner - pos/rot source when custom=NULL
    //     uint32_t*      outHandle, // output: engine handle to new REFR
    //     TESForm*       item,      // base Form to spawn
    //     int            count,     // stack count
    //     void*          extra,     // BSTSmartPointer<ExtraDataList>* or NULL
    //     RE::NiPoint3*  pos,       // custom position or NULL → self pos+forward
    //     RE::NiPoint3*  rot,       // custom rotation or NULL → self rot
    //     int*           refcount); // refcounted int, NULL OK
    //
    // The handle needs TESHandleManager lookup if we want the REFR pointer;
    // but for our purposes the spawn has already registered the REFR in the
    // cell and our cell-sweep will log NEW_REFR on the next tick. We only
    // need to verify the handle is non-null (spawn succeeded) and log it.
    using SpawnRefFn = void (__fastcall*)(
        RE::TESObjectREFR*, uint32_t*,
        RE::TESForm*, int32_t, void*,
        RE::NiPoint3*, RE::NiPoint3*, int32_t*);
    SpawnRefFn g_spawnRef = nullptr;

    // F.1.e (2026-04-25): Papyrus Actor.UnequipItem native handler at
    // REL::ID(116791) @ RVA 0x1FCB310 (FUN_141fce980). Found via xref to
    // "UnequipItem" string in registration site at 0x1400C9432. Args 1+2
    // are VM-context (only used in error paths - None-item, no-inventory,
    // no-3d). For dead NPCs with valid 3D + inventory, success path runs
    // through without touching the VM. This lets us call it off-Papyrus
    // to clear the Actor's equipped-item slot before raw-removing the
    // base entry - fixes the [Tarnung]/equipped-weapon ghost-back issue
    // where engine reconciliation restores the item via the equipped
    // pointer.
    using PapyrusUnequipFn = void (__fastcall*)(
        void* vmCtx, uint32_t vmStackId,
        RE::Actor* actor, RE::TESForm* item,
        bool preventEquip, bool silent);
    PapyrusUnequipFn g_unequipPapyrus = nullptr;

    // F.2 (2026-04-25): Per-mod AttachMod inner core. Found via Papyrus
    // Form.AttachMod handler trace (FUN_14206d840 → calls FUN_140402cf0
    // which loops over mods → calls FUN_140403910 per-mod). REL::ID 40716
    // @ RVA 0x403110. Signature inferred from caller args:
    //   bool (Actor*, Mod*, uint32 rank, uint8 flags)
    // Wait - the Papyrus path is REFR-or-Inv. For REFR, param_1 is REFR*.
    // Our use: post-spawn on receiver, the REFR is the freshly spawned
    // dropped item, and we attach each mod with rank=0, flags=1.
    using AttachModFn = bool (__fastcall*)(
        RE::TESObjectREFR* refr, RE::TESForm* mod,
        uint32_t rank, uint8_t flags);
    AttachModFn g_attachMod = nullptr;

    // (2026-06-18): engine per-entry inventory mod-attach core.
    // FUN_14063de80 @ RVA 0x63D270 - the bare attach reached from the Papyrus
    // AttachMod inventory branch (FUN_14206d840 → FUN_1412e1a20 → here).
    //   bool(void* ctx, void* invEntry, char* statusByte)
    // ctx is a 0x30 struct: +0x00 owner REFR*, +0x08 mod TESForm*, +0x10
    // scratch, +0x18 &statusByte, +0x20 flag(1), +0x21 attach-mode(=1, MUST be
    // set to take the allocate+grow path), +0x22 scratch. invEntry is the
    // 0x28-byte BGSInventoryList entry (*(invList+0x30) + index*0x28); the
    // engine navigates to the stack's kObjectInstance, allocates it if absent,
    // grows the array (FUN_140404300), and adds the mod into its correct slot.
    // Chosen over the full FUN_1412e1a20 wrapper to avoid its critical section /
    // BGSInventoryListEvent dispatch / async BSService command branch - minimal
    // machinery for the fragile post-bridge restore window.
    using AttachModInvCoreFn = bool (__fastcall*)(
        void* ctx, void* invEntry, char* statusByte);
    AttachModInvCoreFn g_attachModInvCore = nullptr;

    // M.2 V3 (2026-05-08): inner AddObjectToContainer hook at RVA 0xB4CBC0.
    // Funnel point for ALL inventory transfers - fires regardless of whether
    // the container's +0xA0 list updates (so it catches BarterMenu / ship
    // cargo paths that don't trigger BGSInventoryListEvent).
    //
    // Signature derived from FUN_140b4f6b0 (vt[0x9A] wrapper) decompile -
    // wrapper calls inner with reordered args:
    //   inner(this, item, smartPtr, reason, count, oldContainer)
    using AddObjInnerFn = void (__fastcall*)(
        RE::TESObjectREFR* dest,
        RE::TESForm*       item,
        void*              smartPtr,    // BSTSmartPointer<ExtraDataList>*
        uint32_t           reason,      // ITEM_TRANSFER_REASON
        int32_t            count,
        RE::TESObjectREFR* src);
    AddObjInnerFn g_origAddObjInner = nullptr;

    // M.2 V25 (2026-05-09): hook FUN_140404300 - the engine's "grow mod
    // array" function. Identified via Ghidra static analysis (m2_v24_growth_re.md).
    // Single function that physically writes BGSObjectInstanceExtra +0x18
    // (modCount) and +0x20 (modArray). Reached via 8+ callers including the
    // AddObjectToContainer side-effect chain that re-resolves leveled-mod
    // entries on items REMAINING in the source container during a take.
    //
    // Hook strategy: when called inside our own AddObjInner sync path, AND
    // the target extra already has modCount > 0, return 0 immediately. This
    // preserves the existing mod-array (which holds host's anchored content
    // or original creation) and stops the engine from re-rolling it. For
    // initial creation (modCount == 0) we let the engine populate normally.
    //
    // Thread-local guard scope: only active during the engine call inside
    // HookedAddObjInner where source/dest is a synced container (player
    // involved, suppressBySink=no). Other callers (Papyrus AttachMod) run
    // unaffected.
    using GrowModsFn = uint32_t (__fastcall*)(void* extra, void* refr, void* smartPtr);
    GrowModsFn g_origGrowMods = nullptr;

    thread_local bool t_insideShipSyncAddObj = false;

    // M.2 V4: noexcept helper to read raw 3-float pos at +0x8C from a REFR-
    // like form. Used inside HookedAddObjInner which has C++ unwinding (so
    // can't __try inline). Returns false if the read raised SEH.
    static bool SafeReadRefrPos(const void* refrLike,
                                 float& outX, float& outY, float& outZ) noexcept
    {
        outX = outY = outZ = 0.f;
        if (!refrLike) return false;
        __try {
            auto* loc = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(refrLike) + 0x8C);
            outX = loc[0]; outY = loc[1]; outZ = loc[2];
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // True while we're replaying a pickup on the client - prevents the
    // client-side hook wrapper from re-broadcasting the event back.
    thread_local bool t_replayingFromPacket = false;

    // F.7 V3 (2026-04-26): track the SPECIFIC REFR currently being replayed
    // so ClassifyActivation can distinguish self-loop (skip) from chained
    // script effects on OTHER REFRs (allow propagation). Without this,
    // host-replayed switches' chained door-opens get filtered out and the
    // door never syncs back to client.
    thread_local RE::TESObjectREFR* t_currentReplayingRefr = nullptr;

    // F.10 (2026-06-11): terminal-chained activations. When the player uses
    // a TERMINAL (kTERM), the door/turret/light it controls gets Activated by
    // the terminal's SCRIPT - activator is the terminal REFR or None, NOT the
    // player - so ClassifyActivation's player gate dropped it and terminal-
    // opened doors never synced. For a short window after a local player
    // terminal use, kDOOR/kACTI activations whose activator is NOT an actor
    // (excludes NPCs walking through doors) classify as normal door/acti and
    // ride the existing DoorSync/ActiSync broadcast+replay.
    static std::atomic<std::uint64_t> s_lastPlayerTermUseMs{ 0 };
    static constexpr std::uint64_t   kTermChainWindowMs = 30000;  // menus stay open a while

    // F.7 V2 - actual definitions live after SnapshotEntry/CollectInventory
    // forward declarations below. (C2065 if defined here referring to
    // SnapshotEntry which isn't yet declared.)

    // Loot-item filter: which TESForm types count as "pickup" for sync
    // purposes. Excludes containers, doors, furniture, actors, activators,
    // lights, etc. (those have their own sync paths or no world-state to
    // converge).
    static bool IsLootFormType(uint8_t formType)
    {
        switch (static_cast<RE::FormType>(formType)) {
        case RE::FormType::kARMO:  // armor
        case RE::FormType::kBOOK:  // book / magazine
        case RE::FormType::kINGR:  // ingredient
        case RE::FormType::kMISC:  // misc loot (credits, junk, etc.)
        case RE::FormType::kWEAP:  // weapon
        case RE::FormType::kAMMO:  // ammo
        case RE::FormType::kKEYM:  // key
        case RE::FormType::kALCH:  // aid
        case RE::FormType::kNOTE:  // note / slate
        case RE::FormType::kSLGM:  // soul gem (unused in SF but harmless)
        case RE::FormType::kFLOR:  // F.9 (2026-06-11): flora/plant HARVEST.
            // Same replay pipeline as world pickups: the client matches the
            // plant REFR by base+pos and replays Activate with the clone as
            // activator - the engine flips the REFR into its harvested state
            // (visual sync) and the yielded ingredient lands in the clone's
            // inventory (invisible on this side → no dupe). Unlike item
            // pickups the REFR persists, which the matcher handles fine.
            return true;
        default:
            return false;
        }
    }

    // Cheap raw-offset read of TESForm::formType (offset 0x2E) - avoids a
    // NiPointer dereference when we only need the byte.
    static uint8_t RawFormType(const void* form)
    {
        if (!form) return 0;
        return *reinterpret_cast<const uint8_t*>(
            static_cast<const uint8_t*>(form) + 0x2E);
    }

    static uint32_t RawFormID(const void* form)
    {
        if (!form) return 0;
        return *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(form) + 0x28);
    }

    // Container inventory snapshot - (base_formID, totalCount) per entry.
    // Used by the event-sink diff path to detect what the engine moved
    // when an BGSInventoryListEvent fires.
    struct ContainerSnapshot {
        std::vector<std::pair<uint32_t, int32_t>> items;
    };

    // Plain-C collect helper - reads the inventoryList's entries into a
    // fixed-size buffer. C2712-free because no C++ objects with destructors
    // cross the __try. Caller copies into ContainerSnapshot::items.
    static constexpr size_t kMaxSnapshotEntries = 512;

    // F.2: max mods per item - verified empirically: most weapons have
    // 12-19 mod slots. 32 is safe upper bound, packet stays <2.5KB at full.
    static constexpr size_t kMaxModsPerDrop = 32;
    struct SnapshotEntry { uint32_t base; int32_t count; };

    static int32_t CollectInventoryEntries(void* invListPtr,
                                            SnapshotEntry* out,
                                            int32_t outCap) noexcept
    {
        if (!invListPtr || !out || outCap <= 0) return 0;
        int32_t n = 0;
        __try {
            auto* invList = reinterpret_cast<uint8_t*>(invListPtr);
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return 0;
            for (uint32_t i = 0; i < entryCount && n < outCap; ++i) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (!obj) continue;
                uint32_t objID = RawFormID(obj);
                if (!objID) continue;
                uint32_t stackCount = *reinterpret_cast<uint32_t*>(entry + 0x10);
                auto* stacksData = *reinterpret_cast<uint8_t**>(entry + 0x18);
                if (!stacksData || stackCount == 0 || stackCount > 1000) continue;
                int32_t total = 0;
                for (uint32_t si = 0; si < stackCount; ++si) {
                    uint8_t* stack = stacksData + static_cast<size_t>(si) * 0x10;
                    uint32_t cnt = *reinterpret_cast<uint32_t*>(stack + 0x08);
                    if (cnt > 100000) continue;
                    total += static_cast<int32_t>(cnt);
                }
                out[n].base  = objID;
                out[n].count = total;
                ++n;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // return whatever we collected so far
        }
        return n;
    }

    // F.7 V2: pre-Activate inventory snapshot for kACTI ghost-cell detection.
    // Filled before g_origActivate runs in HookedActivate when isActi; diffed
    // post-orig to find what the activator script consumed from player.
    static constexpr int32_t kActiSnapMax = 64;
    thread_local SnapshotEntry t_actiPreSnap[kActiSnapMax];
    thread_local int32_t       t_actiPreSnapCount = 0;

    // noexcept helper - diff player inventory post-orig vs pre-snapshot.
    // Lives outside HookedActivate so the SnapshotEntry array + raw memory
    // read don't conflict with HookedActivate's existing __try blocks
    // (C2712: __try can't coexist with C++ unwinding in same function).
    static void ComputeActiConsumedItem(uint32_t& outFormID,
                                         int32_t&  outCount) noexcept
    {
        outFormID = 0;
        outCount  = 0;
        if (t_actiPreSnapCount <= 0) return;
        auto* plr = RE::PlayerCharacter::GetSingleton();
        if (!plr) return;
        uint8_t* plrInv = nullptr;
        __try {
            plrInv = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(plr) + 0xA0);
        } __except (EXCEPTION_EXECUTE_HANDLER) { plrInv = nullptr; }
        if (!plrInv) return;
        SnapshotEntry postBuf[kActiSnapMax]{};
        int32_t postCount = CollectInventoryEntries(
            plrInv, postBuf, kActiSnapMax);

        // Diff: find any base whose count strictly decreased. Picks the
        // first decrease - usually the consumed item. If activator script
        // touches no inventory (verified for Outpost Power Conduit base
        // 0x0000EDC1, 2026-04-26: pre==post 28 items / 11476 total), we
        // return 0 - receiver replays without ghost-add and the script
        // bails identically on its end.
        for (int32_t i = 0; i < t_actiPreSnapCount; ++i) {
            int32_t postC = 0;
            for (int32_t j = 0; j < postCount; ++j) {
                if (postBuf[j].base == t_actiPreSnap[i].base) {
                    postC = postBuf[j].count;
                    break;
                }
            }
            int32_t delta = t_actiPreSnap[i].count - postC;
            if (delta > 0) {
                outFormID = t_actiPreSnap[i].base;
                outCount  = delta;
                return;
            }
        }
    }

    static void SnapshotInventory(RE::TESObjectREFR* refr,
                                   ContainerSnapshot& out) noexcept
    {
        out.items.clear();
        if (!refr) return;
        // Read inventoryList ptr via the REFR's +0xA0 slot (BSGuarded value).
        uint8_t* invList = nullptr;
        __try {
            invList = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(refr) + 0xA0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            invList = nullptr;
        }
        if (!invList) return;
        SnapshotEntry buf[kMaxSnapshotEntries];
        int32_t n = CollectInventoryEntries(invList, buf, static_cast<int32_t>(kMaxSnapshotEntries));
        out.items.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            out.items.emplace_back(buf[i].base, buf[i].count);
        }
    }

    // ---------------------------------------------------------------------
    // BGSInventoryListEvent sink - on each event, we diff the container's
    // current inventoryList against the last snapshot and broadcast deltas.
    // We never decode the event struct itself - it's opaque to us and the
    // snapshot-diff is authoritative.
    // ---------------------------------------------------------------------
    // Opaque event type - we don't know the real struct layout yet. 64
    // bytes is more than any typical inventory event (likely <32 bytes).
    struct InvEventOpaque {
        uint8_t bytes[64];
    };

    // Tracked per-source state - previous snapshot of a container's
    // inventoryList, plus metadata for the peer-side matcher. BGSInventoryList
    // inherits BSTEventSource at offset 0, so the sink's source ptr IS the
    // inventoryList itself.
    struct TrackedContainer {
        uint32_t          containerRefID;
        uint32_t          containerBase;
        uint8_t           refType;       // kREFR=0x4A or kACHR=0x4B
        float             px, py, pz;
        ContainerSnapshot last;
        // M.2 (2026-05-08): timestamp of last player activate, used by
        // TickContainerPolling to limit polling to recently-opened
        // containers. Ship cargo (BarterMenu) doesn't fire BGSInventoryList
        // events on transfers, so its diffs only show up via polling.
        uint64_t          lastActivateMs;
        // M.2 V13 (2026-05-08): debounce window. BarterMenu fires several
        // BGSInventoryListEvents per user action (intermediate take↔place
        // shuffles between buyback / inventory slots). Processing every
        // event broadcasts spurious state and on the receiver replays each
        // → ammo doubles, weapons re-roll. Mark dirty on event arrival;
        // polling tick processes the diff only after 250ms of quiet.
        uint64_t          lastEventMs;   // 0 = clean, else event arrival ts
        // M.2 V22 (2026-05-09): firstEventMs prevents infinite-defer when
        // the user keeps placing items rapidly (events fire every ~100ms,
        // never reaching the 250ms quiet window). Force-flush after 1s of
        // continuous activity so host's container state stays in sync
        // with client's even mid-burst.
        uint64_t          firstEventMs;  // 0 = clean, else first event ts
    };

    static std::mutex                              s_trackedMutex;
    static std::unordered_map<void*, TrackedContainer> s_tracked;
    static std::unordered_set<void*>               s_registeredSources;

    // Called from ProcessEvent to diff current inventoryList state against
    // last snapshot and broadcast any negative deltas. The inventoryList
    // pointer IS the event source (BGSInventoryList extends BSTEventSource
    // at +0, so &list == &list->source - same addr).
    static void DiffAndBroadcastContainer(void* srcPtr);

    static void SendContainerTakePacket(uint32_t containerRefID,
                                         uint32_t containerBase,
                                         uint8_t  refType,
                                         float cx, float cy, float cz,
                                         uint32_t itemBase,
                                         int32_t  count,
                                         uint8_t  transferReason = 4,
                                         const std::vector<uint32_t>* mods = nullptr,
                                         uint16_t modSentinel = 0xFFFF);

    // Forward decl - defined later (~line 850).
    static void ReadInventoryItemMods(RE::TESObjectREFR* refr,
                                       uint32_t formID,
                                       std::vector<uint32_t>& outMods) noexcept;
    static uint16_t ReadInventoryItemModSentinel(RE::TESObjectREFR* refr,
                                                  uint32_t formID) noexcept;

    class ContainerInvSink : public RE::BSTEventSink<InvEventOpaque> {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const InvEventOpaque& /*e*/,
            RE::BSTEventSource<InvEventOpaque>* src) override
        {
            // M.2 V13 (2026-05-08): mark dirty + debounce - DON'T broadcast
            // immediately. BarterMenu fires intermediate take↔place shuffles
            // for the same item during one user action; broadcasting each
            // duplicates ammo and re-rolls weapons on receiver. Polling tick
            // dispatches actual diff after 250ms of no new events.
            if (t_replayingFromPacket) return RE::BSEventNotifyControl::kContinue;
            std::lock_guard g(s_trackedMutex);
            auto it = s_tracked.find(src);
            if (it != s_tracked.end()) {
                const uint64_t now = ::GetTickCount64();
                it->second.lastEventMs = now;
                if (it->second.firstEventMs == 0) {
                    it->second.firstEventMs = now;  // V22: start defer-cycle
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    static ContainerInvSink s_invSink;

    static void DiffAndBroadcastContainer(void* srcPtr)
    {
        // M.2 V12 (2026-05-08): replay-guard. Without this the sink-driven
        // diff path forms an infinite ping-pong: host place → broadcast →
        // client replay → client BGSInventoryListEvent fires on the modified
        // container → client sink broadcasts BACK → host replay → repeat.
        // The AddObjInner hook already honours this guard; the sink path
        // didn't, which produced 3.7M log lines + game freeze in V11.
        if (t_replayingFromPacket) return;

        TrackedContainer tc;
        ContainerSnapshot prev;
        {
            std::lock_guard g(s_trackedMutex);
            auto it = s_tracked.find(srcPtr);
            if (it == s_tracked.end()) return;
            tc   = it->second;
            prev = it->second.last;
        }

        // Collect current inventory into a stack buffer via a noexcept
        // helper (C2712-free), then copy into a ContainerSnapshot.
        SnapshotEntry buf[kMaxSnapshotEntries];
        int32_t n = CollectInventoryEntries(srcPtr, buf,
                                             static_cast<int32_t>(kMaxSnapshotEntries));
        ContainerSnapshot cur;
        cur.items.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            cur.items.emplace_back(buf[i].base, buf[i].count);
        }

        // M.2 V21 (2026-05-09): aggregate cur.items by base so multiple
        // entries with the same FormID (e.g. two Eons with different mod
        // loadouts → separate stacks/entries in the engine) don't produce
        // duplicate place broadcasts. Pre-V21 a 2-of-same-weapon place
        // produced 2× place broadcasts → receiver applied each → ended up
        // with 4 items where host had 2.
        std::unordered_map<uint32_t, int32_t> curMap;
        curMap.reserve(cur.items.size());
        for (auto& [id, c] : cur.items) curMap[id] += c;

        // Build prev map.
        std::unordered_map<uint32_t, int32_t> prevMap;
        prevMap.reserve(prev.items.size());
        for (auto& [id, c] : prev.items) prevMap[id] += c;

        auto& net = Pulsar::Network::GetSingleton();
        bool connected = net.IsConnected();
        bool anyDelta = false;

        // M.2 V10 (2026-05-08): for the place-side broadcast, look up the
        // tracked container's REFR so we can read mods from its just-added
        // inventory entry. For CONT containers the BGSInventoryList path
        // dispatches BEFORE our AddObjInner hook (which gets suppressed
        // via suppressBySink=yes), so without this read the mods are lost.
        RE::TESObjectREFR* trackedRefr = nullptr;
        if (auto* refrForm = RE::TESForm::LookupByID(tc.containerRefID)) {
            uint8_t ft = RawFormType(refrForm);
            if (ft == 0x4A || ft == 0x4B) {
                trackedRefr = reinterpret_cast<RE::TESObjectREFR*>(refrForm);
            }
        }

        for (auto& [id, after] : curMap) {
            int32_t before = 0;
            auto it = prevMap.find(id);
            if (it != prevMap.end()) { before = it->second; prevMap.erase(it); }
            if (after < before) {
                int32_t taken = before - after;
                anyDelta = true;
                PLOG_INFO("[PULSAR] ContainerSync (event) take: container=0x{:08X} item=0x{:08X} x{}"sv,
                          tc.containerRefID, id, taken);
                if (connected) {
                    // Take direction: receiver only RawRemoves on its end
                    // (V8) - no mods needed.
                    SendContainerTakePacket(tc.containerRefID, tc.containerBase,
                                             tc.refType,
                                             tc.px, tc.py, tc.pz, id, taken);
                }
            } else if (after > before) {
                // M.2 (2026-05-08): place-delta. Item count grew (existing
                // entry) → encode as negative count in PKT_CONTAINER_TAKE.
                int32_t placed = after - before;
                anyDelta = true;
                // V10: read mods from the container's inventory entry - the
                // engine has already moved the modded item in (event fires
                // post-transfer), so the entry's stack[0] ExtraDataList has
                // the kObjectInstance with the host's mod loadout.
                std::vector<uint32_t> mods;
                uint16_t sentinel = 0xFFFF;
                if (trackedRefr) {
                    ReadInventoryItemMods(trackedRefr, id, mods);
                    sentinel = ReadInventoryItemModSentinel(trackedRefr, id);
                }
                PLOG_INFO("[PULSAR] ContainerSync (event) place: container=0x{:08X} item=0x{:08X} x{} mods={} sentinel=0x{:04X}"sv,
                          tc.containerRefID, id, placed,
                          static_cast<uint32_t>(mods.size()), sentinel);
                if (connected) {
                    SendContainerTakePacket(tc.containerRefID, tc.containerBase,
                                             tc.refType,
                                             tc.px, tc.py, tc.pz, id, -placed,
                                             /*reason=*/4,
                                             &mods, sentinel);
                }
            }
        }
        // Entries that disappeared entirely - treat as "remainder taken".
        for (auto& [id, before] : prevMap) {
            if (before > 0) {
                anyDelta = true;
                PLOG_INFO("[PULSAR] ContainerSync (event) drain: container=0x{:08X} item=0x{:08X} x{}"sv,
                          tc.containerRefID, id, before);
                if (connected) {
                    SendContainerTakePacket(tc.containerRefID, tc.containerBase,
                                             tc.refType,
                                             tc.px, tc.py, tc.pz, id, before);
                }
            }
        }

        if (anyDelta) {
            std::lock_guard g(s_trackedMutex);
            auto it = s_tracked.find(srcPtr);
            if (it != s_tracked.end()) it->second.last = std::move(cur);
        }
    }

    // Plain-C accessor - pulls out the four raw pointers/values we need
    // from a container REFR so the main register function can use them
    // without mixing __try with C++ objects.
    struct ContainerRawInfo {
        void*    invListSrc;  // = *(refr + 0xA0) - IS the BSTEventSource
        uint32_t refID;
        uint32_t baseID;
        float    px, py, pz;
    };
    static ContainerRawInfo ReadContainerRaw(RE::TESObjectREFR* container) noexcept
    {
        ContainerRawInfo out{};
        if (!container) return out;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(container);
            out.invListSrc = *reinterpret_cast<void**>(pb + 0xA0);
            out.refID = RawFormID(container);
            auto* baseRaw = *reinterpret_cast<uint8_t**>(pb + 0x98);
            if (baseRaw) out.baseID = RawFormID(baseRaw);
            auto* loc = reinterpret_cast<float*>(pb + 0x8C);
            out.px = loc[0]; out.py = loc[1]; out.pz = loc[2];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out.invListSrc = nullptr;
        }
        return out;
    }

    // Register our sink on the container's BGSInventoryList event source so
    // we receive ProcessEvent callbacks for every item add/remove. Called
    // from the Activate hook when a container is first activated - keyed
    // on source pointer so we only register once per container.
    static void RegisterSinkOnContainer(RE::TESObjectREFR* container)
    {
        ContainerRawInfo info = ReadContainerRaw(container);
        if (!info.invListSrc) {
            PLOG_INFO(
                "[PULSAR] F.8 sink-reg: ref=0x{:08X} base=0x{:08X} SKIP - invListSrc=NULL (container has no event source - F.3.d / weapon-case style?)"sv,
                info.refID, info.baseID);
            return;
        }
        auto* src = reinterpret_cast<RE::BSTEventSource<InvEventOpaque>*>(info.invListSrc);
        {
            std::lock_guard g(s_trackedMutex);
            if (s_registeredSources.count(src)) {
                // M.2: refresh activate-timestamp + reset snapshot so polling
                // diffs against current state on this re-open (otherwise a
                // delta against a stale pre-close snap would replay closed-
                // session deltas).
                size_t curEntries = 0;
                auto it = s_tracked.find(src);
                if (it != s_tracked.end()) {
                    it->second.lastActivateMs = ::GetTickCount64();
                    SnapshotInventory(container, it->second.last);
                    curEntries = it->second.last.items.size();
                }
                PLOG_INFO(
                    "[PULSAR] F.8 sink-reg: ref=0x{:08X} base=0x{:08X} already-registered (src=0x{:x}) - refreshed snap entries={}"sv,
                    info.refID, info.baseID,
                    reinterpret_cast<uintptr_t>(src), curEntries);
                return;
            }
            s_registeredSources.insert(src);
        }
        src->RegisterSink(&s_invSink);

        TrackedContainer tc{};
        tc.containerRefID = info.refID;
        tc.containerBase  = info.baseID;
        tc.refType        = RawFormType(container);  // kREFR or kACHR
        tc.px = info.px; tc.py = info.py; tc.pz = info.pz;
        tc.lastActivateMs = ::GetTickCount64();
        SnapshotInventory(container, tc.last);
        const size_t entryCount = tc.last.items.size();

        {
            std::lock_guard g(s_trackedMutex);
            s_tracked[src] = std::move(tc);
        }

        PLOG_INFO("[PULSAR] ContainerSync: tracking container ref=0x{:08X} base=0x{:08X} (source=0x{:x}) entries={}"sv,
                  info.refID, info.baseID,
                  reinterpret_cast<uintptr_t>(src), entryCount);
    }


    static void SendContainerTakePacket(uint32_t containerRefID,
                                         uint32_t containerBase,
                                         uint8_t  refType,
                                         float cx, float cy, float cz,
                                         uint32_t itemBase,
                                         int32_t  count,
                                         uint8_t  transferReason,
                                         const std::vector<uint32_t>* mods,
                                         uint16_t modSentinel)
    {
        using namespace Pulsar::Network;

        ContainerTakeData d{};
        d.modSentinel = modSentinel;
        d.containerRefID   = containerRefID;
        d.containerBase    = containerBase;
        d.containerLocalID = NPCSync::PluginLookup::FormIDToLocalID(containerBase);
        d.posX = cx; d.posY = cy; d.posZ = cz;
        d.itemBase         = itemBase;
        d.itemLocalID      = NPCSync::PluginLookup::FormIDToLocalID(itemBase);
        d.count            = count;
        d.containerRefType = refType;
        d.transferReason   = transferReason;
        d.modCount = static_cast<uint8_t>(mods
            ? std::min<size_t>(mods->size(), kMaxModsPerDrop) : 0);
        NPCSync::PluginLookup::FormIDToPluginName(
            containerBase, d.containerPlugin, sizeof(d.containerPlugin));
        NPCSync::PluginLookup::FormIDToPluginName(
            itemBase, d.itemPlugin, sizeof(d.itemPlugin));

        // Build trailing mod descriptors (variable-length payload).
        std::vector<LootDropMod> modBuf(d.modCount);
        for (uint8_t i = 0; i < d.modCount; ++i) {
            uint32_t modID = (*mods)[i];
            modBuf[i].base    = modID;
            modBuf[i].localID = NPCSync::PluginLookup::FormIDToLocalID(modID);
            NPCSync::PluginLookup::FormIDToPluginName(
                modID, modBuf[i].plugin, sizeof(modBuf[i].plugin));
        }

        size_t totalSize = 1 + sizeof(ContainerTakeData)
                            + d.modCount * sizeof(LootDropMod);
        std::vector<uint8_t> buf(totalSize);
        buf[0] = PKT_CONTAINER_TAKE;
        std::memcpy(buf.data() + 1, &d, sizeof(d));
        if (d.modCount > 0) {
            std::memcpy(buf.data() + 1 + sizeof(d),
                         modBuf.data(),
                         d.modCount * sizeof(LootDropMod));
        }
        Pulsar::Network::GetSingleton().Send(buf.data(), totalSize, true);

        PLOG_INFO(
            "[PULSAR] ContainerSync TX: container=0x{:08X}(base=0x{:08X} '{}' type=0x{:02X}) item=0x{:08X}('{}') x{} mods={} @({:.0f},{:.0f},{:.0f})"sv,
            containerRefID, containerBase,
            d.containerPlugin[0] ? d.containerPlugin : "(none)",
            refType,
            itemBase, d.itemPlugin[0] ? d.itemPlugin : "(none)",
            count, d.modCount, cx, cy, cz);
    }

    static void SendDropPacket(uint32_t baseFormID, uint8_t formType,
                               int32_t count,
                               const std::vector<uint32_t>& mods,
                               uint16_t modSentinel)
    {
        using namespace Pulsar::Network;

        LootDropData d{};
        d.itemBase     = baseFormID;
        d.itemLocalID  = NPCSync::PluginLookup::FormIDToLocalID(baseFormID);
        d.itemFormType = formType;
        d.count        = count;
        d.modCount     = static_cast<uint8_t>(
            std::min<size_t>(mods.size(), kMaxModsPerDrop));
        d.modSentinel  = modSentinel;  // M.2 V16
        NPCSync::PluginLookup::FormIDToPluginName(
            baseFormID, d.itemPlugin, sizeof(d.itemPlugin));

        // Build trailing mod descriptors (variable-length payload).
        std::vector<LootDropMod> modBuf(d.modCount);
        for (uint8_t i = 0; i < d.modCount; ++i) {
            uint32_t modID = mods[i];
            modBuf[i].base    = modID;
            modBuf[i].localID = NPCSync::PluginLookup::FormIDToLocalID(modID);
            NPCSync::PluginLookup::FormIDToPluginName(
                modID, modBuf[i].plugin, sizeof(modBuf[i].plugin));
        }

        size_t totalSize = 1 + sizeof(LootDropData)
                            + d.modCount * sizeof(LootDropMod);
        std::vector<uint8_t> buf(totalSize);
        buf[0] = PKT_LOOT_DROP;
        std::memcpy(buf.data() + 1, &d, sizeof(d));
        if (d.modCount > 0) {
            std::memcpy(buf.data() + 1 + sizeof(d),
                         modBuf.data(),
                         d.modCount * sizeof(LootDropMod));
        }
        Pulsar::Network::GetSingleton().Send(buf.data(), totalSize, true);

        PLOG_INFO(
            "[PULSAR] F.1.d TX: item=0x{:08X} type=0x{:02X} count={} plugin='{}' localID=0x{:X} mods={}"sv,
            baseFormID, formType, count,
            d.itemPlugin[0] ? d.itemPlugin : "(none)", d.itemLocalID,
            d.modCount);
    }

    static void SendPickupPacket(uint32_t refID, uint32_t baseID, uint8_t baseType,
                                  float px, float py, float pz)
    {
        using namespace Pulsar::Network;

        LootPickupData data{};
        data.refFormID    = refID;
        data.baseFormID   = baseID;
        data.baseLocalID  = NPCSync::PluginLookup::FormIDToLocalID(baseID);
        data.baseFormType = baseType;
        data.posX         = px;
        data.posY         = py;
        data.posZ         = pz;
        NPCSync::PluginLookup::FormIDToPluginName(
            baseID, data.basePlugin, sizeof(data.basePlugin));

        uint8_t buf[1 + sizeof(LootPickupData)]{};
        buf[0] = PKT_LOOT_PICKUP;
        std::memcpy(buf + 1, &data, sizeof(data));

        auto& net = Pulsar::Network::GetSingleton();
        net.Send(buf, sizeof(buf), true);  // reliable - item-state must not drop

        PLOG_INFO(
            "[PULSAR] LootSync TX: ref=0x{:08X} base=0x{:08X} type=0x{:02X} plugin='{}' @({:.0f},{:.0f},{:.0f})"sv,
            refID, baseID, baseType,
            data.basePlugin[0] ? data.basePlugin : "(none)",
            px, py, pz);
    }

    // Plain-C helper - inspects the activation and pulls out the fields we
    // care about. noexcept + no C++ objects so __try is legal here.
    struct ActivationClass {
        uint32_t selfID;
        uint32_t baseID;
        uint8_t  baseType;
        float    px, py, pz;
        bool     eligibleLoot;
        bool     isContainer;
        bool     isNPC;        // F.1.e probe: base is TESNPC (live or dead)
        bool     isDoor;       // F.3: base is TESObjectDOOR
        bool     isActi;       // F.7: base is TESObjectACTI (switch, button, conduit)
    };
    static ActivationClass ClassifyActivation(RE::TESObjectREFR* self,
                                                ActivateArgs* args,
                                                bool replaying) noexcept
    {
        ActivationClass c{};
        __try {
            if (!self || !args) return c;
            // F.11 (2026-06-11): script SetOpen drives big station doors through
            // the Activate core with a NULL activator (Ghidra: native handler
            // FUN_142083d60 zeroes the ActivateArgs) - the old null-activator
            // early-out made those doors invisible to the sync. Null activator
            // is now allowed through for kDOOR/kACTI classification below; all
            // other types still require an activator.
            const bool nullActivator = (args->activator == nullptr);
            // Self-loop guard: if this is the very REFR our replay is
            // currently driving, skip - otherwise we'd re-broadcast and
            // bounce the activation back-and-forth.
            if (replaying && self == t_currentReplayingRefr) return c;

            c.selfID = RawFormID(self);
            auto* baseRaw = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(self) + 0x98);
            if (!baseRaw) return c;
            c.baseID   = RawFormID(baseRaw);
            c.baseType = RawFormType(baseRaw);
            auto* loc = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(self) + 0x8C);
            c.px = loc[0]; c.py = loc[1]; c.pz = loc[2];

            const bool isDoorType = c.baseType == static_cast<uint8_t>(RE::FormType::kDOOR);
            const bool isActiType = c.baseType == static_cast<uint8_t>(RE::FormType::kACTI);
            if (nullActivator && !(isDoorType || isActiType)) return c;

            uint32_t activatorID = nullActivator ? 0 : RawFormID(args->activator);
            bool playerActivated = (activatorID == 0x14);
            // F.7 V3: chained-effect propagation - when our replay's
            // script-chain triggers an Activate on a DIFFERENT kDOOR (or
            // kACTI), allow it through despite non-player activator. This
            // is what makes "client presses switch → host's chained door
            // open → propagates back to client" work. NPCs walking through
            // doors don't trigger this branch (replaying=false then).
            // #jitter 2026-06-23: do NOT re-broadcast DOORS during a replay either. Same
            // echo as ACTIs: the replay side re-broadcast every door-open (chainedDoor),
            // and although the door STATE converges (stays open), the broadcast did not -
            // host<->client ping-ponged the same door 400+ times → door jitter. The
            // ORIGINAL side (the button press, replaying=no) still broadcasts the doors via
            // F.10/F.11 below, so doors still open on both; the replay side only applies.
            bool chainedDoor = false;
            // #freeze 2026-06-23: do NOT re-broadcast ACTIs during a replay. A connected/
            // linked activator (a button wired to a door + sibling buttons) fans out when
            // replayed; an ACTI has no convergent state (unlike a door's open/closed), so
            // re-broadcasting replayed ACTIs ping-pongs host<->client and froze both in one
            // frame (button-door test). The door the button drives still syncs via
            // chainedDoor + DoorSync. F.10/F.11 below still set chainedActi on the ORIGINAL
            // side (replaying=false), so legit script/terminal acti-chains are unaffected.
            bool chainedActi = false;
            // F.11: script SetOpen (null activator) on doors/activators - the
            // path big station doors take. Broadcast like a normal door/acti;
            // SetOpen on the receiving side is state-conditional so the replay
            // converges instead of toggling.
            if (nullActivator && !replaying) {
                if (isDoorType) chainedDoor = true;
                if (isActiType) chainedActi = true;
            }
            // F.10: terminal-chained door/acti - script-driven Activate (the
            // activator is NOT an actor; NPCs walking through doors have kACHR
            // activators and stay excluded) inside the window after a local
            // player terminal use. Note the timestamp BEFORE this check is
            // taken in HookedActivate when the player activates a kTERM base.
            if (!playerActivated && !replaying && !nullActivator
                && (isDoorType || isActiType)
                && RawFormType(args->activator) != static_cast<uint8_t>(RE::FormType::kACHR)) {
                const std::uint64_t termMs =
                    s_lastPlayerTermUseMs.load(std::memory_order_relaxed);
                if (termMs != 0 && ::GetTickCount64() - termMs < kTermChainWindowMs) {
                    if (isDoorType) {
                        chainedDoor = true;
                    } else {
                        chainedActi = true;
                    }
                }
            }
            if (!playerActivated && !chainedDoor && !chainedActi) return c;

            if (c.baseType == static_cast<uint8_t>(RE::FormType::kCONT)) {
                c.isContainer = true;
            } else if (c.baseType == static_cast<uint8_t>(RE::FormType::kNPC_)) {
                c.isNPC = true;
            } else if (c.baseType == static_cast<uint8_t>(RE::FormType::kDOOR)) {
                c.isDoor = true;
            } else if (c.baseType == static_cast<uint8_t>(RE::FormType::kACTI)) {
                c.isActi = true;
            } else if (IsLootFormType(c.baseType)) {
                c.eligibleLoot = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // partial fields are fine; flags stay false
        }
        return c;
    }

    // Observation-only log of drop core args. noexcept plain-C so __try is
    // legal (main HookedDropCore can't __try around std::vector-holding
    // C++ locals without C2712).
    struct DropObservation {
        uint32_t selfID;
        uint32_t itemBase;
        uint8_t  itemType;
        int32_t  count;
        float    px, py, pz;
        bool     valid;
    };
    static DropObservation ObserveDropArgs(RE::TESObjectREFR* self,
                                            void* /*out*/,
                                            void* args) noexcept
    {
        DropObservation o{};
        __try {
            if (!self || !args) return o;
            auto* ab = reinterpret_cast<uint8_t*>(args);
            auto* item = *reinterpret_cast<uint8_t**>(ab + 0x00);
            int32_t  count = *reinterpret_cast<int32_t*>(ab + 0x1C);

            o.selfID = RawFormID(self);
            if (item) {
                o.itemBase = RawFormID(item);
                o.itemType = RawFormType(item);
            }
            o.count = count;
            auto* loc = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(self) + 0x8C);
            o.px = loc[0]; o.py = loc[1]; o.pz = loc[2];
            o.valid = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            o.valid = false;
        }
        return o;
    }

    // Drop-core is called for two distinct player-self flows:
    //   (A) Menu-inventory drop → spawn REFR in cell → we want to broadcast
    //   (B) Weapon-fire ammo consumption → internal decrement, no REFR
    //       spawn → must NOT broadcast (would rain ammo on the peer)
    //
    // Discriminator found via ARGS_DUMP comparison (2026-04-25):
    //   Menu: args[+0x18] low 32 = VM-task-id (e.g. 0xfacaec28)
    //         args[+0x20] = 3 (ITEM_TRANSFER_REASON::kDrop)
    //   Fire: args[+0x18] low 32 = 0xFFFFFFFF (-1 sentinel)
    //         args[+0x20] = 0
    //
    // We broadcast only when BOTH match the menu pattern.
    static bool IsMenuDropArgs(void* args) noexcept
    {
        if (!args) return false;
        bool ok = false;
        __try {
            auto* b = reinterpret_cast<uint8_t*>(args);
            int32_t vmHandle = *reinterpret_cast<int32_t*>(b + 0x18);
            int32_t reason   = *reinterpret_cast<int32_t*>(b + 0x20);
            ok = (vmHandle != -1) && (reason == 3);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        return ok;
    }

    // F.2 (2026-04-25): mod-array reader. Reads the kObjectInstance (0x33)
    // ExtraData entry from a REFR's ExtraDataList (REFR+0xC8) and dumps
    // each mod's FormID. Layout reverse-engineered from AttachMod inner
    // FUN_140403910 (REL::ID 40716):
    //   BGSObjectInstanceExtra (size 0x30):
    //     +0x18: uint32 mod_count
    //     +0x20: void* mod_array - array of 16-byte entries
    //       per entry: +0x00 TESForm* (mod), +0x08 (extra: rank? slot?)
    //     +0x28: uint16 (0xFFFF = sentinel?)
    using GetExtraByTypeFn = void* (__fastcall*)(void* extraList, uint32_t type);
    static GetExtraByTypeFn g_getExtraByType = nullptr;  // resolved in Init

    // M.2 V7 (2026-05-08): read mod FormIDs attached to a stack inside a
    // REFR's BGSInventoryList. Used by the AddObjInner hook to capture
    // weapon/armor mods at transfer time so we can replay them on the
    // receiver. Mods on inventory items live in the per-stack ExtraDataList
    // at stack[0] +0x00 (8-byte ptr), then kObjectInstance (type 0x33) on
    // that ExtraDataList - same structure F.2 uses for world REFRs.
    //
    // Layout assumptions (verified empirically against F.2 + existing
    // CollectInventoryEntries):
    //   refr +0xA0 → BGSInventoryList
    //     +0x28 entryCount, +0x30 entries[entryCount] (each 0x28 bytes)
    //       entry +0x00 obj (TESBoundObject*)
    //       entry +0x10 stackCount
    //       entry +0x18 stacks[stackCount] (each 0x10 bytes)
    //         stack +0x00 ExtraDataList* (8 bytes) ← guess; mods may live here
    //         stack +0x08 count
    static void ReadInventoryItemMods(RE::TESObjectREFR* refr,
                                       uint32_t formID,
                                       std::vector<uint32_t>& outMods) noexcept
    {
        outMods.clear();
        if (!refr || !formID || !g_getExtraByType) return;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (!invList) return;
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return;

            for (uint32_t i = 0; i < entryCount; ++i) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (!obj || RawFormID(obj) != formID) continue;

                uint32_t stackCount = *reinterpret_cast<uint32_t*>(entry + 0x10);
                auto* stacksData = *reinterpret_cast<uint8_t**>(entry + 0x18);
                if (!stacksData || stackCount == 0 || stackCount > 1000) return;

                // Try stack[0]'s extra-data ptr at +0x00.
                uint8_t* stack0 = stacksData;
                void* extraList = *reinterpret_cast<void**>(stack0 + 0x00);
                if (!extraList) return;

                void* objInstExtra = g_getExtraByType(extraList, 0x33);  // kObjectInstance
                if (!objInstExtra) return;
                auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
                uint32_t modCount = *reinterpret_cast<uint32_t*>(eb + 0x18);
                void* modArray = *reinterpret_cast<void**>(eb + 0x20);
                if (!modArray || modCount == 0 || modCount > 64) return;

                auto* arr = reinterpret_cast<uint8_t*>(modArray);
                outMods.reserve(modCount);
                for (uint32_t mi = 0; mi < modCount && outMods.size() < kMaxModsPerDrop; ++mi) {
                    auto* mod = *reinterpret_cast<uint8_t**>(arr + mi * 0x10);
                    if (mod) outMods.push_back(RawFormID(mod));
                }
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            outMods.clear();
        }
    }

    // M.2 V16 (2026-05-08): read the kObjectInstance +0x28 sentinel for
    // an inventory item. F.2 docs label this as "0xFFFF when no rank,
    // else primary slot" - engine uses it to render weapon tier (gray
    // vs rare/epic/legendary). Without syncing this, client's proc-gen
    // roll leaves a non-default sentinel even after our V15 modCount
    // truncation, so the weapon still renders as a higher tier.
    static uint16_t ReadInventoryItemModSentinel(RE::TESObjectREFR* refr,
                                                  uint32_t formID) noexcept
    {
        uint16_t result = 0xFFFF;
        if (!refr || !formID || !g_getExtraByType) return result;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (!invList) return result;
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return result;
            for (uint32_t i = 0; i < entryCount; ++i) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (!obj || RawFormID(obj) != formID) continue;
                auto* stacksData = *reinterpret_cast<uint8_t**>(entry + 0x18);
                if (!stacksData) return result;
                void* extraList = *reinterpret_cast<void**>(stacksData + 0x00);
                if (!extraList) return result;
                void* objInstExtra = g_getExtraByType(extraList, 0x33);
                if (!objInstExtra) return result;
                auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
                result = *reinterpret_cast<uint16_t*>(eb + 0x28);
                return result;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { result = 0xFFFF; }
        return result;
    }

    // F.2 ReadModFormIDs: returns the base FormIDs of all mods attached to
    // a REFR via its kObjectInstance ExtraData. Layout RE'd from AttachMod
    // inner FUN_140403910:
    //   BGSObjectInstanceExtra (size 0x30):
    //     +0x18 uint32 mod_count
    //     +0x20 void*  mod_array (entries each 0x10 bytes)
    //       per entry: +0x00 TESForm* mod, +0x08 uint64 extra (rank|ptr packed)
    //     +0x28 uint16 sentinel (0xFFFF when no rank, else primary slot)
    static void ReadModFormIDs(RE::TESObjectREFR* refr,
                                std::vector<uint32_t>& outMods) noexcept
    {
        outMods.clear();
        if (!refr || !g_getExtraByType) return;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return;
            void* objInstExtra = g_getExtraByType(extraList, 0x33);  // kObjectInstance
            if (!objInstExtra) return;
            auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
            uint32_t modCount = *reinterpret_cast<uint32_t*>(eb + 0x18);
            void* modArray = *reinterpret_cast<void**>(eb + 0x20);
            if (!modArray || modCount == 0 || modCount > 64) return;

            auto* arr = reinterpret_cast<uint8_t*>(modArray);
            outMods.reserve(modCount);
            for (uint32_t i = 0; i < modCount && outMods.size() < kMaxModsPerDrop; ++i) {
                auto* mod = *reinterpret_cast<uint8_t**>(arr + i * 0x10);
                if (mod) outMods.push_back(RawFormID(mod));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            outMods.clear();
        }
    }

    // F.2 dev-aid: detailed dump of kObjectInstance entries for layout
    // verification. Logs the BSTArray header (size+capacity) plus per-entry
    // (form FormID + 8-byte extra). Goal: distinguish user-attached mods
    // from base default-loadout slots via the extra field pattern.
    static void DumpModEntriesVerbose(RE::TESObjectREFR* refr) noexcept
    {
        if (!refr || !g_getExtraByType) return;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) {
                PLOG_INFO("[PULSAR] F.2-DBG: refr=0x{:08X} no ExtraDataList"sv,
                          RawFormID(refr));
                return;
            }
            void* objInstExtra = g_getExtraByType(extraList, 0x33);
            if (!objInstExtra) {
                PLOG_INFO("[PULSAR] F.2-DBG: refr=0x{:08X} no kObjectInstance entry"sv,
                          RawFormID(refr));
                return;
            }
            auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
            uint32_t size     = *reinterpret_cast<uint32_t*>(eb + 0x18);
            uint32_t capacity = *reinterpret_cast<uint32_t*>(eb + 0x1C);
            void* dataPtr     = *reinterpret_cast<void**>(eb + 0x20);
            uint16_t sentinel = *reinterpret_cast<uint16_t*>(eb + 0x28);
            PLOG_INFO(
                "[PULSAR] F.2-DBG: refr=0x{:08X} BSTArray size={} cap={} data=0x{:x} sentinel=0x{:04x}"sv,
                RawFormID(refr), size, capacity,
                reinterpret_cast<uintptr_t>(dataPtr), sentinel);
            if (!dataPtr || size == 0 || size > 64) return;
            auto* arr = reinterpret_cast<uint8_t*>(dataPtr);
            for (uint32_t i = 0; i < size; ++i) {
                auto* mod = *reinterpret_cast<uint8_t**>(arr + i * 0x10);
                uint64_t extra = *reinterpret_cast<uint64_t*>(arr + i * 0x10 + 0x08);
                if (mod) {
                    PLOG_INFO(
                        "[PULSAR] F.2-DBG[{}]: form=0x{:08X} extra=0x{:016x} {}"sv,
                        i, RawFormID(mod), extra,
                        extra == 0 ? "<-- candidate user-mod (extra=0)" : "");
                } else {
                    PLOG_INFO("[PULSAR] F.2-DBG[{}]: NULL"sv, i);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_INFO("[PULSAR] F.2-DBG: SEH"sv);
        }
    }

    // F.2 helper: read kObjectInstance.size from a REFR. SEH-wrapped
    // noexcept so callers in C++-unwinding contexts can use the value.
    static uint32_t SafeReadModSlotCount(RE::TESObjectREFR* refr) noexcept
    {
        if (!refr || !g_getExtraByType) return 0;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return 0;
            void* objExtra = g_getExtraByType(extraList, 0x33);
            if (!objExtra) return 0;
            return *reinterpret_cast<uint32_t*>(
                reinterpret_cast<uint8_t*>(objExtra) + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // F.2 SafeReplaceModArrayInPlace: skip AttachMod entirely. Spawned REFR
    // has a kObjectInstance entry from engine init with receiver's default
    // mod loadout. We OVERWRITE the form pointers in-place to match the
    // host's mod combination - no AttachMod side-effects, no UI race.
    //
    // Constraints:
    // Receiver's local mod-slot count must match host's (= same base
    //     form). Otherwise we skip (proc-gen variants, mismatched mods).
    // Mod base FormIDs must already be cross-plugin resolved before
    //     calling this.
    // We only touch entry+0x00 (form*), leaving extras (+0x08) intact
    //     to preserve runtime state (durability, instance-data ptr).
    static void SafeReplaceModArrayInPlace(RE::TESObjectREFR* refr,
                                            const std::vector<uint32_t>& mods,
                                            uint16_t sentinel = 0xFFFF) noexcept
    {
        if (!refr || mods.empty() || !g_getExtraByType) return;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return;
            void* objInstExtra = g_getExtraByType(extraList, 0x33);
            if (!objInstExtra) return;
            auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
            uint32_t localCount = *reinterpret_cast<uint32_t*>(eb + 0x18);
            void* localArray = *reinterpret_cast<void**>(eb + 0x20);
            if (!localArray || localCount == 0 || localCount > 64) return;
            // Note: localCount may differ from mods.size() - happens when
            // host's weapon has legendary effects (extra mod slots from
            // proc-gen rolling). We replace common slots in-place; extras
            // (host > local) are handled by caller via AttachMod.
            if (localCount != mods.size()) {
                PLOG_INFO(
                    "[PULSAR] F.2 V1 in-place: slot-count differs refr=0x{:08X} local={} host={} - replacing common slots only"sv,
                    RawFormID(refr), localCount,
                    static_cast<uint32_t>(mods.size()));
            }

            auto* arr = reinterpret_cast<uint8_t*>(localArray);
            size_t replaced = 0;
            uint32_t commonSlots = std::min(localCount,
                static_cast<uint32_t>(mods.size()));
            for (uint32_t i = 0; i < commonSlots; ++i) {
                auto* mod = RE::TESForm::LookupByID(mods[i]);
                if (mod) {
                    *reinterpret_cast<void**>(arr + i * 0x10) = mod;
                    ++replaced;
                }
            }
            // M.2 V15 (2026-05-08): same fix as inventory variant - when
            // client's spawn proc-gen rolled MORE slots than host has
            // (gray-weapon drops have host=13 default mods, client spawn
            // rolled 16 with random legendaries), truncate modCount so the
            // engine reads only the host-mirrored prefix. Excess slot
            // memory leaks but visuals/stats match host.
            uint32_t hostCount = static_cast<uint32_t>(mods.size());
            if (hostCount < localCount) {
                *reinterpret_cast<uint32_t*>(eb + 0x18) = hostCount;
                PLOG_INFO(
                    "[PULSAR] F.2 V15 mod-trunc: refr=0x{:08X} {}->{} (truncated proc-gen extras)"sv,
                    RawFormID(refr), localCount, hostCount);
            }
            // M.2 V16: also write +0x28 sentinel (item tier indicator).
            uint16_t prevSentinel = *reinterpret_cast<uint16_t*>(eb + 0x28);
            *reinterpret_cast<uint16_t*>(eb + 0x28) = sentinel;
            PLOG_INFO(
                "[PULSAR] F.2 V16 sentinel: refr=0x{:08X} 0x{:04X}->0x{:04X}"sv,
                RawFormID(refr), prevSentinel, sentinel);
            PLOG_INFO(
                "[PULSAR] F.2 V1 in-place: replaced {}/{} mod slots on refr=0x{:08X}"sv,
                replaced, commonSlots, RawFormID(refr));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] F.2 V1 in-place SEH on refr=0x{:08X}"sv,
                      RawFormID(refr));
        }
    }

    // M.2 V20 (2026-05-09): noexcept wrapper for full inventory snapshot
    // of a REFR's +0xA0 list. Used by the receiver place path to detect
    // engine auto-spawned items (NPC-loadout default ammo) by comparing
    // pre/post AddObjInner. C2712-free so the caller (DrainPendingPickups)
    // can hold C++ objects.
    static int32_t SafeSnapshotRefrInventory(RE::TESObjectREFR* refr,
                                              SnapshotEntry* outBuf,
                                              int32_t outCap) noexcept
    {
        if (!refr || !outBuf || outCap <= 0) return 0;
        int32_t n = 0;
        __try {
            auto* invList = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(refr) + 0xA0);
            if (invList) {
                n = CollectInventoryEntries(invList, outBuf, outCap);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
        return n;
    }

    // M.2 V7 (2026-05-08): mirror of SafeReplaceModArrayInPlace but for
    // INVENTORY items. Walks refr->+0xA0 BGSInventoryList, finds entry by
    // formID, gets stack[0]'s ExtraDataList ptr, looks up kObjectInstance
    // and overwrites the mod slot array in-place. Mods on inventory items
    // live there (not on refr's main +0xC8 ExtraDataList).
    static void SafeReplaceModArrayOnInventoryItem(
        RE::TESObjectREFR* refr,
        uint32_t formID,
        const std::vector<uint32_t>& mods,
        uint16_t sentinel = 0xFFFF) noexcept
    {
        if (!refr || !formID || mods.empty() || !g_getExtraByType) return;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (!invList) return;
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return;

            for (uint32_t i = 0; i < entryCount; ++i) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (!obj || RawFormID(obj) != formID) continue;

                uint32_t stackCount = *reinterpret_cast<uint32_t*>(entry + 0x10);
                auto* stacksData = *reinterpret_cast<uint8_t**>(entry + 0x18);
                if (!stacksData || stackCount == 0 || stackCount > 1000) return;

                uint8_t* stack0 = stacksData;
                void* extraList = *reinterpret_cast<void**>(stack0 + 0x00);
                if (!extraList) return;
                void* objInstExtra = g_getExtraByType(extraList, 0x33);
                if (!objInstExtra) return;

                auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
                uint32_t localCount = *reinterpret_cast<uint32_t*>(eb + 0x18);
                void* localArray = *reinterpret_cast<void**>(eb + 0x20);
                if (!localArray || localCount == 0 || localCount > 64) return;

                auto* arr = reinterpret_cast<uint8_t*>(localArray);
                uint32_t commonSlots = std::min(localCount,
                    static_cast<uint32_t>(mods.size()));
                size_t replaced = 0;
                for (uint32_t mi = 0; mi < commonSlots; ++mi) {
                    auto* mod = RE::TESForm::LookupByID(mods[mi]);
                    if (mod) {
                        *reinterpret_cast<void**>(arr + mi * 0x10) = mod;
                        ++replaced;
                    }
                }
                // M.2 V15 (2026-05-08): truncate modCount to host's exact
                // count. AddObjInner with src=NULL on receiver triggers
                // engine proc-gen rolls - client gets MORE slots than host
                // (e.g. local=16, host=13 → 3 random legendary effects in
                // slots 13..15). After replacing the first 13, force the
                // engine to only see 13 by lowering modCount. Excess slot
                // memory leaks (no realloc path) but visuals/stats match
                // host's loadout.
                uint32_t hostCount = static_cast<uint32_t>(mods.size());
                if (hostCount < localCount) {
                    *reinterpret_cast<uint32_t*>(eb + 0x18) = hostCount;
                    PLOG_INFO(
                        "[PULSAR] M.2 V15 mod-trunc: refr=0x{:08X} item=0x{:08X} {}->{} (truncated proc-gen extras)"sv,
                        RawFormID(refr), formID, localCount, hostCount);
                }
                // M.2 V16: copy host's +0x28 sentinel (item tier rendering).
                uint16_t prevSentinel = *reinterpret_cast<uint16_t*>(eb + 0x28);
                *reinterpret_cast<uint16_t*>(eb + 0x28) = sentinel;
                PLOG_INFO(
                    "[PULSAR] M.2 V16 sentinel: refr=0x{:08X} item=0x{:08X} 0x{:04X}->0x{:04X}"sv,
                    RawFormID(refr), formID, prevSentinel, sentinel);
                PLOG_INFO(
                    "[PULSAR] M.2 V7 inv-mod replace: refr=0x{:08X} item=0x{:08X} replaced={}/{} (host_mods={})"sv,
                    RawFormID(refr), formID, replaced, commonSlots,
                    static_cast<uint32_t>(mods.size()));
                return;
            }
            PLOG_INFO(
                "[PULSAR] M.2 V7 inv-mod replace: SKIP - no entry for item=0x{:08X} on refr=0x{:08X}"sv,
                formID, RawFormID(refr));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN(
                "[PULSAR] M.2 V7 inv-mod replace SEH: refr=0x{:08X} item=0x{:08X}"sv,
                RawFormID(refr), formID);
        }
    }

    // F.2 SafeAttachMod: SEH-wrapped per-mod attach. Used on receiver after
    // spawning the dropped REFR to reproduce the host's mod combination.
    static void SafeAttachMod(RE::TESObjectREFR* refr, RE::TESForm* mod) noexcept
    {
        if (!g_attachMod || !refr || !mod) return;
        __try {
            // rank=0 (no specific legendary rank), flags=1 (default attach).
            g_attachMod(refr, mod, 0, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // If the engine rejects (e.g., incompatible mod slot for this
            // base form), the SEH catches and we move to the next mod.
        }
    }


    static void SendDropPacket(uint32_t baseFormID, uint8_t formType,
                               int32_t count,
                               const std::vector<uint32_t>& mods,
                               uint16_t modSentinel = 0xFFFF);

    // M.2 V16: read sentinel (+0x28 of kObjectInstance on a world REFR's
    // main ExtraDataList at +0xC8). Same purpose as the inventory variant.
    static uint16_t ReadWorldRefrModSentinel(RE::TESObjectREFR* refr) noexcept
    {
        uint16_t result = 0xFFFF;
        if (!refr || !g_getExtraByType) return result;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return result;
            void* objInstExtra = g_getExtraByType(extraList, 0x33);
            if (!objInstExtra) return result;
            auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
            result = *reinterpret_cast<uint16_t*>(eb + 0x28);
        } __except (EXCEPTION_EXECUTE_HANDLER) { result = 0xFFFF; }
        return result;
    }

    // F.2 forward decls - used in HookedDropCore which is above definitions.
    static RE::TESObjectREFR* SafeFindLootRefInCell(void* cell,
                                                     uint32_t resolvedBase,
                                                     float px, float py, float pz,
                                                     float radius);
    static void ReadModFormIDs(RE::TESObjectREFR* refr,
                                std::vector<uint32_t>& outMods) noexcept;
    static void DumpModEntriesVerbose(RE::TESObjectREFR* refr) noexcept;
    static RE::TESObjectREFR* FindSpawnedRefNearActor(
        RE::TESObjectREFR* anchor, uint32_t baseID) noexcept;

    // F.2 deferred-read queue: HookedDropCore CANNOT touch kObjectInstance
    // synchronously - doing so on Starfield's main thread races with the
    // Scaleform inventory UI re-render and crashes the host (verified
    // 2026-04-25). Instead we record the drop here and let
    // DrainPendingDropReads pick it up on the next game tick, after the
    // UI has settled.
    struct PendingDropRead {
        uint32_t itemBase;
        uint8_t  itemType;
        int32_t  count;
        float    px, py, pz;
    };
    static std::mutex                       s_pendingDropReadsMutex;
    static std::vector<PendingDropRead>     s_pendingDropReads;

    void __fastcall HookedDropCore(RE::TESObjectREFR* self, void* out, void* args)
    {
        DropObservation o = ObserveDropArgs(self, out, args);
        bool shouldQueue = o.valid && o.selfID == 0x14
            && !t_replayingFromPacket && o.itemBase != 0
            && o.count > 0 && IsMenuDropArgs(args);

        // Queue BEFORE running orig - running orig first then doing
        // anything (mutex lock, queue push) appears to race with the
        // Scaleform inventory-UI re-render and crashes the host.
        // Original F.1.d pattern (TX-before-orig) never crashed; preserve
        // that. The deferred mod readout happens on next game tick anyway.
        if (shouldQueue) {
            auto& net = Pulsar::Network::GetSingleton();
            if (net.IsConnected()) {
                std::lock_guard g(s_pendingDropReadsMutex);
                s_pendingDropReads.push_back({
                    o.itemBase, o.itemType, o.count, o.px, o.py, o.pz
                });
            }
        }

        if (g_origDropCore) g_origDropCore(self, out, args);
    }

    // ---------------------------------------------------------------------
    // F.12 (2026-06-30): host-authoritative NPC-AI-driven door opens.
    //
    // ClassifyActivation deliberately drops kACHR (actor) activators on doors
    // (see its F.10 note) so ambient NPCs walking through doors don't spam the
    // wire. But in co-op the NPC that opens a door FOR the player (tutorial
    // guides, quest scripts, AI packages) is host-driven, so the door never
    // opens on the client and the partner is stuck in front of it. Confirmed
    // in the 2026-06-30 tutorial trace: Lin (0x00005639) activated door
    // 0x001ED329 with no broadcast.
    //
    // We re-admit NPC door opens under tight gates so they ride the EXISTING
    // DoorSync broadcast + clone-replay path (no new packet/RX code):
    // host only (NPCs are host-authoritative; avoids client AI divergence
    //     and a both-sides ping-pong)
    // not replaying (echo guard)
    // activator is an actor (kACHR) other than the local player (player
    //     doors already sync via cls.isDoor)
    // door is near the local player (bounds ambient spam to doors the
    //     co-op partner could actually be blocked by)
    // door is NOT a load/teleport door - replaying a load-door on the
    //     client teleports the clone into the linked cell; regular doors just
    //     play an open animation, which is what we want mirrored
    // per-door debounced (a guide NPC re-opening / walk-through repeats
    //     must not flood the wire)
    // ---------------------------------------------------------------------
    static std::mutex                              s_npcDoorMutex;
    static std::unordered_map<uint32_t, uint64_t>  s_npcDoorLastMs;  // doorRefID -> last broadcast tick
    static constexpr float    kNpcDoorRadius2    = 3000.0f * 3000.0f;  // ~30 m around the player
    static constexpr uint64_t kNpcDoorDebounceMs = 2000;

    // noexcept: door is near the local player AND is a regular (non-teleport)
    // door. Raw-offset reads only, so an SEH from a torn REFR yields "not
    // eligible" instead of crashing the activate path.
    static bool SafeNpcDoorEligible(RE::TESObjectREFR* door) noexcept
    {
        __try {
            if (!door) return false;
            auto* plr = RE::PlayerCharacter::GetSingleton();
            if (!plr) return false;
            auto* dloc = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(door) + 0x8C);
            auto* ploc = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(plr) + 0x8C);
            float dx = dloc[0] - ploc[0];
            float dy = dloc[1] - ploc[1];
            float dz = dloc[2] - ploc[2];
            if (dx * dx + dy * dy + dz * dz > kNpcDoorRadius2) return false;
            // ExtraTeleport (type 0x29) present → load door → skip.
            void* extraList = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(door) + 0xC8);
            if (extraList && g_getExtraByType) {
                void* xt = g_getExtraByType(extraList, 0x29);
                if (xt) return false;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // noexcept: activator is an actor (kACHR) other than the local player.
    static bool SafeActivatorIsNonPlayerActor(void* activator) noexcept
    {
        __try {
            if (!activator) return false;
            if (RawFormType(activator) != static_cast<uint8_t>(RE::FormType::kACHR))
                return false;
            return RawFormID(activator) != 0x14u;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Per-door debounce. Kept in its own function because HookedActivate has
    // __try blocks and so cannot also hold a std::lock_guard (C2712). Returns
    // true (and records the tick) only if this door hasn't broadcast inside
    // the debounce window.
    static bool TryClaimNpcDoorBroadcast(uint32_t doorRefID)
    {
        const uint64_t now = ::GetTickCount64();
        std::lock_guard g(s_npcDoorMutex);
        auto it = s_npcDoorLastMs.find(doorRefID);
        if (it != s_npcDoorLastMs.end() && now - it->second <= kNpcDoorDebounceMs)
            return false;
        s_npcDoorLastMs[doorRefID] = now;
        return true;
    }

    bool __fastcall HookedActivate(RE::TESObjectREFR* self, ActivateArgs* args)
    {
        // L.6 V3 Phase 3i v17 intercept: if this is the cockpit-side hatch and
        // we're waiting for host's forward-bridge save, suppress activate so
        // client doesn't teleport to local FF instance with wrong ship visible.
        if (Pulsar::DockSync::TryHandleCockpitSideHatchActivate(self)) {
            return false;
        }

        // L.6 V3 Phase 3f intercept: if this is the station-side hatch we
        // configured for return-to-ship, redirect via Papyrus MoveTo and
        // skip the engine teleport (which crashes from null+0x4C deref on
        // an unloaded cockpit cell's destDoorRef).
        if (Pulsar::DockSync::TryHandleStationSideHatchActivate(self)) {
            return false;
        }

        // Client walking into the vehicle the host is driving: formal entry
        // crashes the client, so refuse it (SharedVehicleRide, RidePolicy.h).
        if (args && args->activator == RE::PlayerCharacter::GetSingleton() &&
            Pulsar::VehicleSync::ShouldBlockClientEntry(self)) {
            return false;
        }

        // Way-A medpack revive: the local player activates the downed
        // partner-clone → send the revive and consume the activation (no talk
        // dialog on a downed player). Robust against rebinds/controller, unlike
        // the GetAsyncKeyState tap.
        if (args && Pulsar::Coop::Mortality::OnActivateMaybeRevive(self, args->activator)) {
            return false;
        }

        // L.6 V1.2 - Dock-Probe-Window: log ANY Activate while we're
        // capturing a dock sequence. The Hatch is a REFR activation, type
        // unknown - log everything so we can identify it from logs.
        // L.6 V2 (2026-05-02): when the activated REFR is a kFURN inside the
        // cockpit cell, treat as Hatch-Activate and arm cell-change watchdog.
        if (Pulsar::TakeoffProbe::IsDockProbeActive() && self) {
            __try {
                std::uint32_t selfID = RawFormID(self);
                std::uint8_t  selfFt = RawFormType(self);
                auto* baseRaw = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(self) + 0x98);
                std::uint32_t baseID = baseRaw ? RawFormID(baseRaw) : 0;
                std::uint8_t  baseFt = baseRaw ? RawFormType(baseRaw) : 0;
                Pulsar::TakeoffProbe::LogActivateDuringDockProbe(selfID, baseID, baseFt);
                PLOG_INFO(
                    "[PULSAR] L.6 V1.2 DockProbe ⚡ Activate self=0x{:08X} selfFt=0x{:02X} "
                    "base=0x{:08X} baseFt=0x{:02X} replay={}"sv,
                    selfID, selfFt, baseID, baseFt,
                    t_replayingFromPacket ? "YES" : "no");

                // V2 hatch-detect REMOVED - see L.6 cleanup (2026-05-02).
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // Classify in a noexcept helper (can't mix __try with std::vector-
        // holding locals here in C++-unwinding mode).
        ActivationClass cls = ClassifyActivation(self, args, t_replayingFromPacket);

        // F.10: a local player terminal use opens the chained door/acti window -
        // terminal scripts activate their targets (doors, turrets, lights) with
        // a non-actor activator, which ClassifyActivation otherwise drops.
        if (!t_replayingFromPacket && args && args->activator
            && cls.baseType == static_cast<uint8_t>(RE::FormType::kTERM)
            && RawFormID(args->activator) == 0x14u) {
            s_lastPlayerTermUseMs.store(::GetTickCount64(), std::memory_order_relaxed);
            PLOG_INFO("[PULSAR] F.10 terminal use noted: ref=0x{:08X} (door/acti chain window {}s)"sv,
                      cls.selfID, kTermChainWindowMs / 1000);
        }

        uint32_t selfID    = cls.selfID;
        uint32_t baseID    = cls.baseID;
        uint8_t  baseType  = cls.baseType;
        float    px = cls.px, py = cls.py, pz = cls.pz;
        bool     eligible  = cls.eligibleLoot;
        bool     isContainerActivation = cls.isContainer;

        // F.12: host-authoritative NPC-driven door open (see helper block
        // above). Computed pre-orig; broadcast post-orig alongside the player
        // door path. Gated to doors the co-op partner could be blocked by and
        // debounced so a guide NPC's repeats don't flood the wire.
        bool npcDoorOpen = false;
        if (!t_replayingFromPacket
            && cls.baseType == static_cast<uint8_t>(RE::FormType::kDOOR)
            && !cls.isDoor                     // player/chained path already covers these
            && args && args->activator
            && Pulsar::Network::GetSingleton().IsHost()
            && SafeActivatorIsNonPlayerActor(args->activator)
            && SafeNpcDoorEligible(self)) {
            npcDoorOpen = TryClaimNpcDoorBroadcast(cls.selfID);
        }
        if (npcDoorOpen) {
            PLOG_INFO(
                "[PULSAR] F.12 NPC door-open: ref=0x{:08X} base=0x{:08X} byNPC=0x{:08X} @({:.0f},{:.0f},{:.0f})"sv,
                cls.selfID, cls.baseID,
                args && args->activator ? RawFormID(args->activator) : 0u,
                cls.px, cls.py, cls.pz);
        }

        // Install our event-sink on the container's inventoryList on first
        // activation. Item transfers happen AFTER Activate returns (via a
        // UI menu callback), so a synchronous pre/post diff around orig
        // sees no change. The sink fires at the true transfer moment.
        if (isContainerActivation) {
            RegisterSinkOnContainer(self);
        }

        // F.3 raw door-hit probe - fires for ANY door Activate (player, NPC,
        // AI package, script). F.7 V3 extension: also fires for kACTI
        // (Activator) so we can trace the switch→conduit→door script chain
        // and identify which Activate calls fail the player-only filter.
        if (self && args) {
            uint8_t selfFormType = RawFormType(self);
            auto* baseRaw = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(self) + 0x98);
            uint8_t baseFormType = baseRaw ? RawFormType(baseRaw) : 0;
            bool isDoor = baseFormType == static_cast<uint8_t>(RE::FormType::kDOOR);
            bool isActi = baseFormType == static_cast<uint8_t>(RE::FormType::kACTI);
            bool isCont = baseFormType == static_cast<uint8_t>(RE::FormType::kCONT);
            // V2 anti-jitter: note every LOCAL (non-replay) door activation so
            // DoorSync's mirror can skip re-toggling a door the client's own
            // deterministic NPC already opened (open-then-immediately-closed).
            if (isDoor && !t_replayingFromPacket) {
                Pulsar::WorldSync::Door::NoteLocalDoorActivation(RawFormID(self));
            }
            if (isDoor || isActi || isCont) {
                uint32_t actorID = args->activator
                    ? RawFormID(args->activator) : 0;
                uint8_t actorFormType = args->activator
                    ? RawFormType(args->activator) : 0;
                const char* tname = isDoor ? "DOOR" : isActi ? "ACTI" : "CONT";
                PLOG_INFO(
                    "[PULSAR] F.8 probe: type={} refID=0x{:08X} refType=0x{:02X} baseID=0x{:08X} byActor=0x{:08X} actorType=0x{:02X} replaying={}"sv,
                    tname,
                    RawFormID(self), selfFormType,
                    baseRaw ? RawFormID(baseRaw) : 0,
                    actorID, actorFormType,
                    t_replayingFromPacket ? "YES" : "no");

                // L.6 V3 Phase 3f probe: for kDOOR activations dump the
                // ExtraTeleport bytes pre-orig - empirical capture of what
                // engine sees when host activates station-side hatch (the
                // ground-truth for our forging on client). Logs:
                //   destDoorRef ptr+FormID, flags, destPos, destRot, destFlag.
                if (isDoor) {
                    __try {
                        auto* pb = reinterpret_cast<uint8_t*>(self);
                        void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
                        if (extraList) {
                            // Reuse REL::ID 37878 GetExtraByType resolver.
                            using GetExtraFn = void* (__fastcall*)(void*, uint32_t);
                            static GetExtraFn s_getExFn = nullptr;
                            if (!s_getExFn) {
                                s_getExFn = reinterpret_cast<GetExtraFn>(
                                    REL::Relocation<std::uintptr_t>{
                                        REL::ID(37878) }.address());
                            }
                            void* xt = s_getExFn ? s_getExFn(extraList, 0x29) : nullptr;
                            if (xt) {
                                uint64_t innerPtr = *reinterpret_cast<uint64_t*>(
                                    reinterpret_cast<uint8_t*>(xt) + 0x18);
                                if (innerPtr >= 0x10000ull
                                    && innerPtr < 0x00007FFFFFFFFFFFull) {
                                    auto* in = reinterpret_cast<uint8_t*>(innerPtr);
                                    uint64_t destRefPtr = *reinterpret_cast<uint64_t*>(in + 0x00);
                                    uint32_t destRefFmt = 0, destBaseFmt = 0, destCellFmt = 0;
                                    if (destRefPtr >= 0x10000ull
                                        && destRefPtr < 0x00007FFFFFFFFFFFull) {
                                        __try {
                                            auto* destRef = reinterpret_cast<RE::TESObjectREFR*>(destRefPtr);
                                            destRefFmt = destRef->formID;
                                            if (auto* db = destRef->data.objectReference.get())
                                                destBaseFmt = db->formID;
                                            if (auto* dc = destRef->parentCell)
                                                destCellFmt = dc->formID;
                                        } __except (EXCEPTION_EXECUTE_HANDLER) {}
                                    }
                                    uint32_t flags = *reinterpret_cast<uint32_t*>(in + 0x08);
                                    float dpx = *reinterpret_cast<float*>(in + 0x0C);
                                    float dpy = *reinterpret_cast<float*>(in + 0x10);
                                    float dpz = *reinterpret_cast<float*>(in + 0x14);
                                    float drx = *reinterpret_cast<float*>(in + 0x18);
                                    float dry = *reinterpret_cast<float*>(in + 0x1C);
                                    float drz = *reinterpret_cast<float*>(in + 0x20);
                                    uint8_t df = *reinterpret_cast<uint8_t*>(in + 0x24);
                                    PLOG_INFO(
                                        "[PULSAR] F.8 DOOR-XT-PROBE: ref=0x{:08X} "
                                        "outer=0x{:X} inner=0x{:X} destRef=0x{:08X} (ptr=0x{:X}) "
                                        "destBase=0x{:08X} destCell=0x{:08X} "
                                        "flags=0x{:X} pos=({:.2f},{:.2f},{:.2f}) "
                                        "rot=({:.2f},{:.2f},{:.2f}) destFlag=0x{:02X}"sv,
                                        RawFormID(self),
                                        reinterpret_cast<uintptr_t>(xt), innerPtr,
                                        destRefFmt, destRefPtr,
                                        destBaseFmt, destCellFmt,
                                        flags, dpx, dpy, dpz, drx, dry, drz, df);
                                }
                            } else {
                                PLOG_INFO(
                                    "[PULSAR] F.8 DOOR-XT-PROBE: ref=0x{:08X} no ExtraTeleport"sv,
                                    RawFormID(self));
                            }
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        PLOG_WARN("[PULSAR] F.8 DOOR-XT-PROBE: SEH"sv);
                    }
                }

                // F.8 V4 probe: when activating a display-case container
                // (NativeTerminal:DisplayCaseScript handled - uses kLinkedRef
                // chain to manage visual children), walk the case's
                // ExtraDataList linked list and dump types + raw bytes for
                // kLinkedRef (0x4C) and kLinkedRefChildren (0x4D) entries.
                // Goal: identify how to read the linked-children REFR list
                // so we can target visuals directly instead of base+pos
                // matcher. One-shot per session.
                static int s_caseExtraDumps = 0;
                uint32_t caseBaseID = baseRaw ? RawFormID(baseRaw) : 0;
                if (isCont && caseBaseID == 0x00246226 && s_caseExtraDumps < 2) {
                    ++s_caseExtraDumps;
                    __try {
                        auto* edl = *reinterpret_cast<uint8_t**>(
                            reinterpret_cast<uint8_t*>(self) + 0xC8);
                        PLOG_INFO("[PULSAR] case-extras: ref=0x{:08X} edlPtr=0x{:x}"sv,
                                  RawFormID(self),
                                  reinterpret_cast<uintptr_t>(edl));
                        if (edl) {
                            // BaseExtraList @ +0x08, m_head @ +0
                            auto* bel = edl + 0x08;
                            auto* head = *reinterpret_cast<uint8_t**>(bel + 0);
                            int n = 0;
                            while (head && n < 64) {
                                // BSExtraData: vtable @0, next @8, flags @0x10, type @0x12
                                uint8_t  type = head[0x12];
                                auto* next = *reinterpret_cast<uint8_t**>(head + 0x08);
                                if (type == 0x4C || type == 0x4D) {
                                    // Decoded layout (verified via dump):
                                    //   +0x18: uint64 count
                                    //   +0x20: uint64 capacity
                                    //   +0x28: REFR* child[0]  (inline if cap<=2)
                                    //   +0x30: REFR* child[1]
                                    uint64_t count = *reinterpret_cast<uint64_t*>(head + 0x18);
                                    uint64_t cap   = *reinterpret_cast<uint64_t*>(head + 0x20);
                                    const char* tname2 = (type == 0x4C) ? "kLinkedRef" : "kLinkedRefChildren";
                                    PLOG_INFO("[PULSAR] case-extras: type=0x{:02X} ({}) count={} cap={}"sv,
                                              type, tname2, count, cap);
                                    // skip raw slot dump - moved focus to STAT scan
                                    (void)count; (void)cap;
                                } else {
                                    PLOG_INFO("[PULSAR] case-extras: type=0x{:02X} addr=0x{:x}"sv,
                                              type,
                                              reinterpret_cast<uintptr_t>(head));
                                }
                                head = next;
                                ++n;
                            }
                            PLOG_INFO("[PULSAR] case-extras: dumped {} entries"sv, n);
                        }
                        // Scan nearby kSTAT FF REFRs (script-spawned visual
                        // decorations) within tight radius of case origin.
                        // iter 7 neighbour dump found 5 such REFRs at dist
                        // 7-9 from a display-case - these ARE the visuals
                        // we need to cull (NOT the FF refs with item base).
                        auto* selfLoc = reinterpret_cast<float*>(
                            reinterpret_cast<uint8_t*>(self) + 0x8C);
                        float cx = selfLoc[0], cy = selfLoc[1], cz = selfLoc[2];
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        if (player && player->parentCell) {
                            auto* cell = player->parentCell;
                            auto* cb = reinterpret_cast<uint8_t*>(cell);
                            uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
                            auto** ddata = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
                            if (ddata && size > 0 && size < 10000) {
                                int statHits = 0;
                                for (uint32_t i = 0; i < size && statHits < 10; ++i) {
                                    auto* ref = ddata[i];
                                    if (!ref) continue;
                                    auto* base2 = *reinterpret_cast<uint8_t**>(
                                        reinterpret_cast<uint8_t*>(ref) + 0x98);
                                    if (!base2) continue;
                                    if (RawFormType(base2) != static_cast<uint8_t>(RE::FormType::kSTAT)) continue;
                                    uint32_t rid = RawFormID(ref);
                                    if ((rid & 0xFF000000) != 0xFF000000) continue;  // FF only
                                    auto* loc = reinterpret_cast<float*>(
                                        reinterpret_cast<uint8_t*>(ref) + 0x8C);
                                    float dx = loc[0]-cx, dy = loc[1]-cy, dz = loc[2]-cz;
                                    float d = std::sqrt(dx*dx+dy*dy+dz*dz);
                                    if (d > 30.0f) continue;
                                    PLOG_INFO(
                                        "[PULSAR] case-stat: ref=0x{:08X} base=0x{:08X} dist={:.1f}"sv,
                                        rid, RawFormID(base2), d);
                                    ++statHits;
                                }
                                PLOG_INFO("[PULSAR] case-stat: total {} kSTAT-FF found within 30 units"sv, statHits);
                            }
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        PLOG_WARN("[PULSAR] case-extras: SEH walking ExtraDataList"sv);
                    }
                }
            }
        }

        // F.1.e probe (2026-04-25): when an NPC (TESNPC base) is activated,
        // check if REFR+0xA0 is populated and try registering the same
        // BGSInventoryListEvent sink. Goal: empirically confirm whether
        // dead-NPC corpse-looting funnels through the same event path as
        // CONT-containers, or needs a separate Ghidra-trail (BGSCorpseEvent).
        if (cls.isNPC) {
            ContainerRawInfo info = ReadContainerRaw(self);
            PLOG_INFO(
                "[PULSAR] F.1.e probe: NPC activate ref=0x{:08X} base=0x{:08X} invListSrc=0x{:x} ({})"sv,
                cls.selfID, cls.baseID,
                reinterpret_cast<uintptr_t>(info.invListSrc),
                info.invListSrc ? "non-null - attempting sink registration"
                                : "NULL - inventory event source not at +0xA0 on actors");
            if (info.invListSrc) {
                RegisterSinkOnContainer(self);
            }
        }

        // F.7 V2: pre-Activate snapshot of player inventory for kACTI so
        // we can diff post-orig and detect any consumed item (power-cell
        // for power-conduits etc). Cleared either way after dispatch.
        t_actiPreSnapCount = 0;
        if (cls.isActi) {
            auto* plr = RE::PlayerCharacter::GetSingleton();
            if (plr) {
                uint8_t* plrInv = nullptr;
                __try {
                    plrInv = *reinterpret_cast<uint8_t**>(
                        reinterpret_cast<uint8_t*>(plr) + 0xA0);
                } __except (EXCEPTION_EXECUTE_HANDLER) { plrInv = nullptr; }
                if (plrInv) {
                    t_actiPreSnapCount = CollectInventoryEntries(
                        plrInv, t_actiPreSnap, kActiSnapMax);
                }
            }
        }

        // Phase K.2 V2 - note door activation by player so TransitSync
        // can attach the source-door FormID to the next outgoing
        // PKT_TRANSIT. Receiver-side auto-follow calls
        // Player.Activate(door) → engine handles cell-load correctly
        // for interior destinations. Doors only; procedural FF-prefix
        // doors get filtered at TickFollowApply time.
        if (cls.isDoor && args && args->activator) {
            // Activator must be the local player (not an NPC opening).
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && static_cast<RE::TESObjectREFR*>(args->activator) ==
                          static_cast<RE::TESObjectREFR*>(player))
            {
                Pulsar::TransitSync::NoteLocalDoorActivation(selfID);
            }
        }

        // Dispatch original. Engine decides if the activation actually goes
        // through (blocked by scripts, already-taken, etc.).
        bool result = g_origActivate ? g_origActivate(self, args) : false;

        // F.3.b: post-orig lock-state probe + diff broadcast for kCONT/kDOOR.
        // The engine auto-fires Activate again after a successful lockpick
        // mini-game completes, so this same call path catches the unlocked
        // state on the second invocation and LockSync detects the
        // YES→no flag transition vs. its per-REFR cache.
        if (cls.isContainer || cls.isDoor) {
            Pulsar::WorldSync::Lock::OnHostActivate(
                self, selfID, baseID, baseType, px, py, pz);
        }

        // F.3.c: container-open broadcast for non-locked kCONT activates.
        // Distinct from F.3.b lock-flip - covers regular E-press on
        // unlocked containers (weapon cases, normal chests, etc.). The
        // call is a no-op for locked containers (LockSync owns that path)
        // and for failed activations.
        if (cls.isContainer) {
            Pulsar::WorldSync::Lock::OnHostContainerOpen(
                self, selfID, baseID, px, py, pz, result);
        }

        // World-pickup broadcast. SendPickupPacket operates on captured
        // scalar values - no deref of `self` - so it's SEH-safe without an
        // extra wrapper (PluginLookup internals are already __try-guarded).
        if (eligible && result) {
            auto& net = Pulsar::Network::GetSingleton();
            if (net.IsConnected()) {
                SendPickupPacket(selfID, baseID, baseType, px, py, pz);
            }
        }

        // F.3: door-activation hand-off. The Activate hook is shared (loot
        // and doors both funnel through it), but the door-sync logic lives
        // in its own module - DoorSync owns the packet, queue, and replay.
        if (cls.isDoor || npcDoorOpen) {
            Pulsar::WorldSync::Door::OnHostActivate(
                selfID, baseID, px, py, pz, result);
        }

        // F.7: kACTI (Activator) replay-sync - switches, buttons, levers,
        // power-conduits. Same dispatcher pattern as F.3 + per-Activate
        // inventory diff to detect what the activator script consumed
        // from the player (e.g. Power Cell for power-conduits) so the
        // receiver can ghost-add it before replay → script consumes ghost.
        if (cls.isActi) {
            uint32_t consumedFormID = 0;
            int32_t  consumedCount  = 0;
            ComputeActiConsumedItem(consumedFormID, consumedCount);
            t_actiPreSnapCount = 0;
            Pulsar::WorldSync::Acti::OnHostActivate(
                selfID, baseID, px, py, pz, result,
                consumedFormID, consumedCount);
        }

        // Container-take broadcast is handled by ContainerInvSink at event
        // time (see DiffAndBroadcastContainer) - the actual item transfer
        // happens asynchronously after Activate returns, so we'd miss it
        // here. No-op in the Activate path now.

        return result;
    }

    // ---------------------------------------------------------------------
    // Client-side pickup replay - deferred to game thread because Activate
    // touches inventory / script event queue that are single-threaded.
    // ---------------------------------------------------------------------

    struct PendingPickup {
        uint32_t resolvedBase;  // base formID already re-encoded to client load-order
        uint8_t  formType;
        float    px, py, pz;
    };

    struct PendingContainerTake {
        uint32_t resolvedContainerBase;
        uint32_t resolvedItemBase;
        float    px, py, pz;
        int32_t  count;
        uint8_t  refType;          // kREFR or kACHR - selects matcher path
        uint8_t  transferReason;   // M.2 V5: forwarded ITEM_TRANSFER_REASON
        uint16_t modSentinel;      // M.2 V16: kObjectInstance +0x28 sentinel
        std::vector<uint32_t> resolvedMods;  // M.2 V7: cross-plugin-resolved
    };

    struct PendingDrop {
        uint32_t resolvedBase;
        uint8_t  formType;
        int32_t  count;
        uint16_t modSentinel;                // M.2 V16
        std::vector<uint32_t> resolvedMods;  // F.2: client-local mod FormIDs
    };

    static std::mutex             s_pendingMutex;
    static std::vector<PendingPickup> s_pending;
    static std::vector<PendingContainerTake> s_pendingContainerTakes;
    static std::vector<PendingDrop> s_pendingDrops;

    // M.2 V17 (2026-05-08): recently-applied REFR tracking for rapid
    // multi-drop deduplication. When two same-base weapons drop within
    // 100ms, FindSpawnedRefNearActor returns the same REFR for both
    // (position match wins) → second weapon never gets V15+V16 fix.
    // Mark each REFR after we apply mods; subsequent searches skip it
    // for ~3000ms so the second drop's distinct REFR is found.
    static std::mutex                                          s_recentlyAppliedMutex;
    static std::unordered_map<RE::TESObjectREFR*, uint64_t>    s_recentlyApplied;

    static void MarkRefrApplied(RE::TESObjectREFR* refr) noexcept
    {
        if (!refr) return;
        std::lock_guard g(s_recentlyAppliedMutex);
        s_recentlyApplied[refr] = ::GetTickCount64();
    }
    static bool IsRecentlyApplied(RE::TESObjectREFR* refr) noexcept
    {
        if (!refr) return false;
        std::lock_guard g(s_recentlyAppliedMutex);
        auto it = s_recentlyApplied.find(refr);
        if (it == s_recentlyApplied.end()) return false;
        return (::GetTickCount64() - it->second) < 3000ull;
    }
    static void PruneRecentlyApplied() noexcept
    {
        const uint64_t now = ::GetTickCount64();
        std::lock_guard g(s_recentlyAppliedMutex);
        for (auto it = s_recentlyApplied.begin(); it != s_recentlyApplied.end(); ) {
            if ((now - it->second) > 5000ull) it = s_recentlyApplied.erase(it);
            else ++it;
        }
    }

    // M.2 V24.2 (2026-05-09): per-formID anchor of mod-array with
    // activity-based TTL.
    //
    // Why V24.1 failed: TTL was measured from `firstSeenMs`, so anchors
    // expired exactly N seconds after their first observation regardless of
    // ongoing activity. A long BarterMenu session would lose anchors mid-
    // session, and subsequent takes would re-anchor with already-grown
    // engine values (visible in logs as mods=15/16/17 replacing 13/13/14).
    //
    // V24.2 strategy:
    //   • Anchor's mods/sentinel are still set ONCE on first observation
    //     (first-observation wins for content)
    //   • TTL is measured from `lastSeenMs`, refreshed on every successful
    //     access (take observation OR place restore)
    //   • TTL bumped to 5min: enough cushion for casual BarterMenu sessions
    //
    // Effect: anchor persists as long as the user keeps interacting with
    // items of this formID. Once activity stops for 5min, anchor expires
    // and the next take re-anchors fresh.
    struct ModAnchor {
        std::vector<uint32_t> mods;
        uint16_t              sentinel;
        uint64_t              firstSeenMs;
        uint64_t              lastSeenMs;
    };
    static std::mutex s_modAnchorMutex;
    static std::unordered_map<uint32_t, ModAnchor> s_modAnchor;
    static constexpr uint64_t kModAnchorTtlMs = 300000;  // 5 min

    // Record a TAKE observation. First observation per formID sets the
    // mods/sentinel; subsequent observations within TTL bump lastSeenMs but
    // do NOT replace mods (the engine may already have grown them).
    static void V24_RecordTakeObservation(uint32_t itemID,
                                           std::vector<uint32_t>&& mods,
                                           uint16_t sentinel) noexcept
    {
        if (!itemID) return;
        std::lock_guard g(s_modAnchorMutex);
        auto it = s_modAnchor.find(itemID);
        uint64_t now = ::GetTickCount64();
        if (it == s_modAnchor.end()
            || (now - it->second.lastSeenMs) > kModAnchorTtlMs) {
            // No anchor OR previous anchor stale (no activity for TTL) →
            // re-anchor with current observation.
            s_modAnchor[itemID] = ModAnchor{
                std::move(mods), sentinel, now, now
            };
        } else {
            // Active anchor - keep mods/sentinel, just bump activity.
            it->second.lastSeenMs = now;
        }
    }

    // Look up anchored mods for a formID. Returns false if no anchor or
    // TTL of inactivity exceeded. On success, bumps lastSeenMs (place
    // refresh keeps anchor alive across long sessions).
    static bool V24_GetAnchor(uint32_t itemID,
                               std::vector<uint32_t>& outMods,
                               uint16_t& outSentinel) noexcept
    {
        if (!itemID) return false;
        std::lock_guard g(s_modAnchorMutex);
        auto it = s_modAnchor.find(itemID);
        if (it == s_modAnchor.end()) return false;
        uint64_t now = ::GetTickCount64();
        if ((now - it->second.lastSeenMs) > kModAnchorTtlMs) {
            s_modAnchor.erase(it);
            return false;
        }
        outMods     = it->second.mods;
        outSentinel = it->second.sentinel;
        it->second.lastSeenMs = now;
        return true;
    }

    // SEH-wrapped read of (mods+sentinel) from inventory item entry - used
    // pre-engine-call inside HookedAddObjInner (which has C++ unwinding).
    static bool SafeReadInventoryItemModSnapshot(
        RE::TESObjectREFR* refr,
        uint32_t formID,
        std::vector<uint32_t>& outMods,
        uint16_t& outSentinel) noexcept
    {
        outMods.clear();
        outSentinel = 0xFFFF;
        if (!refr || !formID || !g_getExtraByType) return false;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (!invList) return false;
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return false;
            for (uint32_t i = 0; i < entryCount; ++i) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (!obj || RawFormID(obj) != formID) continue;
                auto* stacksData = *reinterpret_cast<uint8_t**>(entry + 0x18);
                if (!stacksData) return true;  // entry exists, no stacks → empty mods
                void* extraList = *reinterpret_cast<void**>(stacksData + 0x00);
                if (!extraList) return true;
                void* objInstExtra = g_getExtraByType(extraList, 0x33);
                if (!objInstExtra) return true;
                auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
                uint32_t modCount = *reinterpret_cast<uint32_t*>(eb + 0x18);
                void* modArray   = *reinterpret_cast<void**>(eb + 0x20);
                outSentinel = *reinterpret_cast<uint16_t*>(eb + 0x28);
                if (!modArray || modCount == 0 || modCount > 64) return true;
                auto* arr = reinterpret_cast<uint8_t*>(modArray);
                outMods.reserve(modCount);
                for (uint32_t mi = 0; mi < modCount && outMods.size() < kMaxModsPerDrop; ++mi) {
                    auto* mod = *reinterpret_cast<uint8_t**>(arr + mi * 0x10);
                    if (mod) outMods.push_back(RawFormID(mod));
                }
                return true;
            }
            return false;  // formID not found
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            outMods.clear();
            outSentinel = 0xFFFF;
            return false;
        }
    }

    // V26: noexcept, non-logging variant of restore - used by the polling
    // tick to silently revert items that the engine grew in the background.
    // Returns true if a divergence was found and corrected (so caller can
    // log a summary instead of per-item).
    static bool SafeRestoreModArrayOnInventoryItemSilent(
        RE::TESObjectREFR* refr,
        uint32_t formID,
        const std::vector<uint32_t>& cachedMods,
        uint16_t cachedSentinel) noexcept
    {
        bool changed = false;
        if (!refr || !formID || !g_getExtraByType) return false;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (!invList) return false;
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return false;

            uint32_t cachedCount = static_cast<uint32_t>(cachedMods.size());

            for (uint32_t i = 0; i < entryCount; ++i) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (!obj || RawFormID(obj) != formID) continue;

                auto* stacksData = *reinterpret_cast<uint8_t**>(entry + 0x18);
                if (!stacksData) continue;
                void* extraList = *reinterpret_cast<void**>(stacksData + 0x00);
                if (!extraList) continue;
                void* objInstExtra = g_getExtraByType(extraList, 0x33);
                if (!objInstExtra) continue;

                auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
                uint32_t localCount = *reinterpret_cast<uint32_t*>(eb + 0x18);
                uint16_t localSentinel = *reinterpret_cast<uint16_t*>(eb + 0x28);

                // Skip if already matches (avoid useless writes - and infinite
                // log spam in caller).
                if (localCount == cachedCount && localSentinel == cachedSentinel) {
                    continue;
                }

                void* localArray = *reinterpret_cast<void**>(eb + 0x20);
                uint32_t commonSlots = std::min(localCount, cachedCount);
                if (localArray && commonSlots > 0) {
                    auto* arr = reinterpret_cast<uint8_t*>(localArray);
                    for (uint32_t mi = 0; mi < commonSlots; ++mi) {
                        auto* mod = RE::TESForm::LookupByID(cachedMods[mi]);
                        if (mod) *reinterpret_cast<void**>(arr + mi * 0x10) = mod;
                    }
                }
                // CLAMPED count write - same latent overflow as the loud
                // variant (see comment there): cached > local capacity must
                // not grow modCount past the allocated array.
                *reinterpret_cast<uint32_t*>(eb + 0x18) = commonSlots;
                *reinterpret_cast<uint16_t*>(eb + 0x28) = cachedSentinel;
                changed = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
        return changed;
    }

    // V26: SEH-isolated formID collector - fills a stack buffer with formIDs
    // of all entries in the container's invList. Separate function (not
    // inlined) because __try cannot coexist with C++-unwinding objects in
    // the caller (C2712).
    static int32_t SafeCollectContainerFormIDs(RE::TESObjectREFR* refr,
                                                uint32_t* outBuf,
                                                int32_t maxOut) noexcept
    {
        if (!refr || !outBuf || maxOut <= 0) return 0;
        int32_t n = 0;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (!invList) return 0;
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return 0;
            for (uint32_t i = 0; i < entryCount && n < maxOut; ++i) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (obj) outBuf[n++] = RawFormID(obj);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
        return n;
    }

    // V26: walk all items in a tracked container, for each formID that has
    // an active anchor restore from the anchor. Run from the polling tick to
    // continuously revert the engine's background growth. Returns number of
    // items reverted in this pass.
    static int32_t SafeRestoreAllAnchoredInContainer(RE::TESObjectREFR* refr)
    {
        if (!refr) return 0;
        constexpr int32_t kMaxForms = 256;
        uint32_t forms[kMaxForms];
        int32_t formCount = SafeCollectContainerFormIDs(refr, forms, kMaxForms);

        int32_t reverted = 0;
        for (int32_t i = 0; i < formCount; ++i) {
            uint32_t formID = forms[i];
            std::vector<uint32_t> anchorMods;
            uint16_t anchorSentinel = 0xFFFF;
            if (!V24_GetAnchor(formID, anchorMods, anchorSentinel)) continue;
            if (SafeRestoreModArrayOnInventoryItemSilent(
                    refr, formID, anchorMods, anchorSentinel)) {
                ++reverted;
            }
        }
        return reverted;
    }

    // V24 restore: like SafeReplaceModArrayOnInventoryItem but always writes
    // modCount (even 0) and sentinel - needed because cached state may be
    // empty (grey weapons with no mods) and we still need to undo any growth
    // the engine applied during the BarterMenu cycle.
    static void SafeRestoreModArrayOnInventoryItem(
        RE::TESObjectREFR* refr,
        uint32_t formID,
        const std::vector<uint32_t>& cachedMods,
        uint16_t cachedSentinel) noexcept
    {
        if (!refr || !formID || !g_getExtraByType) return;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (!invList) return;
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return;

            for (uint32_t i = 0; i < entryCount; ++i) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (!obj || RawFormID(obj) != formID) continue;

                auto* stacksData = *reinterpret_cast<uint8_t**>(entry + 0x18);
                if (!stacksData) return;
                void* extraList = *reinterpret_cast<void**>(stacksData + 0x00);
                if (!extraList) return;
                void* objInstExtra = g_getExtraByType(extraList, 0x33);
                if (!objInstExtra) return;

                auto* eb = reinterpret_cast<uint8_t*>(objInstExtra);
                uint32_t localCount = *reinterpret_cast<uint32_t*>(eb + 0x18);
                void* localArray   = *reinterpret_cast<void**>(eb + 0x20);

                uint32_t cachedCount = static_cast<uint32_t>(cachedMods.size());
                uint32_t commonSlots = std::min(localCount, cachedCount);

                if (localArray && commonSlots > 0) {
                    auto* arr = reinterpret_cast<uint8_t*>(localArray);
                    for (uint32_t mi = 0; mi < commonSlots; ++mi) {
                        auto* mod = RE::TESForm::LookupByID(cachedMods[mi]);
                        if (mod) *reinterpret_cast<void**>(arr + mi * 0x10) = mod;
                    }
                }

                // Write the count CLAMPED to what we actually wrote. Truncation
                // (cached < local, grey weapons) keeps working as before. The
                // unclamped write was a latent overflow: when the sender's item
                // has MORE mods than the local roll allocated slots for (first
                // hit by the F.2-v2 clone equip: local 10, cached 11), count >
                // capacity made the engine read a garbage entry past the array
                // → AV @ RVA 0x402423 (host crash 2026-06-11 17:23). Excess
                // sender mods are dropped (no realloc path); visuals stay
                // slightly incomplete instead of corrupting the heap.
                *reinterpret_cast<uint32_t*>(eb + 0x18) = commonSlots;
                *reinterpret_cast<uint16_t*>(eb + 0x28) = cachedSentinel;

                PLOG_INFO(
                    "[PULSAR] M.2 V24 mod-restore: refr=0x{:08X} item=0x{:08X} mods={}->{} (cached {}) sentinel=0x{:04X}"sv,
                    RawFormID(refr), formID, localCount, commonSlots, cachedCount, cachedSentinel);
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN(
                "[PULSAR] M.2 V24 mod-restore SEH: refr=0x{:08X} item=0x{:08X}"sv,
                RawFormID(refr), formID);
        }
    }

    // (2026-06-18): NATIVE per-entry mod-attach. Finds the inventory
    // entry by formID, then drives the engine's own attach core
    // (g_attachModInvCore = FUN_14063de80) once per mod. Unlike the M.2
    // overwrite primitives, this allocates the kObjectInstance if absent,
    // grows the array, and routes each mod into its CORRECT slot from the mod
    // template. Returns the count of mods the engine accepted. Per-mod
    // SEH-wrapped so one bad mod doesn't abort the rest or fault the restore.
    static uint32_t SafeAttachModNativeToInventoryItem(
        RE::TESObjectREFR* owner,
        uint32_t formID,
        const std::vector<uint32_t>& mods,
        uint16_t sentinel) noexcept
    {
        if (!owner || !formID || mods.empty() || !g_attachModInvCore ||
            !g_getExtraByType) {
            return 0;
        }

        uint32_t attached = 0;

        // modfix (2026-06-19) - the freshly AddObjectToContainer'd base form
        // arrives with its TEMPLATE DEFAULT mods already in a kObjectInstance.
        // Attaching the captured set on top yields base-defaults + captured =
        // an over-modded / duplicated weapon (the "random mods" bug; diagnostic
        // showed actN=25 vs expN=14). Reset the existing mod array to EMPTY
        // first (modCount=0 at kObjectInstance+0x18) so the native attach below
        // rebuilds EXACTLY the captured set: the engine's grow writes from index
        // 0, and trailing stale entries are ignored (reads honour the count).
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(owner);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (invList) {
                uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
                auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
                if (entries && entryCount && entryCount <= 100000) {
                    for (uint32_t i = 0; i < entryCount; ++i) {
                        uint8_t* e = entries + i * 0x28;
                        auto* obj = *reinterpret_cast<uint8_t**>(e + 0x00);
                        if (!obj || RawFormID(obj) != formID) continue;
                        auto* stacksData = *reinterpret_cast<uint8_t**>(e + 0x18);
                        if (!stacksData) break;
                        void* extraList = *reinterpret_cast<void**>(stacksData + 0x00);
                        if (!extraList) break;
                        void* oi = g_getExtraByType(extraList, 0x33);  // kObjectInstance
                        if (oi)
                            *reinterpret_cast<uint32_t*>(
                                reinterpret_cast<uint8_t*>(oi) + 0x18) = 0;
                        break;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        for (uint32_t mi = 0; mi < mods.size() && mi < kMaxModsPerDrop; ++mi) {
            RE::TESForm* mod = RE::TESForm::LookupByID(mods[mi]);
            if (!mod) continue;

            __try {
                auto* pb = reinterpret_cast<uint8_t*>(owner);
                auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
                if (!invList) break;
                uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
                auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
                if (!entries || entryCount == 0 || entryCount > 100000) break;

                // Re-resolve the entry by formID before each attach: attaching
                // can split/re-order a multi-count stack. Modded gear is
                // count-1 so this is normally stable; the re-walk is cheap and
                // defensive.
                uint8_t* entry = nullptr;
                for (uint32_t i = 0; i < entryCount; ++i) {
                    uint8_t* e = entries + i * 0x28;
                    auto* obj = *reinterpret_cast<uint8_t**>(e + 0x00);
                    if (obj && RawFormID(obj) == formID) { entry = e; break; }
                }
                if (!entry) break;

                // Build the 0x30 ctx exactly as the Papyrus inventory path does
                // (FUN_14206d6b0): +0x00 owner, +0x08 mod, +0x18 &attachOk
                // (success accumulator, MUST be non-null AND start at 1 - the
                // engine does `*accumulator &= addModResult`, then returns it),
                // +0x20 flag=1, +0x21 attach-mode=1. The THIRD arg is a separate
                // status byte the core sets 0→1; its bool return is INVERTED
                // (false on success), so we read the accumulator, not the return.
                alignas(8) uint8_t ctx[0x30] = {};
                char attachOk = 1;     // ctx+0x18 accumulator (== add-mod result after call)
                char status   = 0;     // third arg (core sets it; unused by us)
                *reinterpret_cast<void**>(ctx + 0x00) = owner;
                *reinterpret_cast<void**>(ctx + 0x08) = mod;
                *reinterpret_cast<char**>(ctx + 0x18) = &attachOk;
                ctx[0x20] = 1;
                ctx[0x21] = 1;

                g_attachModInvCore(ctx, entry, &status);
                if (attachOk != 0) {
                    ++attached;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN(
                    "[PULSAR] native attach SEH: owner=0x{:08X} item=0x{:08X} mod=0x{:08X}"sv,
                    RawFormID(owner), formID, mods[mi]);
                break;  // structure likely torn - stop touching this item
            }
        }

        // Stamp the source sentinel (+0x28 item-tier rendering) so the restored
        // item renders at the original tier - the engine's fresh kObjectInstance
        // starts at 0xFFFF. Cosmetic; one SEH-guarded write.
        if (attached > 0 && sentinel != 0xFFFF) {
            __try {
                auto* pb = reinterpret_cast<uint8_t*>(owner);
                auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
                if (invList) {
                    uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
                    auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
                    if (entries && entryCount > 0 && entryCount <= 100000) {
                        for (uint32_t i = 0; i < entryCount; ++i) {
                            uint8_t* e = entries + i * 0x28;
                            auto* obj = *reinterpret_cast<uint8_t**>(e + 0x00);
                            if (!obj || RawFormID(obj) != formID) continue;
                            auto* stacksData = *reinterpret_cast<uint8_t**>(e + 0x18);
                            if (!stacksData) break;
                            void* extraList = *reinterpret_cast<void**>(stacksData + 0x00);
                            if (!extraList) break;
                            void* objInst = g_getExtraByType(extraList, 0x33);
                            if (objInst) {
                                *reinterpret_cast<uint16_t*>(
                                    reinterpret_cast<uint8_t*>(objInst) + 0x28) = sentinel;
                            }
                            break;
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { /* sentinel is cosmetic */ }
        }

        return attached;
    }

    // Scan a single cell for a REFR whose base matches resolvedBase and
    // position is within radius of (px,py,pz). SEH-wrapped because the
    // references array can be torn during proc-gen cell activation (T114).
    static RE::TESObjectREFR* SafeFindLootRefInCell(void* cell,
                                                     uint32_t resolvedBase,
                                                     float px, float py, float pz,
                                                     float radius)
    {
        __try {
            if (!cell) return nullptr;
            auto* cb = reinterpret_cast<uint8_t*>(cell);
            // Same layout as VehicleSync: cell+0x80 size, cell+0x88 data ptr.
            uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > 10000) return nullptr;

            RE::TESObjectREFR* best = nullptr;
            float bestDist2 = radius * radius;

            for (uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;
                auto* base = *reinterpret_cast<uint8_t**>(
                    reinterpret_cast<uint8_t*>(ref) + 0x98);
                if (!base) continue;
                uint32_t baseID = RawFormID(base);
                if (baseID != resolvedBase) continue;

                // M.2 V17: skip REFRs we already applied mods to in the
                // last few seconds. Without this, two same-base drops in
                // rapid succession both find REFR #1 → REFR #2 never gets
                // the mod/sentinel fix and stays proc-gen rolled.
                if (IsRecentlyApplied(ref)) continue;

                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(ref) + 0x8C);
                float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    best = ref;
                }
            }
            return best;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // Plain-C SEH wrapper - must live outside the C++-unwinding drain
    // function (C2712: __try forbidden next to objects with destructors).
    static void SafeCallOrigActivate(RE::TESObjectREFR* refr,
                                      ActivateArgs* args,
                                      uint32_t refIDForLog) noexcept
    {
        if (!g_origActivate) return;
        __try {
            g_origActivate(refr, args);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] LootSync: client Activate raised SEH on ref=0x{:08X}"sv,
                      refIDForLog);
        }
    }

    // Find a container REFR by base formID + position. Similar to the loot
    // matcher but filters on base == resolvedContainerBase (not form-type -
    // a container's base is already a TESObjectCONT and its formID is
    // unique within a cell).
    static RE::TESObjectREFR* SafeFindContainerByBaseInCell(void* cell,
                                                             uint32_t resolvedBase,
                                                             float px, float py, float pz,
                                                             float radius)
    {
        __try {
            if (!cell) return nullptr;
            auto* cb = reinterpret_cast<uint8_t*>(cell);
            uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > 10000) return nullptr;

            RE::TESObjectREFR* best = nullptr;
            float bestDist2 = radius * radius;
            for (uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;
                auto* base = *reinterpret_cast<uint8_t**>(
                    reinterpret_cast<uint8_t*>(ref) + 0x98);
                if (!base) continue;
                if (RawFormID(base) != resolvedBase) continue;
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(ref) + 0x8C);
                float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    best = ref;
                }
            }
            return best;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // F.1.e: position-only matcher for dead-NPC corpses. Base FormID is
    // dynamic (FF-prefix) on procedurally-spawned NPCs and won't match
    // across machines, so we skip the base filter and rely on position +
    // ACHR formType. NPCSync keeps positions roughly aligned.
    static RE::TESObjectREFR* SafeFindActorRefByPos(void* cell,
                                                     float px, float py, float pz,
                                                     float radius)
    {
        __try {
            if (!cell) return nullptr;
            auto* cb = reinterpret_cast<uint8_t*>(cell);
            uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > 10000) return nullptr;

            RE::TESObjectREFR* best = nullptr;
            float bestDist2 = radius * radius;
            for (uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;
                if (RawFormType(ref) != static_cast<uint8_t>(RE::FormType::kACHR))
                    continue;
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(ref) + 0x8C);
                float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    best = ref;
                }
            }
            return best;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // F.1.e: SEH-wrapped Papyrus UnequipItem call - clears the actor's
    // equipped-slot pointer to baseFormID so that subsequent raw-remove
    // isn't undone by engine reconciliation on next inventory access.
    // Safe to call when item isn't actually equipped (handler iterates
    // and no-ops when no matching equipped entry).
    static void SafeUnequipFromActor(RE::Actor* actor, RE::TESForm* item) noexcept
    {
        if (!g_unequipPapyrus || !actor || !item) return;
        __try {
            // VM ctx + stack ID = 0,0 - only used in error paths
            // (None-item, no-inventory, no-3d). Dead NPCs satisfy all.
            // preventEquip=false (don't block re-equip), silent=true.
            g_unequipPapyrus(nullptr, 0, actor, item, false, true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // SEH lands when actor was freed mid-call or VM-error path
            // dereferenced our null context. Silent - the raw-remove
            // fallback below still fires.
        }
    }

    // Remove |count| units of base from a container's inventoryList. Uses
    // the same swap-remove pattern verified in F.1.b (count@+0x08, entry
    // swap-remove when stacks drain). Returns number of units actually
    // removed (may be less than requested on proc-gen divergence).
    static int32_t RawRemoveFromContainer(RE::TESObjectREFR* container,
                                           uint32_t baseFormID,
                                           int32_t count) noexcept
    {
        if (!container || !baseFormID || count <= 0) return 0;
        int32_t removed = 0;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(container);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (!invList) return 0;
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return 0;

            // M.2 V23 (2026-05-09): iterate ALL matching entries, not just
            // the first. When host has multiple variants of the same base
            // (different mod loadouts → separate entries), pre-V23 break
            // statement after first match left other variants untouched.
            // For 5 items spread across 5 variants, we'd remove only 1.
            // Loop continues until removed == count or no more entries.
            for (uint32_t i = 0; i < entryCount && removed < count; ) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (!obj || RawFormID(obj) != baseFormID) {
                    ++i;
                    continue;
                }

                uint32_t stackCount = *reinterpret_cast<uint32_t*>(entry + 0x10);
                auto* stacksData = *reinterpret_cast<uint8_t**>(entry + 0x18);
                if (!stacksData || stackCount == 0 || stackCount > 1000) {
                    ++i;
                    continue;
                }

                for (uint32_t si = 0; si < stackCount && removed < count; ++si) {
                    uint8_t* stack = stacksData + static_cast<size_t>(si) * 0x10;
                    uint32_t* cnt = reinterpret_cast<uint32_t*>(stack + 0x08);
                    if (*cnt == 0 || *cnt > 100000) continue;
                    int32_t remain = count - removed;
                    uint32_t take = (*cnt <= static_cast<uint32_t>(remain)) ? *cnt : static_cast<uint32_t>(remain);
                    *cnt -= take;
                    removed += static_cast<int32_t>(take);
                }

                // If every stack is drained to 0, collapse + swap-remove the
                // entry so UI doesn't show a count=0 ghost.
                bool allDrained = true;
                for (uint32_t si = 0; si < stackCount; ++si) {
                    uint8_t* stack = stacksData + static_cast<size_t>(si) * 0x10;
                    uint32_t cnt = *reinterpret_cast<uint32_t*>(stack + 0x08);
                    if (cnt > 0) { allDrained = false; break; }
                }
                if (allDrained) {
                    *reinterpret_cast<uint32_t*>(entry + 0x10) = 0;
                    uint32_t* dataSizePtr = reinterpret_cast<uint32_t*>(invList + 0x28);
                    uint32_t dataCount = *dataSizePtr;
                    if (dataCount > 0 && i < dataCount - 1) {
                        uint8_t* last = entries + (dataCount - 1) * 0x28;
                        std::memcpy(entry, last, 0x28);
                        std::memset(last, 0, 0x28);
                    } else if (dataCount > 0 && i == dataCount - 1) {
                        std::memset(entry, 0, 0x28);
                    }
                    *dataSizePtr = dataCount > 0 ? (dataCount - 1) : 0;
                    entryCount = *dataSizePtr;  // updated count after swap-remove
                    // Don't ++i - swap-remove placed last entry at i, re-check.
                } else {
                    ++i;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // return whatever we got so far
        }
        return removed;
    }

    // M.2 (2026-05-08): noexcept wrapper around the engine's
    // TESObjectREFR vt[0x9A] AddObjectToContainer call. Used by the
    // place-apply path on the receiver side. C2712-free so the caller
    // (DrainPendingPickups) can hold C++ objects.
    static bool SafeAddObjectToContainer(RE::TESObjectREFR* container,
                                          RE::TESForm* itemForm,
                                          int32_t count) noexcept
    {
        if (!container || !itemForm || count <= 0) return false;
        bool ok = false;
        __try {
            using AddObjFn = void (__fastcall*)(
                RE::TESObjectREFR*, RE::TESForm*,
                void* /*BSTSmartPointer<ExtraDataList>*/,
                int32_t, RE::TESObjectREFR*, uint32_t /*reason*/);
            auto** vt = *reinterpret_cast<AddObjFn***>(container);
            AddObjFn fn = reinterpret_cast<AddObjFn>(vt[0x9A]);
            void* nullSmart = nullptr;
            fn(container, itemForm, &nullSmart, count, nullptr,
                static_cast<uint32_t>(RE::ITEM_TRANSFER_REASON::kScriptAddItem));
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        return ok;
    }

    // M.2 V4 (2026-05-08): noexcept wrapper around the inner core
    // FUN_140B4D2E0 - bypasses vt[0x9A] for ship-form / non-vt-overridden
    // REFRs. Used by the receiver to replay AddObjInner with same shape
    // as host's hooked event. Sets t_replayingFromPacket while running so
    // our own hook doesn't broadcast back.
    static bool SafeAddObjInnerReplay(
        RE::TESObjectREFR* dest, RE::TESForm* item,
        int32_t count, RE::TESObjectREFR* src,
        uint32_t reason) noexcept
    {
        if (!g_origAddObjInner || !item || count <= 0) return false;
        bool ok = false;
        bool prev = t_replayingFromPacket;
        t_replayingFromPacket = true;
        __try {
            void* nullSmart = nullptr;
            g_origAddObjInner(dest, item, &nullSmart, reason, count, src);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        t_replayingFromPacket = prev;
        return ok;
    }

    // Spawn a REFR of |itemForm| at |anchor|'s position (+forward offset, as
    // the engine does for real drops). Calls the inner REFR-creator directly
    // bypasses drop-core, which required VM task handles we can't forge.
    // Returns the engine's 32-bit handle to the new REFR (0 = spawn failed).
    // SEH-wrapped since the primitive touches cell navmesh / physics and is
    // off the main engine call path here.
    static uint32_t SafeSpawnDroppedRef(RE::TESObjectREFR* anchor,
                                         RE::TESForm* itemForm,
                                         int32_t count,
                                         uint32_t baseIDForLog) noexcept
    {
        if (!g_spawnRef || !anchor || !itemForm || count <= 0) return 0;
        uint32_t outHandle = 0;
        __try {
            // extra=NULL, pos=NULL, rot=NULL, refcount=NULL → uses anchor's
            // pos+rot with forward-offset exactly like the player drop path.
            g_spawnRef(anchor, &outHandle, itemForm, count,
                       nullptr, nullptr, nullptr, nullptr);
            return outHandle;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN(
                "[PULSAR] F.1.d: SpawnRef raised SEH for base=0x{:08X}"sv,
                baseIDForLog);
            return 0;
        }
    }

    // Disable + mark-for-delete on the REFR - pure despawn, no inventory.
    // kDisabled is bit 11 of formFlags @ +0x20; SetDelete is TESForm vtable
    // slot 0x33 (exposed via CommonLibSF). SEH-wrapped for belt-and-
    // suspenders; raw flag write is a single mov, the virtual call should
    // never fault on a valid REFR.
    // F.2 helper: read clone's position, scan its cell for a REFR with
    // matching base in 3m radius. SEH-wrapped + noexcept so it's safe to
    // call from DrainPendingPickups (which has C++ unwinding constraints).
    static RE::TESObjectREFR* FindSpawnedRefNearActor(
        RE::TESObjectREFR* anchor, uint32_t baseID) noexcept
    {
        if (!anchor || !baseID) return nullptr;
        float cx = 0, cy = 0, cz = 0;
        void* cell = nullptr;
        __try {
            auto* cb = reinterpret_cast<uint8_t*>(anchor);
            float* loc = reinterpret_cast<float*>(cb + 0x8C);
            cx = loc[0]; cy = loc[1]; cz = loc[2];
            cell = anchor->parentCell;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!cell) return nullptr;
        return SafeFindLootRefInCell(cell, baseID, cx, cy, cz, 300.0f);
    }

    static bool SafeDisableAndDelete(RE::TESObjectREFR* refr,
                                      uint32_t refIDForLog) noexcept
    {
        if (!refr) return false;
        __try {
            uint32_t* flagsPtr = reinterpret_cast<uint32_t*>(
                reinterpret_cast<uint8_t*>(refr) + 0x20);
            *flagsPtr |= 0x800u;  // kDisabled
            refr->SetDelete(true);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] LootSync: disable/SetDelete raised SEH on ref=0x{:08X}"sv,
                      refIDForLog);
            return false;
        }
    }

    // Hide a REFR's 3D visually without waiting for cell-reload.
    // Approach: combine flag-cull with position-displacement (proven
    // pattern from F.6 GrabSync - three transform writes that the
    // engine respects). For display-case spawned children:
    // SetDelete alone only marks for GC, render keeps going.
    // APP_CULLED bit (NiAVObject.flags+0x118 bit 0 in NetImmerse
    //    convention) - try multiple candidates since Starfield's enum
    //    isn't documented; defensive write of 0x1|0x4|0x10.
    // Position-displacement to z=-1e7 puts the mesh below the world
    //    so even if cull bit is wrong, mesh is off-screen.
    // Returns whether data3D was reachable for the writes.
    static bool SafeHideRefr3D(RE::TESObjectREFR* refr,
                                uint32_t refIDForLog) noexcept
    {
        if (!refr) return false;
        __try {
            constexpr float kHide[3] = { 0.0f, 0.0f, -1.0e7f };

            // Raw REFR.data.location backup used by some engine paths
            auto* loc = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(refr) + 0x8C);
            loc[0] = kHide[0]; loc[1] = kHide[1]; loc[2] = kHide[2];

            auto* loadedData = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(refr) + 0xB8);
            if (!loadedData) {
                PLOG_INFO("[PULSAR] hide-3D ref=0x{:08X} loadedData=NULL"sv,
                          refIDForLog);
                return false;
            }
            auto* data3D = *reinterpret_cast<uint8_t**>(loadedData + 0x08);
            if (!data3D) {
                PLOG_INFO("[PULSAR] hide-3D ref=0x{:08X} data3D=NULL (loadedData=0x{:x})"sv,
                          refIDForLog, reinterpret_cast<uintptr_t>(loadedData));
                return false;
            }

            uint64_t flagsBefore = *reinterpret_cast<uint64_t*>(data3D + 0x118);
            float worldZBefore = *reinterpret_cast<float*>(data3D + 0xB0 + 8);

            // NiAVObject local/world/previousWorld translate writes -
            // F.6 GrabSync proved these stick visually for grabbables.
            std::memcpy(data3D + 0x70, kHide, sizeof(kHide));
            std::memcpy(data3D + 0xB0, kHide, sizeof(kHide));
            std::memcpy(data3D + 0xF0, kHide, sizeof(kHide));

            // NiAVObject.flags @ +0x118 - multi-bit defensive cull
            auto* flags = reinterpret_cast<uint64_t*>(data3D + 0x118);
            *flags |= 0x1ull | 0x4ull | 0x10ull;
            PLOG_INFO(
                "[PULSAR] hide-3D ref=0x{:08X} ok data3D=0x{:x} flagsBefore=0x{:x} flagsAfter=0x{:x} worldZ {:.1f}->{:.1f}"sv,
                refIDForLog, reinterpret_cast<uintptr_t>(data3D),
                flagsBefore, *flags, worldZBefore,
                *reinterpret_cast<float*>(data3D + 0xB0 + 8));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] LootSync: hide-3D raised SEH on ref=0x{:08X}"sv,
                      refIDForLog);
            return false;
        }
    }

    // Pick the single highest-FormID world REFR matching base near
    // (px,py,pz) within radius, then cull. Empirical findings from
    // iter 8 testing on Starfield weapon-cases:
    //
    // For each item-base near a display-case there are typically two
    // FF REFRs in the cell:
    // LOWER FormID  = editor-placed lootable (the ammobox on the
    //                     table next to the case). User can loot it
    //                     normally. Culling this kills the table item.
    // HIGHER FormID = script-spawned tracking REFR (has data3D
    //                     but no visible mesh - visual rendering goes
    //                     through the case container's own NiNode
    //                     hierarchy, not this REFR's data3D).
    //
    // The actual ammo visual inside the case is rendered through a
    // node attached to the case container, NOT through any of the
    // matching FF REFRs we can find via cell-scan. To hide it
    // properly we'd need to trigger the case-script's OnInventoryRemove
    // which runs Disable() on the visual - see F.7+F.8 V4 backlog
    // (Papyrus DispatchStaticCall path, ~8h effort).
    //
    // Picking HIGHEST-FF here avoids killing the table item. Weapons
    // (single cand) hide correctly because their visual IS the cull
    // target. Ammo visuals stay stuck until cell reload - known
    // cosmetic limitation, state is correct.
    //
    // Walks player's parentCell first then NPCSync's active-cell
    // snapshot. SEH-wrapped because cell ref-arrays can tear during
    // streaming (T114).
    static int CullAllVisualChildrenInCells(uint32_t resolvedBase,
                                             float px, float py, float pz,
                                             float radius)
    {
        const float radius2 = radius * radius;
        RE::TESObjectREFR* bestRef = nullptr;
        uint32_t           bestID  = 0;
        float              bestDist2 = 0.0f;
        float              bestDx = 0.0f, bestDy = 0.0f, bestDz = 0.0f;

        auto walk = [&](void* cell) noexcept {
            if (!cell) return;
            __try {
                auto* cb = reinterpret_cast<uint8_t*>(cell);
                uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
                auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
                if (!data || size == 0 || size > 10000) return;
                for (uint32_t i = 0; i < size; ++i) {
                    auto* ref = data[i];
                    if (!ref) continue;
                    auto* base = *reinterpret_cast<uint8_t**>(
                        reinterpret_cast<uint8_t*>(ref) + 0x98);
                    if (!base) continue;
                    if (RawFormID(base) != resolvedBase) continue;
                    auto* loc = reinterpret_cast<float*>(
                        reinterpret_cast<uint8_t*>(ref) + 0x8C);
                    float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
                    float d2 = dx*dx + dy*dy + dz*dz;
                    if (d2 > radius2) continue;

                    uint32_t id = RawFormID(ref);
                    PLOG_INFO(
                        "[PULSAR] ContainerSync: cand ref=0x{:08X} base=0x{:08X} dist={:.1f}"sv,
                        id, resolvedBase, std::sqrt(d2));
                    if (id > bestID) {
                        bestRef = ref; bestID = id;
                        bestDist2 = d2; bestDx = dx; bestDy = dy; bestDz = dz;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
        };

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && player->parentCell) walk(player->parentCell);
        auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
        for (auto* cell : cells) {
            if (!cell) continue;
            if (player && cell == player->parentCell) continue;
            walk(cell);
        }
        // Diagnostic: dump ALL nearby REFRs (any base) for the first
        // few invocations to identify what else lives around the case
        // origin. Helps when our base-filtered cand list misses the
        // actual visual mesh.
        static int s_neighbourDumps = 0;
        if (s_neighbourDumps < 4) {
            ++s_neighbourDumps;
            auto dumpAll = [&](void* cell) noexcept {
                if (!cell) return;
                __try {
                    auto* cb = reinterpret_cast<uint8_t*>(cell);
                    uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
                    auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
                    if (!data || size == 0 || size > 10000) return;
                    int logged = 0;
                    for (uint32_t i = 0; i < size && logged < 30; ++i) {
                        auto* ref = data[i];
                        if (!ref) continue;
                        auto* base = *reinterpret_cast<uint8_t**>(
                            reinterpret_cast<uint8_t*>(ref) + 0x98);
                        if (!base) continue;
                        auto* loc = reinterpret_cast<float*>(
                            reinterpret_cast<uint8_t*>(ref) + 0x8C);
                        float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
                        float d2 = dx*dx + dy*dy + dz*dz;
                        if (d2 > 100.0f * 100.0f) continue;  // ≤100 units
                        PLOG_INFO(
                            "[PULSAR] neighbour ref=0x{:08X} base=0x{:08X} type=0x{:02X} dist={:.1f}"sv,
                            RawFormID(ref), RawFormID(base), RawFormType(base),
                            std::sqrt(d2));
                        ++logged;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            };
            if (player && player->parentCell) dumpAll(player->parentCell);
        }

        if (!bestRef) return 0;
        PLOG_INFO(
            "[PULSAR] ContainerSync: cull-hit ref=0x{:08X} base=0x{:08X} dist={:.1f} (dx={:.1f} dy={:.1f} dz={:.1f}) [highest-FormID]"sv,
            bestID, resolvedBase, std::sqrt(bestDist2), bestDx, bestDy, bestDz);
        SafeDisableAndDelete(bestRef, bestID);
        SafeHideRefr3D(bestRef, bestID);
        return 1;
    }

    static RE::TESObjectREFR* FindLocalLootRef(const PendingPickup& p)
    {
        // Try player's parent cell first (common case - host and client
        // share the same interior/worldspace cell).
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && player->parentCell) {
            auto* hit = SafeFindLootRefInCell(player->parentCell,
                                              p.resolvedBase,
                                              p.px, p.py, p.pz,
                                              800.0f);  // 8m match radius
            if (hit) return hit;
        }

        // Fall back to every cell NPCSync has ever tracked as active -
        // adjacent exterior cells during streaming etc.
        auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
        for (auto* cell : cells) {
            if (!cell) continue;
            if (player && cell == player->parentCell) continue;
            auto* hit = SafeFindLootRefInCell(cell,
                                              p.resolvedBase,
                                              p.px, p.py, p.pz,
                                              800.0f);
            if (hit) return hit;
        }
        return nullptr;
    }

    // M.2 V4 (2026-05-08): inner AddObjectToContainer hook with broadcast.
    // Catches transfers that DON'T fire BGSInventoryListEvent - most
    // importantly ship cargo, where data lives on the ship form
    // (e.g. 0x0003F7E8 Frontier) not on the cargo REFR.
    //
    // Filter rules:
    //   • only act when player (0x14) is involved
    //   • suppress when the OTHER side already has a sink registered -
    //     existing BGSInventoryList diff path handles it (avoid double-
    //     broadcast for normal containers like the captain locker)
    //   • t_replayingFromPacket guard prevents the receiver-side replay
    //     from re-broadcasting (set by the apply path)
    // V25: hooked replacement for FUN_140404300. Returns 0 (skip) when:
    //   • we're inside an AddObjInner call originating from our sync path
    //   • the target BGSObjectInstanceExtra already has modCount > 0
    //
    // Otherwise calls the original. SEH-wrapped read on +0x18 - defensive
    // against bad pointers on engine-driven calls.
    // V25.2: hook reduced to skip-only path (no per-call logging - verified
    // 491 calls with guard=off in V25.1, all background async). Inline guard
    // is dead - kept for future BarterMenu-scoped detection. V26's polling
    // restore is what actually fights the growth.
    static uint32_t __fastcall HookedGrowMods(void* extra, void* refr, void* smartPtr)
    {
        if (t_insideShipSyncAddObj && extra) {
            uint32_t existingCount = 0;
            __try {
                existingCount = *reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(extra) + 0x18);
            } __except (EXCEPTION_EXECUTE_HANDLER) { existingCount = 0; }
            if (existingCount > 0) {
                PLOG_INFO(
                    "[PULSAR] M.2 V25 grow-skip (inline): extra=0x{:p} modCount={}"sv,
                    extra, existingCount);
                return 0;
            }
        }
        if (g_origGrowMods) return g_origGrowMods(extra, refr, smartPtr);
        return 0;
    }

    static void __fastcall HookedAddObjInner(
        RE::TESObjectREFR* dest,
        RE::TESForm*       item,
        void*              smartPtr,
        uint32_t           reason,
        int32_t            count,
        RE::TESObjectREFR* src)
    {
        // M.2 V24 (2026-05-09): pre-engine snapshot of source-side mods on
        // TAKE direction. Captures the ORIGINAL mod-array before BarterMenu's
        // player ↔ buyback ↔ container shuffling has a chance to grow it on
        // host side. Restored post-engine on the matching PLACE call.
        const uint32_t v24DestID = dest ? RawFormID(dest) : 0;
        const uint32_t v24SrcID  = src  ? RawFormID(src)  : 0;
        const bool v24PlayerIsDest = (v24DestID == 0x14u);
        const bool v24PlayerIsSrc  = (v24SrcID  == 0x14u);
        const uint32_t v24ItemID = item ? RawFormID(item) : 0;
        const bool v24Eligible =
            (v24PlayerIsDest || v24PlayerIsSrc) && count != 0 && v24ItemID
            && !t_replayingFromPacket;

        if (v24Eligible && v24PlayerIsDest && src) {
            // TAKE: snapshot mods from container side. First observation per
            // formID becomes anchor; subsequent observations within TTL are
            // ignored (the engine may already have grown mods on remaining
            // items in the container - we don't want that polluting anchor).
            std::vector<uint32_t> preMods;
            uint16_t preSentinel = 0xFFFF;
            if (SafeReadInventoryItemModSnapshot(src, v24ItemID,
                                                  preMods, preSentinel)) {
                PLOG_INFO(
                    "[PULSAR] M.2 V24.2 take-obs: container=0x{:08X} item=0x{:08X} mods={} sentinel=0x{:04X}"sv,
                    v24SrcID, v24ItemID,
                    static_cast<uint32_t>(preMods.size()), preSentinel);
                V24_RecordTakeObservation(v24ItemID, std::move(preMods), preSentinel);
            }
        }

        // V25: gate the guard for engine grow-skip. Only activate while the
        // engine call is running, and only when player is involved (covers
        // BarterMenu / loot / vendor - anywhere mods could grow on items the
        // user is shuffling).
        const bool v25Guard = v24Eligible;
        if (v25Guard) t_insideShipSyncAddObj = true;
        if (g_origAddObjInner) {
            g_origAddObjInner(dest, item, smartPtr, reason, count, src);
        }
        if (v25Guard) t_insideShipSyncAddObj = false;

        // V24.2: restore mods on PLACE direction using anchored value.
        // Anchor persists with activity-based TTL - see V24_GetAnchor.
        if (v24Eligible && v24PlayerIsSrc && dest) {
            std::vector<uint32_t> anchorMods;
            uint16_t anchorSentinel = 0xFFFF;
            if (V24_GetAnchor(v24ItemID, anchorMods, anchorSentinel)) {
                SafeRestoreModArrayOnInventoryItem(dest, v24ItemID,
                                                    anchorMods, anchorSentinel);
            } else {
                PLOG_INFO(
                    "[PULSAR] M.2 V24.2 no-anchor: container=0x{:08X} item=0x{:08X} (place not restored)"sv,
                    v24DestID, v24ItemID);
            }
        }

        uint32_t destID = dest ? RawFormID(dest) : 0;
        uint32_t srcID  = src  ? RawFormID(src)  : 0;
        const bool playerIsDest = (destID == 0x14u);
        const bool playerIsSrc  = (srcID  == 0x14u);
        if (!playerIsDest && !playerIsSrc) return;
        if (count == 0 || !item) return;

        // Resolve the "container side" - the non-player party.
        RE::TESObjectREFR* container = playerIsDest ? src : dest;
        uint32_t containerID         = playerIsDest ? srcID : destID;
        if (!container || !containerID) return;

        uint32_t itemID = RawFormID(item);
        uint8_t  itemTy = RawFormType(item);

        // Suppress if regular BGSInventoryListEvent path already handles
        // this container (avoid double-broadcast). Read the +0xA0 invList
        // source under SEH and check s_registeredSources.
        ContainerRawInfo info = ReadContainerRaw(container);
        bool suppressBySink = false;
        if (info.invListSrc) {
            std::lock_guard g(s_trackedMutex);
            suppressBySink = s_registeredSources.count(info.invListSrc) > 0;
        }

        // Re-entry guard - do not broadcast our own replay.
        if (t_replayingFromPacket) {
            PLOG_INFO(
                "[PULSAR] M.2 AddObjInner: REPLAY suppress dest=0x{:08X} src=0x{:08X} item=0x{:08X} count={} reason={}"sv,
                destID, srcID, itemID, count, reason);
            return;
        }

        PLOG_INFO(
            "[PULSAR] M.2 AddObjInner: dest=0x{:08X} src=0x{:08X} item=0x{:08X} type=0x{:02X} count={} reason={} suppressBySink={}"sv,
            destID, srcID, itemID, itemTy, count, reason,
            suppressBySink ? "yes" : "no");

        if (suppressBySink) return;

        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        // Encode direction in count sign:
        //   playerIsDest = take from container → positive count
        //   playerIsSrc  = place into container → negative count
        int32_t signedCount = playerIsDest ? count : -count;

        // Container position - use raw +0x8C if container is a real REFR,
        // else 0/0/0 (ship-form etc. has no world pos that maps cleanly).
        float cx = 0.f, cy = 0.f, cz = 0.f;
        SafeReadRefrPos(container, cx, cy, cz);

        // M.2 V7: read mods on the moveable item AFTER the engine call -
        // dest now holds the entry with its ExtraDataList intact (mods
        // travel with the items during transfer). For TAKE: dest=player.
        // For PLACE: dest=container. Either way, dest has the item now.
        std::vector<uint32_t> mods;
        ReadInventoryItemMods(dest, itemID, mods);
        uint16_t sentinel = ReadInventoryItemModSentinel(dest, itemID);

        SendContainerTakePacket(containerID, containerID,
                                 RawFormType(container),
                                 cx, cy, cz, itemID, signedCount,
                                 static_cast<uint8_t>(reason),
                                 &mods, sentinel);
    }

}  // anonymous

void Init()
{
    // Hook the Activate core - stable REL::ID on 1.16.236.
    REL::Relocation<void*> target{ REL::ID(63380) };  // RVA 0xB47920
    void* fn = target.get();

    MH_STATUS st = MH_CreateHook(
        fn,
        reinterpret_cast<LPVOID>(&HookedActivate),
        reinterpret_cast<LPVOID*>(&g_origActivate));
    if (st != MH_OK) {
        PLOG_WARN("[PULSAR] LootSync: MH_CreateHook failed: {}"sv,
                  static_cast<int>(st));
        return;
    }
    st = MH_EnableHook(fn);
    if (st != MH_OK) {
        PLOG_WARN("[PULSAR] LootSync: MH_EnableHook failed: {}"sv,
                  static_cast<int>(st));
        return;
    }
    PLOG_INFO("[PULSAR] LootSync: Activate hook installed at 0x{:x} (REL::ID 63380)"sv,
              reinterpret_cast<uintptr_t>(fn));

    // F.1.d: hook DropObject core (TESObjectREFR vtable slot 0x8B).
    REL::Relocation<void*> dropTarget{ REL::ID(63579) };  // RVA 0xB66680
    void* dropFn = dropTarget.get();
    MH_STATUS dst = MH_CreateHook(
        dropFn,
        reinterpret_cast<LPVOID>(&HookedDropCore),
        reinterpret_cast<LPVOID*>(&g_origDropCore));
    if (dst != MH_OK) {
        PLOG_WARN("[PULSAR] F.1.d: MH_CreateHook (DropCore) failed: {}"sv,
                  static_cast<int>(dst));
    } else {
        dst = MH_EnableHook(dropFn);
        if (dst != MH_OK) {
            PLOG_WARN("[PULSAR] F.1.d: MH_EnableHook (DropCore) failed: {}"sv,
                      static_cast<int>(dst));
        } else {
            PLOG_INFO(
                "[PULSAR] F.1.d: DropObject core hook installed at 0x{:x} (REL::ID 63579)"sv,
                reinterpret_cast<uintptr_t>(dropFn));
        }
    }

    // F.1.d: resolve inner REFR-spawn primitive. No hook - we call it
    // directly on the receiver side to spawn dropped items without going
    // through the VM-bound drop-core path. REL::ID 63591 @ RVA 0xB68F10.
    REL::Relocation<void*> spawnTarget{ REL::ID(63591) };
    g_spawnRef = reinterpret_cast<SpawnRefFn>(spawnTarget.get());
    PLOG_INFO(
        "[PULSAR] F.1.d: inner SpawnRef resolved at 0x{:x} (REL::ID 63591)"sv,
        reinterpret_cast<uintptr_t>(g_spawnRef));

    // M.2 V3: hook inner AddObjectToContainer at RVA 0xB4CBC0. Resolve via
    // raw RVA - no stable REL::ID known (CommonLibSF placeholder 85991
    // points to an unrelated 0xD0-byte struct copy helper; verified
    // 2026-05-08 via Ghidra Q4).
    {
        std::uintptr_t moduleBase =
            REX::FModule::GetExecutingModule().GetBaseAddress();
        void* addObjFn = reinterpret_cast<void*>(moduleBase + 0xB4CBC0);
        MH_STATUS aoSt = MH_CreateHook(
            addObjFn,
            reinterpret_cast<LPVOID>(&HookedAddObjInner),
            reinterpret_cast<LPVOID*>(&g_origAddObjInner));
        if (aoSt != MH_OK) {
            PLOG_WARN("[PULSAR] M.2 V3: MH_CreateHook (AddObjInner) failed: {}"sv,
                      static_cast<int>(aoSt));
        } else {
            aoSt = MH_EnableHook(addObjFn);
            if (aoSt != MH_OK) {
                PLOG_WARN("[PULSAR] M.2 V3: MH_EnableHook (AddObjInner) failed: {}"sv,
                          static_cast<int>(aoSt));
            } else {
                PLOG_INFO(
                    "[PULSAR] M.2 V3: AddObjectToContainer inner hook installed at 0x{:x} (RVA 0xB4CBC0)"sv,
                    reinterpret_cast<uintptr_t>(addObjFn));
            }
        }

        // M.2 V25: hook FUN_140404300 - engine's mod-array grow function.
        // Identified via Ghidra (m2_v24_growth_re.md). Skip-on-existing
        // semantics gated by t_insideShipSyncAddObj thread-local - see
        // HookedGrowMods.
        void* growFn = reinterpret_cast<void*>(moduleBase + 0x403B00);
        MH_STATUS gmSt = MH_CreateHook(
            growFn,
            reinterpret_cast<LPVOID>(&HookedGrowMods),
            reinterpret_cast<LPVOID*>(&g_origGrowMods));
        if (gmSt != MH_OK) {
            PLOG_WARN("[PULSAR] M.2 V25: MH_CreateHook (GrowMods) failed: {}"sv,
                      static_cast<int>(gmSt));
        } else {
            gmSt = MH_EnableHook(growFn);
            if (gmSt != MH_OK) {
                PLOG_WARN("[PULSAR] M.2 V25: MH_EnableHook (GrowMods) failed: {}"sv,
                          static_cast<int>(gmSt));
            } else {
                PLOG_INFO(
                    "[PULSAR] M.2 V25: GrowMods (FUN_140404300) hook installed at 0x{:x}"sv,
                    reinterpret_cast<uintptr_t>(growFn));
            }
        }
    }

    // F.1.e: resolve Papyrus UnequipItem native handler. Used on receiver
    // before raw-removing items from a dead-NPC corpse - clears the
    // equipped-item slot so engine reconciliation doesn't restore the
    // weapon ([Tarnung] / equipped marker).
    REL::Relocation<void*> unequipTarget{ REL::ID(116791) };
    g_unequipPapyrus = reinterpret_cast<PapyrusUnequipFn>(unequipTarget.get());
    PLOG_INFO(
        "[PULSAR] F.1.e: PapyrusUnequip resolved at 0x{:x} (REL::ID 116791)"sv,
        reinterpret_cast<uintptr_t>(g_unequipPapyrus));

    // F.2: ExtraDataList::GetByType - used to read kObjectInstance entry
    // from a REFR. REL::ID 37878 @ RVA 0x347170.
    REL::Relocation<void*> getExtraTarget{ REL::ID(37878) };
    g_getExtraByType = reinterpret_cast<GetExtraByTypeFn>(getExtraTarget.get());
    PLOG_INFO(
        "[PULSAR] F.2: GetExtraByType resolved at 0x{:x} (REL::ID 37878)"sv,
        reinterpret_cast<uintptr_t>(g_getExtraByType));

    // F.2: per-mod AttachMod inner. REL::ID 40716 @ RVA 0x403110.
    REL::Relocation<void*> attachModTarget{ REL::ID(40716) };
    g_attachMod = reinterpret_cast<AttachModFn>(attachModTarget.get());
    PLOG_INFO(
        "[PULSAR] F.2: AttachMod resolved at 0x{:x} (REL::ID 40716)"sv,
        reinterpret_cast<uintptr_t>(g_attachMod));

    // engine per-entry inventory mod-attach core. RVA 0x63D270 (resolve
    // by offset - no stable REL::ID needed for this internal helper).
    {
        REL::Relocation<void*> invCoreTarget{ REL::Offset(0x63D270) };
        g_attachModInvCore =
            reinterpret_cast<AttachModInvCoreFn>(invCoreTarget.get());
        PLOG_INFO(
            "[PULSAR] inventory AttachMod core resolved at 0x{:x} (RVA 0x63D270)"sv,
            reinterpret_cast<uintptr_t>(g_attachModInvCore));
    }
}

void TickDebugContainerTrace()
{
    // Deprecated - ContainerInvSink fires synchronously on events; no
    // polling needed. Kept for header ABI compatibility.
}

// M.2 (2026-05-08): Periodic poll of recently-activated tracked containers.
// Ship cargo uses BarterMenu (FUN_1415D15D0 'ShipInventoryData' ctor sets
// every slot to BarterMenu::vftable) - its inventoryList event source at
// +0xA0 never fires BGSInventoryListEvent on barter transfers. The diff
// won't otherwise reach DiffAndBroadcastContainer. This tick polls the
// snapshot every ~500ms and emits the same delta packets the event path
// would, for any container activated within the last 60s.
void TickContainerPolling()
{
    static uint64_t s_lastPollMs = 0;
    static uint64_t s_lastPruneMs = 0;
    const uint64_t now = ::GetTickCount64();
    // V13: faster cadence (200ms) so the 250ms-debounce window is responsive.
    if (now - s_lastPollMs < 200) return;
    s_lastPollMs = now;
    // V17: periodic prune of recently-applied REFR set (every 5s).
    if (now - s_lastPruneMs >= 5000ull) {
        s_lastPruneMs = now;
        PruneRecentlyApplied();
    }

    // M.2 V18 (2026-05-09): debounce-only. Pre-V18 OR'd `active` (60s
    // post-activate polling for ship-cargo) with `dirtyDebounced`, but
    // `active` defeated the debounce - every 200ms tick broadcast the
    // current diff during BarterMenu transactions, multiplying ammo and
    // re-spawning weapons. Ship-cargo is covered by the AddObjInner hook
    // (V3+), not polling, so the active-window path is unnecessary.
    // Only fire when the source has been quiet for >=250ms after an
    // event arrived.
    std::vector<void*> activeKeys;
    std::vector<uint32_t> trackedRefIDs;
    {
        std::lock_guard g(s_trackedMutex);
        activeKeys.reserve(s_tracked.size());
        trackedRefIDs.reserve(s_tracked.size());
        for (auto& [src, tc] : s_tracked) {
            trackedRefIDs.push_back(tc.containerRefID);
            const bool dirtyDebounced =
                tc.lastEventMs != 0 && (now - tc.lastEventMs) >= 250ull;
            // M.2 V22: max-defer force-flush. Without this, continuous
            // user activity (events every ~100ms < 250ms debounce) would
            // hold broadcasts indefinitely → host state diverges from
            // client. Force a broadcast every 1s of continuous activity.
            const bool forceFlush =
                tc.firstEventMs != 0 && (now - tc.firstEventMs) >= 1000ull;
            if (dirtyDebounced || forceFlush) {
                activeKeys.push_back(src);
                tc.lastEventMs = 0;
                tc.firstEventMs = 0;
            }
        }
    }

    // M.2 V26 (2026-05-09): continuous anchor restore. The engine grows
    // kObjectInstance mod-arrays asynchronously in the background - outside
    // any of our hooks (verified: V25 grow-skip never fires because
    // FUN_140404300 is always called with thread-local guard=off, meaning
    // it runs on a different thread/scheduler than HookedAddObjInner).
    //
    // Strategy: every tick, walk each tracked container, for each item with
    // an active anchor restore the original mods/sentinel. This races the
    // engine's growth and keeps the visible/persisted state at canonical.
    //
    // Cost: bounded - only runs on tracked containers (sink-registered),
    // skips items without anchors (silent no-op), and the silent variant
    // skips writes when state already matches.
    for (uint32_t refID : trackedRefIDs) {
        auto* refrForm = RE::TESForm::LookupByID(refID);
        if (!refrForm) continue;
        uint8_t ft = RawFormType(refrForm);
        if (ft != 0x4A && ft != 0x4B) continue;
        auto* refr = reinterpret_cast<RE::TESObjectREFR*>(refrForm);
        int32_t reverted = SafeRestoreAllAnchoredInContainer(refr);
        if (reverted > 0) {
            PLOG_INFO(
                "[PULSAR] M.2 V26 polling-revert: refr=0x{:08X} items={}"sv,
                refID, reverted);
        }
    }

    for (void* src : activeKeys) {
        DiffAndBroadcastContainer(src);
    }
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;
    if (!data || length < 1) return;

    // PKT_LOOT_DROP dispatch - smallest payload, check first.
    if (data[0] == PKT_LOOT_DROP) {
        if (length < 1 + sizeof(LootDropData)) return;
        LootDropData d{};
        std::memcpy(&d, data + 1, sizeof(d));

        uint32_t resolved = 0;
        if (d.itemPlugin[0] != '\0') {
            resolved = NPCSync::PluginLookup::ResolveCrossPlugin(
                d.itemPlugin, d.itemLocalID);
        }
        if (!resolved) resolved = d.itemBase;

        // F.2: extract trailing mod descriptors and resolve each via the
        // same cross-plugin lookup as the item base.
        std::vector<uint32_t> resolvedMods;
        if (d.modCount > 0 && d.modCount <= kMaxModsPerDrop) {
            size_t modBytes = static_cast<size_t>(d.modCount)
                * sizeof(LootDropMod);
            if (length >= 1 + sizeof(LootDropData) + modBytes) {
                resolvedMods.reserve(d.modCount);
                const uint8_t* modPtr = data + 1 + sizeof(LootDropData);
                for (uint8_t i = 0; i < d.modCount; ++i) {
                    LootDropMod m{};
                    std::memcpy(&m, modPtr + i * sizeof(LootDropMod),
                                 sizeof(LootDropMod));
                    uint32_t mr = 0;
                    if (m.plugin[0] != '\0') {
                        mr = NPCSync::PluginLookup::ResolveCrossPlugin(
                            m.plugin, m.localID);
                    }
                    if (!mr) mr = m.base;
                    if (mr) resolvedMods.push_back(mr);
                }
            }
        }

        {
            std::lock_guard g(s_pendingMutex);
            s_pendingDrops.push_back({
                resolved, d.itemFormType, d.count,
                d.modSentinel, std::move(resolvedMods)
            });
        }
        PLOG_INFO(
            "[PULSAR] F.1.d RX: item=0x{:08X} type=0x{:02X} count={} plugin='{}' resolved=0x{:08X} mods={}"sv,
            d.itemBase, d.itemFormType, d.count,
            d.itemPlugin[0] ? d.itemPlugin : "(none)",
            resolved, d.modCount);
        return;
    }

    // PKT_CONTAINER_TAKE dispatch first (longer payload, different type byte).
    if (data[0] == PKT_CONTAINER_TAKE) {
        if (length < 1 + sizeof(ContainerTakeData)) return;
        ContainerTakeData t{};
        std::memcpy(&t, data + 1, sizeof(t));

        uint32_t resolvedContainerBase = 0;
        if (t.containerPlugin[0] != '\0') {
            resolvedContainerBase = NPCSync::PluginLookup::ResolveCrossPlugin(
                t.containerPlugin, t.containerLocalID);
        }
        if (!resolvedContainerBase) resolvedContainerBase = t.containerBase;

        uint32_t resolvedItemBase = 0;
        if (t.itemPlugin[0] != '\0') {
            resolvedItemBase = NPCSync::PluginLookup::ResolveCrossPlugin(
                t.itemPlugin, t.itemLocalID);
        }
        if (!resolvedItemBase) resolvedItemBase = t.itemBase;

        // M.2 V5: forward transferReason; default 4 (kStoreInContainer) when
        // sender is on a pre-V5 build that wrote 0/garbage.
        uint8_t reason = t.transferReason ? t.transferReason : 4;

        // M.2 V7: parse trailing mod array; cross-plugin resolve each.
        std::vector<uint32_t> resolvedMods;
        if (t.modCount > 0 && t.modCount <= kMaxModsPerDrop) {
            size_t needed = 1 + sizeof(ContainerTakeData)
                            + t.modCount * sizeof(LootDropMod);
            if (length >= needed) {
                resolvedMods.reserve(t.modCount);
                for (uint8_t i = 0; i < t.modCount; ++i) {
                    LootDropMod m{};
                    std::memcpy(&m, data + 1 + sizeof(ContainerTakeData)
                                + i * sizeof(LootDropMod), sizeof(m));
                    uint32_t rm = 0;
                    if (m.plugin[0] != '\0') {
                        rm = NPCSync::PluginLookup::ResolveCrossPlugin(
                            m.plugin, m.localID);
                    }
                    if (!rm) rm = m.base;
                    if (rm) resolvedMods.push_back(rm);
                }
            }
        }

        {
            std::lock_guard g(s_pendingMutex);
            PendingContainerTake p{};
            p.resolvedContainerBase = resolvedContainerBase;
            p.resolvedItemBase      = resolvedItemBase;
            p.px = t.posX; p.py = t.posY; p.pz = t.posZ;
            p.count = t.count;
            p.refType = t.containerRefType;
            p.transferReason = reason;
            p.modSentinel = t.modSentinel;
            p.resolvedMods = std::move(resolvedMods);
            s_pendingContainerTakes.push_back(std::move(p));
        }

        PLOG_INFO(
            "[PULSAR] ContainerSync RX: container base=0x{:08X} type=0x{:02X} item=0x{:08X} x{} mods={} reason={} @({:.0f},{:.0f},{:.0f})"sv,
            resolvedContainerBase, t.containerRefType,
            resolvedItemBase, t.count, t.modCount, reason,
            t.posX, t.posY, t.posZ);
        return;
    }

    if (length < 1 + sizeof(LootPickupData)) return;
    if (data[0] != PKT_LOOT_PICKUP) return;

    LootPickupData p{};
    std::memcpy(&p, data + 1, sizeof(p));

    // Re-encode the host's base FormID into our load-order. Fall back to
    // the raw base when the plugin isn't in our list or ESL/ESM flag-aware
    // mapping can't be resolved - exact FormID matches still work when the
    // plugin happens to load at the same compile index on both machines.
    uint32_t resolved = 0;
    if (p.basePlugin[0] != '\0') {
        resolved = NPCSync::PluginLookup::ResolveCrossPlugin(
            p.basePlugin, p.baseLocalID);
    }
    if (!resolved) resolved = p.baseFormID;

    {
        std::lock_guard g(s_pendingMutex);
        s_pending.push_back({
            resolved, p.baseFormType, p.posX, p.posY, p.posZ
        });
    }

    PLOG_INFO(
        "[PULSAR] LootSync RX: base=0x{:08X} type=0x{:02X} plugin='{}' localID=0x{:X} resolved=0x{:08X} @({:.0f},{:.0f},{:.0f})"sv,
        p.baseFormID, p.baseFormType,
        p.basePlugin[0] ? p.basePlugin : "(none)",
        p.baseLocalID, resolved, p.posX, p.posY, p.posZ);
}

void DrainPendingPickups()
{
    // F.2 deferred drop-reads: process drops queued by HookedDropCore.
    // Cell-scan + kObjectInstance read happens HERE (game-tick), away
    // from the inventory-UI re-render that races with sync reads when
    // done from the hook itself.
    std::vector<PendingDropRead> dropReads;
    {
        std::lock_guard g(s_pendingDropReadsMutex);
        if (!s_pendingDropReads.empty()) dropReads.swap(s_pendingDropReads);
    }
    for (auto& dr : dropReads) {
        std::vector<uint32_t> mods;
        uint16_t sentinel = 0xFFFF;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (p && p->parentCell) {
            auto* spawned = SafeFindLootRefInCell(p->parentCell,
                                                   dr.itemBase,
                                                   dr.px, dr.py, dr.pz,
                                                   300.0f);
            if (spawned) {
                DumpModEntriesVerbose(spawned);
                ReadModFormIDs(spawned, mods);
                sentinel = ReadWorldRefrModSentinel(spawned);
            }
        }
        SendDropPacket(dr.itemBase, dr.itemType, dr.count, mods, sentinel);
    }

    std::vector<PendingPickup> work;
    {
        std::lock_guard g(s_pendingMutex);
        // Drain ALL queues - early-return only if NONE has work,
        // otherwise container takes get stranded when no world-pickup is
        // pending.
        if (s_pending.empty() && s_pendingContainerTakes.empty()
            && s_pendingDrops.empty()) return;
        work.swap(s_pending);
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        // Put them back - try next tick when the singleton exists.
        std::lock_guard g(s_pendingMutex);
        s_pending.insert(s_pending.begin(), work.begin(), work.end());
        return;
    }

    for (auto& p : work) {
        auto* refr = FindLocalLootRef(p);
        if (!refr) {
            PLOG_INFO(
                "[PULSAR] LootSync: no local match for base=0x{:08X} @({:.0f},{:.0f},{:.0f}) - item may be in unloaded cell"sv,
                p.resolvedBase, p.px, p.py, p.pz);
            continue;
        }

        uint32_t matchedRefID = RawFormID(refr);

        // Replay Activate using the OTHER player's clone as activator. The
        // clone is a real Actor (type kACHR), so the engine runs the full
        // pickup path and despawns the REFR - but the item lands in the
        // clone's inventory, not the local player's. The clone inventory is
        // invisible to the UI on this side, so effectively no dupe.
        //
        // Fallback: if no clone is available (not spawned yet / F.2 hasn't
        // rebuilt equipment-sync), drop to SetDelete - visual stays but at
        // least no dupe.
        auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
        bool cleaned = false;
        if (clone) {
            ActivateArgs args{};
            args.activator   = reinterpret_cast<RE::TESObjectREFR*>(clone);
            args.countOrFlag = 1;
            args.flag1c      = 0;
            args.flag1d      = 1;
            t_replayingFromPacket = true;
            SafeCallOrigActivate(refr, &args, matchedRefID);
            t_replayingFromPacket = false;
            cleaned = true;
        } else {
            // No clone yet - at least mark the REFR deleted so the game save
            // doesn't preserve a ghost item.
            cleaned = SafeDisableAndDelete(refr, matchedRefID);
        }

        PLOG_INFO(
            "[PULSAR] LootSync: despawned local ref=0x{:08X} (base=0x{:08X}) via={} result={}"sv,
            matchedRefID, p.resolvedBase,
            clone ? "clone-activator" : "setdelete",
            cleaned ? "ok" : "failed");
    }

    // Container-take draining. Each pending take identifies a container by
    // (resolvedBase, position) and an item+count to remove from it. Use the
    // same cell-scan pattern as FindLocalLootRef but filter by exact base.
    std::vector<PendingContainerTake> ctWork;
    {
        std::lock_guard g(s_pendingMutex);
        if (!s_pendingContainerTakes.empty()) {
            ctWork.swap(s_pendingContainerTakes);
        }
    }
    for (auto& t : ctWork) {
        bool isActor = (t.refType == static_cast<uint8_t>(RE::FormType::kACHR));
        RE::TESObjectREFR* container = nullptr;

        // M.2 V4 (2026-05-08): direct formID lookup first - handles ship-
        // form containers (e.g. Frontier 0x0003F7E8) that aren't in any
        // active cell. Falls through to position-based cell-scan for
        // ordinary containers (also covers cross-plugin remap case).
        auto* directForm = RE::TESForm::LookupByID(t.resolvedContainerBase);
        if (directForm) {
            uint8_t ft = RawFormType(directForm);
            if (ft == 0x4A || ft == 0x4B || ft == t.refType) {
                container = reinterpret_cast<RE::TESObjectREFR*>(directForm);
            }
        }

        // For actors: position-only match (base FormID is dynamic, useless
        // across machines). For containers: match by base FormID + position.
        auto findInCell = [&](void* cell) -> RE::TESObjectREFR* {
            if (isActor) {
                return SafeFindActorRefByPos(cell, t.px, t.py, t.pz, 1200.0f);
            }
            return SafeFindContainerByBaseInCell(cell, t.resolvedContainerBase,
                                                  t.px, t.py, t.pz, 1200.0f);
        };

        if (!container && player && player->parentCell) {
            container = findInCell(player->parentCell);
        }
        if (!container) {
            auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
            for (auto* cell : cells) {
                if (!cell) continue;
                if (player && cell == player->parentCell) continue;
                container = findInCell(cell);
                if (container) break;
            }
        }
        if (!container) {
            PLOG_INFO(
                "[PULSAR] ContainerSync: no local {} matched base=0x{:08X} type=0x{:02X} @({:.0f},{:.0f},{:.0f})"sv,
                isActor ? "actor" : "container",
                t.resolvedContainerBase, t.refType, t.px, t.py, t.pz);
            continue;
        }

        // M.2 V4 - encoded count semantics:
        //   t.count > 0 : take from container (host's player gained items)
        //                 → on receiver: AddObjInner(localPlayer, item, ..., count, container)
        //                   moves items from local container to local player
        //   t.count < 0 : place into container (host's player lost items)
        //                 → on receiver: AddObjInner(container, item, ..., -count, NULL)
        //                   spawns items into local container
        // Direct AddObjInner replay (instead of vt[0x9A] / RawRemove) so
        // ship-form storage gets updated by the engine's specialised path
        // the +0xA0 inventoryList path doesn't reach ship cargo.
        auto* itemForm = RE::TESForm::LookupByID(t.resolvedItemBase);
        uint32_t cRefID = RawFormID(container);
        if (!itemForm) {
            PLOG_INFO(
                "[PULSAR] ContainerSync: SKIP - item base=0x{:08X} not in form-table on receiver (mirror {}=0x{:08X})"sv,
                t.resolvedItemBase, isActor ? "actor" : "container", cRefID);
            continue;
        }
        // M.2 V5: helper for inventory-count diagnostic - counts total items
        // of `formID` in the REFR's BGSInventoryList at +0xA0. SEH-safe.
        auto countItemInRefrInv = [](RE::TESObjectREFR* refr, uint32_t formID) -> int32_t {
            if (!refr || !formID) return -1;
            int32_t total = -1;
            __try {
                auto* pb = reinterpret_cast<uint8_t*>(refr);
                auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
                if (!invList) { total = -1; }
                else {
                    SnapshotEntry buf[64]{};
                    int32_t n = CollectInventoryEntries(invList, buf, 64);
                    total = 0;
                    for (int32_t i = 0; i < n; ++i) {
                        if (buf[i].base == formID) total += buf[i].count;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { total = -2; }
            return total;
        };

        if (t.count < 0) {
            int32_t addCount = -t.count;
            int32_t preCount = countItemInRefrInv(container, t.resolvedItemBase);

            // M.2 V20 (2026-05-09): pre-snapshot container's full inv so
            // we can diff after AddObjInner and remove any items the
            // engine auto-spawned (NPC-loadout default ammo for placed
            // weapons - V19's kScriptAddItem reason didn't suppress it).
            SnapshotEntry preSnapBuf[kMaxSnapshotEntries];
            int32_t preSnapN = SafeSnapshotRefrInventory(
                container, preSnapBuf,
                static_cast<int32_t>(kMaxSnapshotEntries));

            bool ok = SafeAddObjInnerReplay(
                container, itemForm, addCount,
                /*src=*/nullptr,
                static_cast<uint32_t>(RE::ITEM_TRANSFER_REASON::kScriptAddItem));
            int32_t postCount = countItemInRefrInv(container, t.resolvedItemBase);
            // V7+V15+V16: replace mod slots + truncate excess proc-gen
            // rolls + write host's +0x28 sentinel (item tier).
            if (ok && !t.resolvedMods.empty()) {
                SafeReplaceModArrayOnInventoryItem(
                    container, t.resolvedItemBase, t.resolvedMods,
                    t.modSentinel);

                // V26: seed anchor on receiver side using the broadcasted
                // mods. Without this, only the sender (who took the item)
                // has an anchor - but the engine grows on BOTH sides, and
                // polling-restore needs an anchor on each side to fight it.
                std::vector<uint32_t> anchorCopy = t.resolvedMods;
                V24_RecordTakeObservation(t.resolvedItemBase,
                                           std::move(anchorCopy),
                                           t.modSentinel);
            }

            // M.2 V20: post-snapshot diff and remove auto-spawned items
            // (engine NPC-loadout adds default ammo on weapon place).
            // Compare post-state with pre-snapshot; any base that grew
            // EXCEPT our intended item gets RawRemove'd. Treats cleaning
            // best-effort (removed count may be < expected if engine
            // tracked the items elsewhere).
            if (ok) {
                SnapshotEntry postSnapBuf[kMaxSnapshotEntries];
                int32_t postSnapN = SafeSnapshotRefrInventory(
                    container, postSnapBuf,
                    static_cast<int32_t>(kMaxSnapshotEntries));

                std::unordered_map<uint32_t, int32_t> preMap;
                for (int32_t i = 0; i < preSnapN; ++i) {
                    preMap[preSnapBuf[i].base] = preSnapBuf[i].count;
                }
                int32_t cleanedItems = 0;
                int32_t cleanedTotal = 0;
                for (int32_t i = 0; i < postSnapN; ++i) {
                    uint32_t baseID = postSnapBuf[i].base;
                    int32_t  postC  = postSnapBuf[i].count;
                    if (baseID == t.resolvedItemBase) continue;  // our item - skip
                    int32_t preC = 0;
                    auto it = preMap.find(baseID);
                    if (it != preMap.end()) preC = it->second;
                    int32_t excess = postC - preC;
                    if (excess > 0) {
                        int32_t removed2 = RawRemoveFromContainer(
                            container, baseID, excess);
                        cleanedItems++;
                        cleanedTotal += removed2;
                        PLOG_INFO(
                            "[PULSAR] M.2 V20 auto-spawn cleanup: refr=0x{:08X} item=0x{:08X} {}->{} removed={}"sv,
                            cRefID, baseID, preC, postC, removed2);
                    }
                }
                if (cleanedItems > 0) {
                    PLOG_INFO(
                        "[PULSAR] M.2 V20 cleanup summary: refr=0x{:08X} entries={} total-removed={}"sv,
                        cRefID, cleanedItems, cleanedTotal);
                }
            }
            PLOG_INFO(
                "[PULSAR] ContainerSync: mirror {}=0x{:08X} item=0x{:08X} placed={} pre={} post={} mods={} reason={} result={}"sv,
                isActor ? "actor" : "container", cRefID,
                t.resolvedItemBase, addCount, preCount, postCount,
                static_cast<uint32_t>(t.resolvedMods.size()),
                t.transferReason, ok ? "v15-ok" : "SEH");
            continue;
        }
        // Take: t.count > 0
        // M.2 V9 (2026-05-08): try AddObjInner with NEGATIVE count to
        // trigger both the inventory removal AND the engine UI-refresh
        // event in one call. PLACE direction works live (calls AddObjInner
        // with +count). TAKE was using RawRemove which mutates +0xA0 but
        // doesn't fire events → BarterMenu UI cached. Hypothesis: signed
        // count → engine treats negative as remove. If diagnostic shows
        // count not honoured (src didn't decrease or worse, increased),
        // fall back to RawRemove.
        if (t.count > 0) {
            int32_t preSrc = countItemInRefrInv(container, t.resolvedItemBase);
            bool ok = SafeAddObjInnerReplay(
                container, itemForm, /*count=*/-t.count,
                /*src=*/nullptr, t.transferReason);
            int32_t postSrc = countItemInRefrInv(container, t.resolvedItemBase);
            int32_t removed = preSrc - postSrc;
            // V9 fallback: if engine didn't honour negative count (postSrc
            // unchanged or grew), do raw mutation so cargo state still
            // mirrors. Two failure modes handled:
            // postSrc == preSrc : no-op, just RawRemove the count
            // postSrc >  preSrc : engine ADDED items; RawRemove the
            //                          unwanted growth + the intended count
            // If neither (success, postSrc < preSrc) we skip the fallback.
            if (postSrc >= preSrc) {
                int32_t want = t.count + (postSrc - preSrc);
                int32_t fallback = RawRemoveFromContainer(container,
                                                          t.resolvedItemBase,
                                                          want);
                int32_t postSrc2 = countItemInRefrInv(container, t.resolvedItemBase);
                PLOG_INFO(
                    "[PULSAR] ContainerSync: mirror {}=0x{:08X} item=0x{:08X} taken={} V9-NEG-FAILED fallback-removed={}/{} src={}->{}->{} reason={} result=raw-fallback"sv,
                    isActor ? "actor" : "container", cRefID,
                    t.resolvedItemBase, t.count, fallback, want,
                    preSrc, postSrc, postSrc2, t.transferReason);
                continue;
            }
            PLOG_INFO(
                "[PULSAR] ContainerSync: mirror {}=0x{:08X} item=0x{:08X} taken={} removed={} src={}->{} reason={} result=v9-neg-{}"sv,
                isActor ? "actor" : "container", cRefID,
                t.resolvedItemBase, t.count, removed,
                preSrc, postSrc, t.transferReason, ok ? "ok" : "SEH");

            // F.1.e/F.8 visual-child fallback - preserved from pre-V4 path.
            // Dead-NPC corpse: equipped weapon spawns world REFR; cull it.
            // Display-case kCONT: weapon-case script PlaceAtMe-spawns visual
            // children that aren't in the inventoryList; always cull them
            // wide-radius (kSpawnContainer extra-data filter inside
            // CullAllVisualChildrenInCells excludes editor-placed lootables).
            if (isActor) {
                auto* player2 = RE::PlayerCharacter::GetSingleton();
                RE::TESObjectREFR* worldRef = nullptr;
                if (player2 && player2->parentCell) {
                    worldRef = SafeFindLootRefInCell(player2->parentCell,
                                                      t.resolvedItemBase,
                                                      t.px, t.py, t.pz, 500.0f);
                }
                if (!worldRef) {
                    auto cells2 = Pulsar::NPCSync::GetActiveCellsSnapshot();
                    for (auto* cell : cells2) {
                        if (!cell) continue;
                        if (player2 && cell == player2->parentCell) continue;
                        worldRef = SafeFindLootRefInCell(cell,
                                                          t.resolvedItemBase,
                                                          t.px, t.py, t.pz, 500.0f);
                        if (worldRef) break;
                    }
                }
                if (worldRef) {
                    uint32_t wRefID = RawFormID(worldRef);
                    auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
                    bool vok = false;
                    const char* via = "setdelete";
                    if (clone) {
                        ActivateArgs args{};
                        args.activator   = reinterpret_cast<RE::TESObjectREFR*>(clone);
                        args.countOrFlag = 1;
                        args.flag1c      = 0;
                        args.flag1d      = 1;
                        t_replayingFromPacket = true;
                        SafeCallOrigActivate(worldRef, &args, wRefID);
                        t_replayingFromPacket = false;
                        vok = true;
                        via = "clone-activator";
                    } else {
                        vok = SafeDisableAndDelete(worldRef, wRefID);
                    }
                    PLOG_INFO(
                        "[PULSAR] ContainerSync: visual-fallback actor despawned worldREFR=0x{:08X} (base=0x{:08X}) via={} result={}"sv,
                        wRefID, t.resolvedItemBase, via, vok ? "ok" : "failed");
                }
            } else {
                int n = CullAllVisualChildrenInCells(t.resolvedItemBase,
                                                      t.px, t.py, t.pz, 200.0f);
                if (n > 0) {
                    PLOG_INFO(
                        "[PULSAR] ContainerSync: visual-fallback container culled={} base=0x{:08X} @({:.0f},{:.0f},{:.0f})"sv,
                        n, t.resolvedItemBase, t.px, t.py, t.pz);
                }
            }
            continue;
        }
    }

    // F.1.d drop replay. Pop pending drops, look up the base Form, find the
    // remote clone actor, call g_origDropCore(clone, &out, &args). The engine
    // handles physics + cell-reference registration; REFR spawns at clone's
    // position (= peer player's position on our side). No inventory pre-add
    // here - if the drop-core needs the item in clone's inventory, it will
    // no-op gracefully (observed paths are lenient about this).
    std::vector<PendingDrop> dropWork;
    {
        std::lock_guard g(s_pendingMutex);
        if (!s_pendingDrops.empty()) {
            dropWork.swap(s_pendingDrops);
        }
    }
    for (auto& dp : dropWork) {
        auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
        if (!clone) {
            PLOG_WARN(
                "[PULSAR] F.1.d: drop replay skipped - no remote clone for base=0x{:08X}"sv,
                dp.resolvedBase);
            continue;
        }
        auto* item = RE::TESForm::LookupByID(dp.resolvedBase);
        if (!item) {
            PLOG_WARN(
                "[PULSAR] F.1.d: drop replay skipped - unknown base=0x{:08X}"sv,
                dp.resolvedBase);
            continue;
        }

        uint32_t handle = SafeSpawnDroppedRef(
            reinterpret_cast<RE::TESObjectREFR*>(clone),
            item, dp.count, dp.resolvedBase);

        // F.2: locate the freshly-spawned REFR via cell scan around the
        // F.2-V1 in-place mod replace + extras-AttachMod for legendary
        // effects: spawn already created the REFR with a default
        // kObjectInstance entry. Overwrite common form pointers in-place
        // (no side effects). For extra mods (host > local - legendary
        // effects, epic-tier rolls), call AttachMod for those few. Few
        // calls = manageable side-effect risk.
        if (handle && !dp.resolvedMods.empty()) {
            RE::TESObjectREFR* spawned = FindSpawnedRefNearActor(
                reinterpret_cast<RE::TESObjectREFR*>(clone),
                dp.resolvedBase);
            if (spawned) {
                SafeReplaceModArrayInPlace(spawned, dp.resolvedMods, dp.modSentinel);
                MarkRefrApplied(spawned);  // M.2 V17 - block dedup races

                // Legendary-effect extras: anything beyond local slot
                // count needs AttachMod since the in-place replace can't
                // grow the array.
                uint32_t localCount = SafeReadModSlotCount(spawned);
                size_t extraStart = localCount;
                if (extraStart < dp.resolvedMods.size()) {
                    for (size_t i = extraStart; i < dp.resolvedMods.size(); ++i) {
                        auto* m = RE::TESForm::LookupByID(dp.resolvedMods[i]);
                        if (m) SafeAttachMod(spawned, m);
                    }
                    PLOG_INFO(
                        "[PULSAR] F.2 V1 attached {} legendary-extras (slots {}..{}) on refr=0x{:08X}"sv,
                        dp.resolvedMods.size() - extraStart,
                        static_cast<uint32_t>(extraStart),
                        static_cast<uint32_t>(dp.resolvedMods.size() - 1),
                        RawFormID(spawned));
                }
            }
        }

        PLOG_INFO(
            "[PULSAR] F.1.d: drop spawn base=0x{:08X} count={} anchor=clone -> handle=0x{:x} ({}) mods={}"sv,
            dp.resolvedBase, dp.count, handle,
            handle ? "ok" : "failed",
            dp.resolvedMods.size());
    }

}

// Replay primitive shared with sibling sync modules (DoorSync, future).
// Sets the t_replayingFromPacket guard so the inbound replay doesn't get
// re-broadcast by our own hook, then dispatches via the SEH-wrapped
// trampoline call. Defined outside the anonymous namespace so the linker
// can resolve external callers - but inside namespace Loot so it can name
// the anon-ns helpers without qualification.
void CallOrigActivateAsReplay(RE::TESObjectREFR* refr, void* args)
{
    if (!refr || !args) return;
    auto* prevRefr = t_currentReplayingRefr;
    bool  prevFlag = t_replayingFromPacket;
    t_replayingFromPacket   = true;
    t_currentReplayingRefr  = refr;
    SafeCallOrigActivate(refr, static_cast<ActivateArgs*>(args),
                          RawFormID(refr));
    t_currentReplayingRefr  = prevRefr;
    t_replayingFromPacket   = prevFlag;
}

bool SetReplayingFromPacket(bool value)
{
    const bool prev = t_replayingFromPacket;
    t_replayingFromPacket = value;
    return prev;
}

bool IsReplayingFromPacket()
{
    return t_replayingFromPacket;
}

// F.2-v2: public wrappers around the M.2 mod primitives (file-statics above).
void ReadItemMods(RE::TESObjectREFR* a_container, std::uint32_t a_itemFormID,
                  std::vector<std::uint32_t>& a_outMods, std::uint16_t& a_outSentinel)
{
    a_outSentinel = 0xFFFF;
    a_outMods.clear();
    if (!a_container || !a_itemFormID) return;
    ReadInventoryItemMods(a_container, a_itemFormID, a_outMods);
    a_outSentinel = ReadInventoryItemModSentinel(a_container, a_itemFormID);
}

void ApplyItemMods(RE::TESObjectREFR* a_container, std::uint32_t a_itemFormID,
                   const std::vector<std::uint32_t>& a_mods, std::uint16_t a_sentinel)
{
    if (!a_container || !a_itemFormID) return;
    SafeRestoreModArrayOnInventoryItem(a_container, a_itemFormID, a_mods, a_sentinel);
}

// native engine mod-attach - see header. Restore-only; correct slots,
// allocates+grows. Returns mods attached.
std::uint32_t AttachItemModsNative(RE::TESObjectREFR* a_owner,
                                   std::uint32_t a_itemFormID,
                                   const std::vector<std::uint32_t>& a_mods,
                                   std::uint16_t a_sentinel)
{
    if (!a_owner || !a_itemFormID || a_mods.empty()) return 0;
    return SafeAttachModNativeToInventoryItem(a_owner, a_itemFormID, a_mods, a_sentinel);
}

// ===================================================================
// Key/keycard mirror. When ANY player loots a key (FormType kKEYM),
// mirror it into the peer's inventory so neither gets stuck at a loading-
// screen door that needs the key. Poll-based (catches every loot path -
// container, world, corpse, quest-reward, script-give - without per-path
// hooks). Idempotent: both sides track seen keys (s_knownKeys) so a mirrored
// key is never echoed back. Keys are safe to duplicate (door only checks
// "has key X"); quest turn-in items are deliberately NOT mirrored.
// ===================================================================
static std::mutex                   s_keyMutex;
static std::unordered_set<uint32_t> s_knownKeys;        // base FormIDs already seen/sent
static std::deque<uint32_t>         s_pendingKeyAdds;    // keys to add on the game thread
static uint64_t                     s_lastKeyScanMs = 0;
constexpr uint64_t                  kKeyScanIntervalMs = 2000;

void OnKeyMirrorReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;
    if (!data || length < 1 + sizeof(KeyMirrorData)) return;
    KeyMirrorData d{};
    std::memcpy(&d, data + 1, sizeof(d));
    d.plugin[sizeof(d.plugin) - 1] = '\0';
    uint32_t resolved = 0;
    if (d.plugin[0] != '\0')
        resolved = NPCSync::PluginLookup::ResolveCrossPlugin(d.plugin, d.localID);
    if (!resolved) resolved = d.base;
    if (!resolved) return;
    // Mark known (suppress echo) + queue the inventory-add for the game thread
    // (engine inventory ops are not safe on the network thread).
    std::lock_guard<std::mutex> lk(s_keyMutex);
    if (s_knownKeys.insert(resolved).second)
        s_pendingKeyAdds.push_back(resolved);
}

void TickKeySync()
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected()) return;
    auto* plr = RE::PlayerCharacter::GetSingleton();
    if (!plr) return;

    // One inventory snapshot (robust SEH-guarded walk) used for both the
    // add-dedup and the broadcast scan.
    ContainerSnapshot snap;
    SnapshotInventory(static_cast<RE::TESObjectREFR*>(plr), snap);
    std::unordered_set<uint32_t> present;
    present.reserve(snap.items.size());
    for (const auto& it : snap.items) present.insert(it.first);

    // 1) drain peer-mirrored keys → add to local player if missing.
    std::deque<uint32_t> toAdd;
    { std::lock_guard<std::mutex> lk(s_keyMutex); toAdd.swap(s_pendingKeyAdds); }
    for (uint32_t fid : toAdd) {
        if (present.count(fid)) continue;                 // already have it
        auto* form = RE::TESForm::LookupByID(fid);
        if (!form || form->GetFormType() != RE::FormType::kKEYM) continue;
        auto* bound = reinterpret_cast<RE::TESBoundObject*>(form);  // kKEYM => TESKey : TESBoundObject
        const bool prev = SetReplayingFromPacket(true);   // don't re-broadcast this add
        plr->AddObjectToContainer(bound, nullptr, 1, nullptr, RE::ITEM_TRANSFER_REASON::kNone);
        SetReplayingFromPacket(prev);
        present.insert(fid);
        PLOG_INFO("[PULSAR] [KEYSYNC] +key 0x{:08X} (mirrored from peer)"sv, fid);
    }

    // 2) scan local inventory for NEW keys → broadcast (rate-limited).
    uint64_t now = GetTickCount64();
    if (now - s_lastKeyScanMs < kKeyScanIntervalMs) return;
    s_lastKeyScanMs = now;

    for (const auto& it : snap.items) {
        const uint32_t fid = it.first;
        auto* form = RE::TESForm::LookupByID(fid);
        if (!form || form->GetFormType() != RE::FormType::kKEYM) continue;
        bool isNew;
        { std::lock_guard<std::mutex> lk(s_keyMutex); isNew = s_knownKeys.insert(fid).second; }
        if (!isNew) continue;
        Pulsar::Network::KeyMirrorData d{};
        d.base    = fid;
        d.localID = NPCSync::PluginLookup::FormIDToLocalID(fid);
        NPCSync::PluginLookup::FormIDToPluginName(fid, d.plugin, sizeof(d.plugin));
        std::uint8_t buf[1 + sizeof(Pulsar::Network::KeyMirrorData)];
        buf[0] = static_cast<std::uint8_t>(Pulsar::Network::PKT_KEY_MIRROR);
        std::memcpy(buf + 1, &d, sizeof(d));
        net.Send(buf, sizeof(buf), true);
        PLOG_INFO("[PULSAR] [KEYSYNC] broadcast key 0x{:08X} to peer"sv, fid);
    }
}

}  // namespace Pulsar::WorldSync::Loot
