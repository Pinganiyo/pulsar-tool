#include "PCH.h"
#include "QuestSync/QuestSync.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "EventTrace/EventTrace.h"
#include "QuestSync/StageJournal.h"

#include <REL/Relocation.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESQuest.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

using namespace std::string_view_literals;

namespace Pulsar::QuestSync {
namespace {

    // L.8 Phase 1 - native TESQuest stage-set worker. Verified 2026-05-09 via
    // Ghidra wrapper-chain decompile (FUN_1420CF3A0 Papyrus → FUN_1420D54D0
    // inner → FUN_140D0E1F0 core). Clean signature, no Papyrus VM dependency.
    // The worker:
    // Gates on TESQuest+0x114 bit 0 (running flag)
    // Validates stage exists in the per-quest stage hash table @ +0x1A0
    // Calls subordinate stage-script worker FUN_140D2B290
    // Updates highestSetStageID @ +0x328 if newStage > current
    // Calls vt[0x17] with 0x80000000 (HUD recalc)
    // Calls FUN_140CF2E60 which Notifies TESQuestStageEvent on the global
    //     BSTEventSource @ RVA 0x5978238
    // Returns true if a stage script was found+executed, false otherwise.
    using SetStageWorker_t = bool (*)(RE::TESQuest*, std::uint16_t);
    static constexpr std::uintptr_t kRVA_SetStageWorker = 0xD0DAC0;

    static SetStageWorker_t g_origSetStage = nullptr;

    // L.8 Phase 2 - TESQuest::Start engine core. RE'd 2026-05-09 (Ghidra
    // sub-agent): only call site of the StartStopEvent Notify wrapper
    // FUN_140B984F0 in the entire binary. Sets +0x114 |= 0x80 (priming),
    // expands aliases/objectives, clears bit 7, sets *outOk=1, fires
    // BSTEventSource<TESQuestStartStopEvent>::Notify(formID, started=1).
    // Signature: param_1 = QuestStartCtx* (caller frame, not portable),
    // param_2 = TESQuest** (deref to get the quest), param_3 = byte* outOk.
    // We hook only for TX broadcast - RX does NOT replay this because
    // QuestStartCtx is caller-frame state we can't reconstruct on the peer.
    // Phase 1 stage sync triggers quest-start naturally on the peer when
    // the initial-stage scripts run (Bethesda's standard pattern).
    using StartWorker_t = void (*)(void* ctx, RE::TESQuest** questPtr, char* outOk);
    static constexpr std::uintptr_t kRVA_StartWorker = 0xEEF850;
    static StartWorker_t g_origStart = nullptr;

    // L.8 Phase 2 - TESQuest::Stop engine core. RE'd 2026-05-09 (Ghidra
    // sub-agent): clean (TESQuest*, char) signature. Walks aliases,
    // clears +0x114 bit 0 (running flag), calls vt[0xb8](this, 2), fires
    // stage event via FUN_140CF2DF0 Notify, then if param_2 != 0 calls
    // FUN_140B984F0(formID, started=0, ?) to fire StartStopEvent.
    // Hook for TX broadcast + RX replay (signature is fully portable).
    using StopWorker_t = void (*)(RE::TESQuest* quest, char param_2);
    static constexpr std::uintptr_t kRVA_StopWorker = 0xCF9970;
    static StopWorker_t g_origStop = nullptr;

    // Echo-suppression: when applying a remote stage update we still call the
    // worker, which still fires TESQuestStageEvent locally (and thus our hook
    // re-fires recursively because the hook IS the worker). The flag tells
    // the broadcast path to skip emission so we don't loop {RX → worker →
    // hook → broadcast → peer → RX → ...}. Per-thread atomic - Network and
    // game-thread can both hit the worker.
    static thread_local bool tls_applyingRemoteStage = false;
    static thread_local bool tls_applyingRemoteStop  = false;

    // Dedupe identical Start/Stop broadcasts. Quest start fires from
    // multiple paths (Papyrus, StoryTeller drain, ScriptEvent activation)
    // so the worker can run twice in rapid succession without state
    // change. Map keyed by formID; value 0=stopped, 1=started. Reset()
    // clears so a reconnect re-broadcasts cleanly.
    static std::mutex                                s_lastStartStopMutex;
    static std::unordered_map<std::uint32_t, std::uint8_t> s_lastStartStop;

    // Drop duplicate stage broadcasts. Useful because some quests call
    // SetCurrentStageID on the SAME stage (the engine path is idempotent -
    // worker returns true if scripts ran, and they may run multiple times
    // per stage e.g. across save/load). Keyed by quest formID, valued by
    // last-broadcast stageID. Reset() clears so reconnects don't suppress.
    static std::mutex                                s_lastSeenMutex;
    static std::unordered_map<std::uint32_t, std::uint16_t> s_lastSeenStage;

    // 0.5.0 missed-stage replay: every local stage set, and when the peer
    // last dropped (0 = no gap to replay). NOT cleared by Reset().
    static std::mutex                 s_journalMutex;
    static StageJournal               s_journal;
    static std::atomic<std::uint64_t> s_gapStartMs{ 0 };

    // Raw +0x28 read for TESForm.formID - verified universal offset on
    // 1.16.236 (memory/reference_formid_offset.md). Avoids vt-call on a
    // potentially mid-mutation TESQuest from inside the hook body.
    static std::uint32_t RawFormID(const void* form) noexcept
    {
        if (!form) return 0;
        return *reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(form) + 0x28);
    }

    // Forward declarations - bodies live near the bottom of this namespace
    // (kept down there because they wrap raw worker calls in __try, and
    // C2712 forbids __try anywhere upstream of std::lock_guard scopes).
    static bool          SafeCallOrigSetStage(RE::TESQuest* quest, std::uint16_t stage) noexcept;
    static std::uint32_t SafeRawFormID(const void* form) noexcept;
    static void          SafeCallOrigStart(void* ctx, RE::TESQuest** questPtr, char* outOk) noexcept;
    static void          SafeCallOrigStop(RE::TESQuest* quest, char param_2) noexcept;
    static RE::TESQuest* SafeDerefQuestPtr(RE::TESQuest** pp) noexcept;

    static void BroadcastStage(std::uint32_t questFormID, std::uint16_t stageID)
    {
        // Skip if peer not connected - saves serialization cost and the
        // s_lastSeen book-keeping is irrelevant when nobody listens.
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        // Dedupe identical (formID, stageID) tuple. Worker can be called
        // multiple times per stage during save-load + script re-fires.
        {
            std::lock_guard g(s_lastSeenMutex);
            auto it = s_lastSeenStage.find(questFormID);
            if (it != s_lastSeenStage.end() && it->second == stageID) {
                return;
            }
            s_lastSeenStage[questFormID] = stageID;
        }

        std::uint8_t buf[1 + sizeof(Pulsar::Network::QuestStageData)]{};
        buf[0] = Pulsar::Network::PKT_QUEST_STAGE;
        Pulsar::Network::QuestStageData payload{ questFormID, stageID };
        std::memcpy(buf + 1, &payload, sizeof(payload));

        net.Send(buf, sizeof(buf), /*reliable=*/true);

        PLOG_INFO(
            "[PULSAR] QuestSync TX: quest=0x{:08X} stage={}"sv,
            questFormID, stageID);
    }

    // Hook body - runs in place of the engine's worker. Calls original
    // first so the engine state advances exactly as it would have without
    // the hook, then broadcasts if (a) original succeeded and (b) we're
    // not currently applying a remote update.
    static bool __fastcall HookedSetStage(RE::TESQuest* quest, std::uint16_t stage)
    {
        bool result = SafeCallOrigSetStage(quest, stage);

        if (!result) return result;

        // EventTrace - every local quest-stage change, named, even when solo or
        // when this is a remote-applied set. The single richest signal for
        // quest-progress sync design.
        if (quest) {
            std::uint32_t tfid = SafeRawFormID(quest);
            Pulsar::EventTrace::Trace("QUEST", "stage SET quest=%s stage=%u%s",
                Pulsar::EventTrace::NameOf(tfid).c_str(), stage,
                tls_applyingRemoteStage ? " (remote)" : "");
        }

        if (tls_applyingRemoteStage) return result;
        if (!quest) return result;

        std::uint32_t formID = SafeRawFormID(quest);
        if (!formID) return result;

        {
            std::lock_guard g(s_journalMutex);
            s_journal.Record(formID, stage, GetTickCount64());
        }
        BroadcastStage(formID, stage);
        return result;
    }

    // Phase 2 - broadcast helper for Start/Stop. Dedupes via s_lastStartStop
    // map so the same transition isn't broadcast twice.
    static void BroadcastStartStop(std::uint8_t pktType, std::uint32_t formID,
                                   std::uint16_t flag)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        const std::uint8_t startedVal =
            (pktType == Pulsar::Network::PKT_QUEST_START) ? 1 : 0;
        {
            std::lock_guard g(s_lastStartStopMutex);
            auto it = s_lastStartStop.find(formID);
            if (it != s_lastStartStop.end() && it->second == startedVal) {
                return;
            }
            s_lastStartStop[formID] = startedVal;
        }

        std::uint8_t buf[1 + sizeof(Pulsar::Network::QuestStartStopData)]{};
        buf[0] = pktType;
        Pulsar::Network::QuestStartStopData payload{ formID, flag };
        std::memcpy(buf + 1, &payload, sizeof(payload));
        net.Send(buf, sizeof(buf), /*reliable=*/true);

        PLOG_INFO(
            "[PULSAR] QuestSync TX: quest=0x{:08X} {} flag={}"sv,
            formID,
            startedVal ? "START" : "STOP",
            flag);
    }

    // Phase 2 - TESQuest::Start hook. Calls orig, then broadcasts on TX
    // path. Note: NO echo-suppression flag is used here because we never
    // replay this function on RX (caller-frame ctx is non-portable).
    static void __fastcall HookedStart(void* ctx, RE::TESQuest** questPtr, char* outOk)
    {
        SafeCallOrigStart(ctx, questPtr, outOk);

        RE::TESQuest* quest = SafeDerefQuestPtr(questPtr);
        if (!quest) return;

        std::uint32_t formID = SafeRawFormID(quest);
        if (!formID) return;

        Pulsar::EventTrace::Trace("QUEST", "START quest=%s",
            Pulsar::EventTrace::NameOf(formID).c_str());

        BroadcastStartStop(Pulsar::Network::PKT_QUEST_START, formID, 0);
    }

    // Phase 2 - TESQuest::Stop hook. Calls orig, then broadcasts unless
    // we're currently applying a remote stop (echo suppression). param_2
    // is forwarded as the payload flag so the receiver can distinguish
    // hard-stop (1+) from quiet cleanup (0).
    static void __fastcall HookedStop(RE::TESQuest* quest, char param_2)
    {
        SafeCallOrigStop(quest, param_2);

        if (tls_applyingRemoteStop) return;
        if (!quest) return;

        std::uint32_t formID = SafeRawFormID(quest);
        if (!formID) return;

        Pulsar::EventTrace::Trace("QUEST", "STOP quest=%s flag=%d",
            Pulsar::EventTrace::NameOf(formID).c_str(),
            static_cast<int>(static_cast<std::uint8_t>(param_2)));

        BroadcastStartStop(Pulsar::Network::PKT_QUEST_STOP, formID,
                           static_cast<std::uint16_t>(static_cast<std::uint8_t>(param_2)));
    }

    static std::atomic<bool> s_initDone{ false };

    // SEH-only helper. C2712 forbids __try in functions that contain
    // objects with non-trivial destructors (lock_guard, etc.), so the
    // raw worker call lives here and the public API holds the locks.
    static bool SafeCallOrigSetStage(RE::TESQuest* quest, std::uint16_t stage) noexcept
    {
        if (!g_origSetStage) return false;
        __try {
            return g_origSetStage(quest, stage);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static std::uint32_t SafeRawFormID(const void* form) noexcept
    {
        __try { return RawFormID(form); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static void SafeCallOrigStart(void* ctx, RE::TESQuest** questPtr, char* outOk) noexcept
    {
        if (!g_origStart) return;
        __try { g_origStart(ctx, questPtr, outOk); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void SafeCallOrigStop(RE::TESQuest* quest, char param_2) noexcept
    {
        if (!g_origStop) return;
        __try { g_origStop(quest, param_2); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static RE::TESQuest* SafeDerefQuestPtr(RE::TESQuest** pp) noexcept
    {
        if (!pp) return nullptr;
        __try { return *pp; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

}  // anonymous namespace

void Init()
{
    if (s_initDone.exchange(true)) return;

    // Resolve via raw RVA - no stable REL::ID for FUN_140D0E1F0 known.
    // Pattern matches LootSync M.2 V3 (AddObjInner @ raw 0xB4CBC0).
    std::uintptr_t moduleBase =
        REX::FModule::GetExecutingModule().GetBaseAddress();
    void* fn = reinterpret_cast<void*>(moduleBase + kRVA_SetStageWorker);

    MH_STATUS st = MH_CreateHook(
        fn,
        reinterpret_cast<LPVOID>(&HookedSetStage),
        reinterpret_cast<LPVOID*>(&g_origSetStage));
    if (st != MH_OK) {
        PLOG_WARN(
            "[PULSAR] QuestSync: MH_CreateHook(SetCurrentStageID) failed: {}"sv,
            static_cast<int>(st));
        return;
    }
    st = MH_EnableHook(fn);
    if (st != MH_OK) {
        PLOG_WARN(
            "[PULSAR] QuestSync: MH_EnableHook(SetCurrentStageID) failed: {}"sv,
            static_cast<int>(st));
        return;
    }

    PLOG_INFO(
        "[PULSAR] QuestSync: TESQuest::SetCurrentStageID hook installed at "
        "0x{:x} (RVA 0x{:X})"sv,
        reinterpret_cast<std::uintptr_t>(fn),
        kRVA_SetStageWorker);

    // Phase 2 - Start hook. RVA 0xEEF850.
    {
        void* startFn = reinterpret_cast<void*>(moduleBase + kRVA_StartWorker);
        MH_STATUS sst = MH_CreateHook(
            startFn,
            reinterpret_cast<LPVOID>(&HookedStart),
            reinterpret_cast<LPVOID*>(&g_origStart));
        if (sst != MH_OK) {
            PLOG_WARN(
                "[PULSAR] QuestSync: MH_CreateHook(Start) failed: {}"sv,
                static_cast<int>(sst));
        } else {
            sst = MH_EnableHook(startFn);
            if (sst != MH_OK) {
                PLOG_WARN(
                    "[PULSAR] QuestSync: MH_EnableHook(Start) failed: {}"sv,
                    static_cast<int>(sst));
            } else {
                PLOG_INFO(
                    "[PULSAR] QuestSync: TESQuest::Start hook installed at "
                    "0x{:x} (RVA 0x{:X})"sv,
                    reinterpret_cast<std::uintptr_t>(startFn),
                    kRVA_StartWorker);
            }
        }
    }

    // Phase 2 - Stop hook. RVA 0xCF9970.
    {
        void* stopFn = reinterpret_cast<void*>(moduleBase + kRVA_StopWorker);
        MH_STATUS sst = MH_CreateHook(
            stopFn,
            reinterpret_cast<LPVOID>(&HookedStop),
            reinterpret_cast<LPVOID*>(&g_origStop));
        if (sst != MH_OK) {
            PLOG_WARN(
                "[PULSAR] QuestSync: MH_CreateHook(Stop) failed: {}"sv,
                static_cast<int>(sst));
        } else {
            sst = MH_EnableHook(stopFn);
            if (sst != MH_OK) {
                PLOG_WARN(
                    "[PULSAR] QuestSync: MH_EnableHook(Stop) failed: {}"sv,
                    static_cast<int>(sst));
            } else {
                PLOG_INFO(
                    "[PULSAR] QuestSync: TESQuest::Stop hook installed at "
                    "0x{:x} (RVA 0x{:X})"sv,
                    reinterpret_cast<std::uintptr_t>(stopFn),
                    kRVA_StopWorker);
            }
        }
    }
}

void Shutdown()
{
    // No-op: MinHook persists hooks until plugin teardown. Engine event
    // sources live in static memory; we never tear ours down here.
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;

    if (!data || length < 1) return;
    const std::uint8_t pktType = data[0];

    if (pktType == PKT_QUEST_STAGE || pktType == PKT_QUEST_STAGE_REPLAY) {
        if (length < 1 + sizeof(QuestStageData)) return;

        QuestStageData d{};
        std::memcpy(&d, data + 1, sizeof(d));

        // A replayed stage the client already reached on its own during the gap
        // would re-run that stage's scripts (duplicate rewards): skip it.
        if (pktType == PKT_QUEST_STAGE_REPLAY) {
            const std::uint64_t gap = s_gapStartMs.load(std::memory_order_acquire);
            bool already = false;
            {
                std::lock_guard g(s_journalMutex);
                already = gap != 0 && s_journal.ContainsSince(d.questFormID, d.stageID, gap);
            }
            if (already) {
                PLOG_INFO("[PULSAR] QuestSync RX replay: quest=0x{:08X} stage={} already set here - skip"sv,
                          d.questFormID, d.stageID);
                return;
            }
        }

        PLOG_INFO(
            "[PULSAR] QuestSync RX: quest=0x{:08X} stage={}"sv,
            d.questFormID, d.stageID);

        if (!g_origSetStage) {
            PLOG_WARN(
                "[PULSAR] QuestSync RX: orig worker null (Init didn't run?)"sv);
            return;
        }

        // Resolve TESQuest by formID. LookupByID is safe to call from any
        // thread (read-only hashtable lookup); the apply itself touches
        // mutable engine state but the worker is internally guarded by
        // engine locks.
        auto* form = RE::TESForm::LookupByID(d.questFormID);
        if (!form) {
            PLOG_INFO(
                "[PULSAR] QuestSync RX: quest 0x{:08X} not loaded - drop"sv,
                d.questFormID);
            return;
        }
        // Reject non-quest forms (defensive: cross-machine FormID drift
        // could theoretically resolve to a different type).
        if (form->GetFormType() != RE::FormType::kQUST) {
            PLOG_WARN(
                "[PULSAR] QuestSync RX: 0x{:08X} resolved as type {} not QUST - drop"sv,
                d.questFormID, static_cast<int>(form->GetFormType()));
            return;
        }
        auto* quest = static_cast<RE::TESQuest*>(form);

        // Pre-update s_lastSeenStage so the hook's immediate post-call
        // broadcast path dedupes against this stage even if the
        // tls flag misses any nested re-entry path.
        {
            std::lock_guard g(s_lastSeenMutex);
            s_lastSeenStage[d.questFormID] = d.stageID;
        }

        tls_applyingRemoteStage = true;
        bool ok = SafeCallOrigSetStage(quest, d.stageID);
        tls_applyingRemoteStage = false;

        PLOG_INFO(
            "[PULSAR] QuestSync RX applied: quest=0x{:08X} stage={} ok={}"sv,
            d.questFormID, d.stageID, ok ? 1 : 0);
        return;
    }

    // PKT_QUEST_START - Phase 2. RX intentionally log-only: the engine
    // Start core (FUN_140EEFEE0) takes a caller-frame QuestStartCtx that
    // we can't reconstruct on the peer. Practical observation: most
    // quests start naturally on the receiver via Phase 1 stage sync (the
    // initial-stage scripts run, and the engine treats stage 0/10 as
    // implicit start). If divergence shows up in playtest, Phase 2 v2
    // will add an explicit StoryTeller-queue-based apply path.
    if (pktType == PKT_QUEST_START) {
        if (length < 1 + sizeof(QuestStartStopData)) return;
        QuestStartStopData d{};
        std::memcpy(&d, data + 1, sizeof(d));

        // Pre-update last-seen so the local TX hook (if it later fires
        // for the same start triggered by stage cascade) dedupes.
        {
            std::lock_guard g(s_lastStartStopMutex);
            s_lastStartStop[d.questFormID] = 1;
        }

        PLOG_INFO(
            "[PULSAR] QuestSync RX: quest=0x{:08X} START (peer started - "
            "relying on stage-cascade for local apply)"sv,
            d.questFormID);
        return;
    }

    // PKT_QUEST_STOP - Phase 2. RX replays via TESQuest::Stop core
    // (FUN_140CFA0A0) with clean (TESQuest*, char) signature. Echo
    // suppression flag prevents the orig's StartStopEvent Notify from
    // re-broadcasting back to the peer.
    if (pktType == PKT_QUEST_STOP) {
        if (length < 1 + sizeof(QuestStartStopData)) return;
        QuestStartStopData d{};
        std::memcpy(&d, data + 1, sizeof(d));

        PLOG_INFO(
            "[PULSAR] QuestSync RX: quest=0x{:08X} STOP flag={}"sv,
            d.questFormID, d.stageOrFlag);

        if (!g_origStop) {
            PLOG_WARN("[PULSAR] QuestSync RX: orig Stop null"sv);
            return;
        }

        auto* form = RE::TESForm::LookupByID(d.questFormID);
        if (!form) {
            PLOG_INFO(
                "[PULSAR] QuestSync RX: stop quest 0x{:08X} not loaded - drop"sv,
                d.questFormID);
            return;
        }
        if (form->GetFormType() != RE::FormType::kQUST) {
            PLOG_WARN(
                "[PULSAR] QuestSync RX: 0x{:08X} resolved as type {} not QUST - drop"sv,
                d.questFormID, static_cast<int>(form->GetFormType()));
            return;
        }
        auto* quest = static_cast<RE::TESQuest*>(form);

        {
            std::lock_guard g(s_lastStartStopMutex);
            s_lastStartStop[d.questFormID] = 0;
        }

        const char param2 = static_cast<char>(d.stageOrFlag & 0xFF);
        tls_applyingRemoteStop = true;
        SafeCallOrigStop(quest, param2);
        tls_applyingRemoteStop = false;

        PLOG_INFO(
            "[PULSAR] QuestSync RX applied: quest=0x{:08X} STOP flag={}"sv,
            d.questFormID, static_cast<int>(param2));
        return;
    }
}

void NotePeerDisconnected()
{
    std::uint64_t expected = 0;
    s_gapStartMs.compare_exchange_strong(expected, GetTickCount64(), std::memory_order_acq_rel);
}

void ForgetMissedStages()
{
    s_gapStartMs.store(0, std::memory_order_release);
}

void ReplayMissedStagesToPeer()
{
    using namespace Pulsar::Network;
    const std::uint64_t gap = s_gapStartMs.exchange(0, std::memory_order_acq_rel);
    if (gap == 0) return;
    std::vector<StageEntry> missed;
    {
        std::lock_guard g(s_journalMutex);
        missed = s_journal.Since(gap);
    }
    auto& net = Pulsar::Network::GetSingleton();
    for (const auto& e : missed) {
        std::uint8_t buf[1 + sizeof(QuestStageData)]{};
        buf[0] = PKT_QUEST_STAGE_REPLAY;
        QuestStageData payload{ e.quest, e.stage };
        std::memcpy(buf + 1, &payload, sizeof(payload));
        net.Send(buf, sizeof(buf), /*reliable=*/true);
    }
    PLOG_DIAG("[PULSAR] co-op: replayed {} quest stage(s) set while the partner was disconnected"sv,
              missed.size());
}

void Reset()
{
    {
        std::lock_guard g(s_lastSeenMutex);
        s_lastSeenStage.clear();
    }
    {
        std::lock_guard g(s_lastStartStopMutex);
        s_lastStartStop.clear();
    }
    tls_applyingRemoteStage = false;
    tls_applyingRemoteStop  = false;
    PLOG_INFO("[PULSAR] QuestSync: Reset (last-seen + start/stop caches cleared)"sv);
}

}  // namespace Pulsar::QuestSync
