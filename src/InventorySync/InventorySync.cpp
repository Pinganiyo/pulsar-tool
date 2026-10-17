#include "PCH.h"
#include "InventorySync/InventorySync.h"
#include "InventorySync/DirectRestore.h"

#include "Config.h"
#include "Forms/SaveBuffer.h"
#include "Network/NetworkHost.h"
#include "Profile/ClientProfile.h"
#include "WorldSync/WorldSync.h"

#include <Windows.h>
#include <MinHook.h>
#include <REL/Relocation.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::InventorySync
{
    namespace
    {
        // ─── BGSLoadFormBuffer raw layout (verified RE 2026-05-10) ──────
        //
        // Total size 0xF0 (240) bytes. Vtable at REL::ID 398652 = 0x144B21CE8.
        // Layout decomp'd from FUN_141867710 / FUN_141868700 / FUN_140b34ca0
        // engine load-init sites (see local RE notes §2).
        //
        // CRITICAL: position @ +0x24 (NOT +0x14 like SaveFB),
        //           capacity @ +0x20.
        //
        // Forms::SaveBuffer's existing LoadFormBufferRaw uses Skyrim-derived
        // (incorrect) offsets - its DeserializeForm therefore only triggers
        // a state-zero side effect (vt[0x1B] LoadGame), which happens to be
        // useful for biped re-init but does NOT actually apply the blob.
        //
        // For real player-state restore we own this layout + chain locally.
        struct alignas(8) LoadFB {
            std::uint8_t bytes[0xF0];
        };
        static_assert(sizeof(LoadFB) == 0xF0, "LoadFB must be 240 bytes");

        constexpr std::ptrdiff_t kOff_vtable      = 0x00;
        constexpr std::ptrdiff_t kOff_buffer      = 0x08;
        constexpr std::ptrdiff_t kOff_capacity    = 0x20;
        constexpr std::ptrdiff_t kOff_position    = 0x24;
        constexpr std::ptrdiff_t kOff_sentinel38  = 0x38;
        constexpr std::ptrdiff_t kOff_form        = 0xD8;
        constexpr std::ptrdiff_t kOff_saveVersion = 0xE0;
        constexpr std::ptrdiff_t kOff_sentinelEF  = 0xEF;

        // TESForm vtable slots (from CommonLibSF + verified PC vtable
        // 0x144CBC3E8 / SBR_STAGE4 decomp).
        constexpr int kSlot_LoadGame       = 0x1B;
        constexpr int kSlot_InitLoadGame   = 0x1C;
        constexpr int kSlot_FinishLoadGame = 0x1D;

        // LoadFB vtable slot for the chunk reader (FUN_140904F90).
        // Verified via SBR_DUMP §vt[0x6]/[+0x30] and m_3_wire_format_re.
        constexpr int kLoadFB_Slot_ChunkReader = 0x06;

        // LoadFB inner offsets used by V2 chunk-filter hook.
        // buf+0x174 = current chunk tag (u32 LE).
        // buf+0x178 = current chunk body size (u32).
        // Verified via FUN_140904F90 + FUN_1405B9380 decomp in
        // local RE notes
        constexpr std::ptrdiff_t kOff_curTag  = 0x174;
        constexpr std::ptrdiff_t kOff_curSize = 0x178;

        // Vtable address (resolved at runtime via REL::ID 398652).
        static REL::Relocation<std::uintptr_t> s_loadFBVtable{ REL::ID(398652) };

        // ─── V2 chunk-filter - runtime vtable hook ─────────────────────
        // Allowlist of chunk tags to KEEP when applying a remote player
        // blob. All other tags are silently consumed at the chunk-reader
        // level so PlayerCharacter::FinishLoadGame's switch never sees
        // them (no dispatch → no world-state overlay).
        //
        // Tag values are u32 little-endian = 4 ASCII chars MSB-first read.
        //   "DATA" → 0x41544144 - base REFR pos/rot/scale (L.7 overrides)
        //   "NAME" → 0x454D414E - baseForm pointer (identical between peers)
        //   "EDID" → 0x44494445 - actor editor-ID alias
        //   "BFCB" → 0x42434642 - THE inventory/AV/skills/perks sub-blob
        //
        // Full chunk-tag map + KEEP/DROP rationale lives in
        // memory/reference_m3_chunk_tags.md and local RE notes
        static constexpr std::uint32_t kKeepTags[] = {
            0x41544144u,  // DATA
            0x454D414Eu,  // NAME
            0x44494445u,  // EDID
            0x42434642u,  // BFCB
        };

        static bool IsKeepTag(std::uint32_t a_tag) noexcept
        {
            for (auto t : kKeepTags) {
                if (t == a_tag) return true;
            }
            return false;
        }

        // Custom LoadFB vtable. Bytewise copy of the engine vtable with
        // slot 0x06 (chunk reader) swapped for our filtering hook. All
        // other slots dispatch to the original engine implementations.
        constexpr std::size_t kCustomVtSlotCount = 256;
        alignas(16) static void* s_customVt[kCustomVtSlotCount]{};
        static std::atomic<bool> s_customVtReady{ false };
        static void* s_origChunkReader = nullptr;

        // L.7 V2 - global engine-vtable patch state. When true, the
        // chunk reader hook runs on the engine's own LoadFB instances too
        // (not just our M.3 apply buf). Used to skip player BFCB chunks
        // during bridge load so live player keeps pre-bridge state.
        static std::atomic<bool> s_engineVtPatched{ false };

        // Declared here (instead of in the State block below) so the
        // ChunkReaderHook above can read the bridge-pending flag without a
        // forward-declaration dance. Same anonymous-namespace internal
        // linkage as the rest of the state block.
        std::atomic<bool> s_preBridgeSnapshotPending{ false };

        // Thread-local flag set TRUE during our M.3 ApplyBlobToForm call.
        // Distinguishes "PULSAR apply chain context" (KEEP allowlist for
        // client snapshot) from "engine bridge load context" (drop player
        // chunks). Thread-local because engine load runs on BSService
        // worker thread while our apply runs on game-main.
        static thread_local bool s_inApplyChain = false;

        // L.7 V3.3 - per-form dispatcher hook state. The engine's
        // bridge-load LoadFB instances do NOT put the form pointer at
        // buf+0xD8 (we set that field in our M.3 ApplyBlobToForm but the
        // engine doesn't - confirmed by V3.2 diagnostic showing playerHits=0
        // even when engineCalls=47). Reliable identification of "currently
        // dispatched form is the player" comes from the per-form dispatcher
        // FUN_14181E030, which receives the recordHeader with formID at
        // offset 0x40 (RE: local RE notes).
        //
        // We MinHook FUN_14181E030, write the formID into this thread_local
        // before calling orig, restore after. ChunkReaderHook then reads
        // this flag instead of buf+0xD8.
        static thread_local std::uint32_t s_engineDispatchFormID = 0;

        // Diagnostic budget for first-N dispatch logs per bridge cycle.
        static std::atomic<int> s_diag_dispatchLogBudget{ 0 };

        // ─── L.7 V5.1 - Memory-mutation watchpoint ───────────────────
        // V5 used a narrow region (0x100..0x600) and saw zero engine
        // mutations. Widened to 0x00..0x1000 (4KB) to catch position writes
        // (+0x80..0x94), TESForm flags (+0x20), and any high-offset state.
        // Indirect mutations (via heap pointers like ExtraContainerChanges)
        // are STILL invisible - the pointer doesn't change, only the
        // pointed-to data does. For those we'd need to walk through the
        // pointer (TODO if V5.1 still shows no diff).
        static std::uint64_t HashPlayerRegion() noexcept
        {
            std::uint64_t h = 1469598103934665603ULL;
            __try {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) return 0;
                auto* b = reinterpret_cast<const std::uint8_t*>(player);
                for (std::size_t i = 0; i < 0x1000; ++i) {
                    h ^= b[i];
                    h *= 1099511628211ULL;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
            return h;
        }

        // Pre-bridge / post-bridge whole-player snapshot. Compared in
        // DoApply to log how many bytes the engine actually wrote to the
        // player struct (regardless of which function wrapped it).
        static std::array<std::uint8_t, 0x1000> s_diag_playerSnapAtArm{};
        static std::atomic<bool>                s_diag_playerSnapValid{ false };

        static void CapturePlayerSnap() noexcept
        {
            __try {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    s_diag_playerSnapValid.store(false, std::memory_order_release);
                    return;
                }
                std::memcpy(s_diag_playerSnapAtArm.data(), player, 0x1000);
                s_diag_playerSnapValid.store(true, std::memory_order_release);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                s_diag_playerSnapValid.store(false, std::memory_order_release);
            }
        }

        // Returns the byte-offset of the FIRST mismatch between current
        // player state and the snapshot, plus a count of differing 16-byte
        // rows. Reports max 8 differing offsets via callback.
        static std::uint32_t ComparePlayerSnap(std::uint64_t* a_diffMask) noexcept
        {
            *a_diffMask = 0;
            if (!s_diag_playerSnapValid.load(std::memory_order_acquire)) return 0xFFFFFFFFu;
            std::uint32_t diffCount = 0;
            __try {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) return 0xFFFFFFFFu;
                auto* live = reinterpret_cast<const std::uint8_t*>(player);
                auto* snap = s_diag_playerSnapAtArm.data();
                // Track which 64-byte rows differ (16 rows = 1KB span).
                // Each bit in diffMask represents one 256-byte chunk of the
                // 0x1000-byte player snapshot.
                std::uint64_t mask = 0;
                for (std::size_t i = 0; i < 0x1000; ++i) {
                    if (live[i] != snap[i]) {
                        ++diffCount;
                        const auto bit = i / 64;  // 64 64-byte rows = 0x1000
                        mask |= (1ULL << bit);
                    }
                }
                *a_diffMask = mask;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0xFFFFFFFFu;
            }
            return diffCount;
        }

        // Per-bridge mutation counters. Reset in ArmForBridge, dumped in
        // DoApply. Each tracks how many times that hook's orig wrapped a
        // detected player-state mutation.
        static std::atomic<std::uint32_t> s_diag_mut_chunkReader{ 0 };
        static std::atomic<std::uint32_t> s_diag_mut_dispatcher{ 0 };
        static std::atomic<std::uint32_t> s_diag_mut_bfcb{ 0 };
        static std::atomic<std::uint32_t> s_diag_mut_pcFinish{ 0 };
        // Log budget for per-mutation details (first N per bridge).
        static std::atomic<int> s_diag_mutLogBudget{ 0 };

        // The hook: behavior depends on calling context.
        //   (a) PULSAR apply chain - s_inApplyChain=true
        //       Original KEEP-allowlist: drop non-KEEP chunks (world-state),
        //       pass KEEP chunks (DATA/NAME/EDID/BFCB) to dispatcher.
        //   (b) Engine bridge load on player - s_inApplyChain=false AND
        //       buf.form==player AND IsPreBridgeSnapshotPending()
        //       Drop ONLY BFCB (inventory+AVs+skills+perks). Let DATA/NAME/
        //       EDID/everything-else pass through so engine cell-streaming
        //       + position apply work normally and per-form invariants stay
        //       intact. Live player keeps pre-bridge inventory/skills.
        //       V2 attempted drop-ALL (return 0 immediately) - that part
        //       alone didn't crash (1st bridge logged successful diagnostic
        //       hash). The actual V2 crash was infinite recursion in the
        //       passthrough call below, caused by GetCustomVtable corrupting
        //       s_origChunkReader; fixed in GetCustomVtable() (guard write).
        //   (c) Anything else (non-player forms during bridge, non-bridge
        //       loads, main-menu loads, autosave recovery, ...) -
        //       passthrough. Calls original engine chunk reader unchanged.
        using ChunkReaderFn = std::uint64_t(__fastcall*)(void* /*buf*/, void* /*param2*/);

        // L.7 V3.2 - diagnostic counters. Engine-context only (not
        // apply-chain). Reset on each ArmForBridge so each bridge cycle gets
        // a clean tally; dump in DoApply after the .psprof apply runs.
        static std::atomic<std::uint32_t> s_diag_engineCalls{ 0 };
        static std::atomic<std::uint32_t> s_diag_playerHits{ 0 };
        static std::atomic<std::uint32_t> s_diag_bfcbDrops{ 0 };
        static std::atomic<std::uint32_t> s_diag_passthroughs{ 0 };
        static std::atomic<std::uint32_t> s_diag_nonPlayerCalls{ 0 };
        static std::atomic<std::uint32_t> s_diag_pendingMissedPlayer{ 0 };

        static std::uint64_t __fastcall ChunkReaderHook(void* a_buf, void* a_p2) noexcept
        {
            auto orig = reinterpret_cast<ChunkReaderFn>(s_origChunkReader);
            if (!orig || !a_buf) return 0;

            __try {
                auto* base = reinterpret_cast<std::uint8_t*>(a_buf);

                // Context (b) - engine bridge load on player change-form.
                // Drop ONLY BFCB chunks: loop until orig() returns a chunk
                // whose tag is NOT BFCB (or EOF), pass that one through.
                // Player's per-form chunk loop sees DATA/NAME/EDID/etc.
                // normally but never BFCB → no inventory/AV/skill overlay.
                if (!s_inApplyChain) {
                    s_diag_engineCalls.fetch_add(1, std::memory_order_relaxed);
                    // V3.3: use dispatcher-set thread_local instead of
                    // buf+0xD8 (which is only valid in our apply chain).
                    const bool isPlayer = (s_engineDispatchFormID == 0x14u);
                    const bool pending =
                        s_preBridgeSnapshotPending.load(
                            std::memory_order_acquire);
                    if (!isPlayer) {
                        s_diag_nonPlayerCalls.fetch_add(1, std::memory_order_relaxed);
                    } else if (!pending) {
                        s_diag_pendingMissedPlayer.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (isPlayer && pending)
                    {
                        s_diag_playerHits.fetch_add(1, std::memory_order_relaxed);
                        constexpr std::uint32_t kBFCB = 0x42434642u;
                        int dropCount = 0;
                        while (true) {
                            const auto ok = orig(a_buf, a_p2);
                            if (!ok) return ok;  // EOF / error
                            const auto tag =
                                *reinterpret_cast<std::uint32_t*>(
                                    base + kOff_curTag);
                            if (tag != kBFCB) {
                                s_diag_passthroughs.fetch_add(1,
                                    std::memory_order_relaxed);
                                return ok;  // pass non-BFCB chunk to caller
                            }
                            s_diag_bfcbDrops.fetch_add(1,
                                std::memory_order_relaxed);
                            // BFCB - advance position past body, clear
                            // tag/size, loop to read next chunk.
                            const auto sz =
                                *reinterpret_cast<std::uint32_t*>(
                                    base + kOff_curSize);
                            const auto pos =
                                *reinterpret_cast<std::uint32_t*>(
                                    base + kOff_position);
                            const auto cap =
                                *reinterpret_cast<std::uint32_t*>(
                                    base + kOff_capacity);
                            if (static_cast<std::uint64_t>(pos) +
                                    static_cast<std::uint64_t>(sz) > cap) {
                                // Truncated → treat as EOF.
                                *reinterpret_cast<std::uint32_t*>(
                                    base + kOff_curTag)  = 0;
                                *reinterpret_cast<std::uint32_t*>(
                                    base + kOff_curSize) = 0;
                                return 0;
                            }
                            *reinterpret_cast<std::uint32_t*>(
                                base + kOff_position) = pos + sz;
                            *reinterpret_cast<std::uint32_t*>(
                                base + kOff_curTag)  = 0;
                            *reinterpret_cast<std::uint32_t*>(
                                base + kOff_curSize) = 0;
                            // Sanity bound - only 1 BFCB expected on player;
                            // 16 leaves room without risking infinite loop.
                            if (++dropCount > 16) return 0;
                        }
                    }
                    // Context (c) - non-player or non-bridge load. Pass
                    // through unchanged so normal loads (main-menu, autosave,
                    // other forms during bridge) behave exactly as before.
                    const std::uint64_t hashBefore =
                        pending ? HashPlayerRegion() : 0;
                    const auto ret = orig(a_buf, a_p2);
                    if (pending) {
                        const std::uint64_t hashAfter = HashPlayerRegion();
                        if (hashAfter != hashBefore) {
                            s_diag_mut_chunkReader.fetch_add(1,
                                std::memory_order_relaxed);
                            if (s_diag_mutLogBudget.fetch_sub(1,
                                    std::memory_order_relaxed) > 0)
                            {
                                const auto curTag =
                                    *reinterpret_cast<std::uint32_t*>(
                                        base + kOff_curTag);
                                PLOG_INFO(
                                    "[PULSAR] [L.7 V5-mut] chunkReader MUTATED player! "
                                    "dispatchFormID=0x{:08X} tagAfter=0x{:08X} "
                                    "before=0x{:016X} after=0x{:016X}"sv,
                                    s_engineDispatchFormID, curTag,
                                    hashBefore, hashAfter);
                            }
                        }
                    }
                    return ret;
                }

                // Context (a) - PULSAR M.3 apply chain. KEEP-allowlist:
                // drop non-KEEP chunks (world-state from client snapshot),
                // pass KEEP chunks to PlayerCharacter::FinishLoadGame
                // dispatch.
                int dropCount = 0;

                while (true) {
                    const auto ok = orig(a_buf, a_p2);
                    if (!ok) return ok;  // EOF / error

                    const auto tag =
                        *reinterpret_cast<std::uint32_t*>(base + kOff_curTag);
                    if (IsKeepTag(tag)) {
                        return ok;
                    }

                    // DROP - advance cursor past body manually, clear pending
                    // state, loop to read next chunk header.
                    const auto sz =
                        *reinterpret_cast<std::uint32_t*>(base + kOff_curSize);
                    const auto pos =
                        *reinterpret_cast<std::uint32_t*>(base + kOff_position);
                    const auto cap =
                        *reinterpret_cast<std::uint32_t*>(base + kOff_capacity);

                    // Safety: don't run past buffer end.
                    if (static_cast<std::uint64_t>(pos) +
                            static_cast<std::uint64_t>(sz) > cap) {
                        // Truncated chunk → treat as EOF.
                        *reinterpret_cast<std::uint32_t*>(base + kOff_curTag)  = 0;
                        *reinterpret_cast<std::uint32_t*>(base + kOff_curSize) = 0;
                        return 0;
                    }

                    *reinterpret_cast<std::uint32_t*>(base + kOff_position) =
                        pos + sz;
                    *reinterpret_cast<std::uint32_t*>(base + kOff_curTag)  = 0;
                    *reinterpret_cast<std::uint32_t*>(base + kOff_curSize) = 0;
                    if (++dropCount > 4096) {
                        // Sanity bound - should never hit; bail to avoid
                        // infinite loop on pathological blob.
                        return 0;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // L.7 V4.5 - PlayerCharacter::FinishLoadGame direct hook. RVA
        // CORRECTED from 0xB230A0 to 0x1A4B590 based on V4.4 runtime
        // verification: `PlayerCharacter::GetSingleton()->vt[0x1D]` resolved
        // to image-base + 0x1A4B590 in 1.16.236, NOT 0xB230A0. The
        // M3_ChunkSwitch.txt decompile label "PlayerCharacter::FinishLoadGame
        // @ 0x140B230A0" was incorrect for this build - 0xB230A0 is some
        // other Actor/ExtraData chunk processor (V4.x diagnostics showed
        // 14431 calls with formID=0 and zero singleton matches).
        //
        // The engine bridge load applies host's BFCB chunk to the live
        // player by calling this function with the parsed BGSLoadFormBuffer;
        // it mutates inventory + AVs + skills + perks + factions. We must
        // skip-call when `s_preBridgeSnapshotPending && !s_inApplyChain`
        // (engine bridge context) so the live player retains its pre-bridge
        // state.
        //
        // Signature: bool fn(PlayerCharacter* a_player, BGSLoadFormBuffer* a_buf)
        static REL::Relocation<std::uintptr_t> s_pcFinishLoadGame{
            REL::Offset{ 0x1A4B590 } };

        using PcFinishLoadGameFn = bool(__fastcall*)(void*, void*);
        static PcFinishLoadGameFn s_origPcFinishLoadGame = nullptr;
        static std::atomic<bool> s_pcFinishHooked{ false };
        static std::atomic<std::uint32_t> s_diag_finishCalls{ 0 };
        static std::atomic<std::uint32_t> s_diag_finishPlayerCalls{ 0 };
        static std::atomic<std::uint32_t> s_diag_finishSkipped{ 0 };
        static std::atomic<int>           s_diag_finishLogBudget{ 0 };

        static bool __fastcall PcFinishLoadGameDetour(void* a_form, void* a_buf) noexcept
        {
            s_diag_finishCalls.fetch_add(1, std::memory_order_relaxed);

            // V4.3 - V4.2 diagnostic showed formID at +0x28 is consistently 0
            // across 9009 calls. The function is recursively invoked by
            // FUN_1405E14D0 (BFCB sub-blob loader) with temporary structs that
            // have no formID at the expected offset. Pointer-comparison
            // against the live PlayerCharacter singleton is robust to that
            // and unambiguously identifies the OUTER engine call for the
            // player.
            std::uint32_t formID = 0;
            __try {
                if (a_form) {
                    formID = *reinterpret_cast<const std::uint32_t*>(
                        static_cast<const std::uint8_t*>(a_form) + 0x28);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                formID = 0;
            }

            void* livePC = nullptr;
            __try {
                livePC = static_cast<void*>(RE::PlayerCharacter::GetSingleton());
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                livePC = nullptr;
            }

            const bool isPlayerByPtr = (livePC != nullptr && a_form == livePC);
            const bool isPlayerByFid = (formID == 0x14u);
            const bool isPlayer = isPlayerByPtr || isPlayerByFid;
            const bool pending =
                s_preBridgeSnapshotPending.load(std::memory_order_acquire);

            // V4.3 diagnostic: ALWAYS log on player match (no budget), and
            // log first 20 non-matching calls so we can see what else flows
            // through.
            if (pending && (isPlayer ||
                    s_diag_finishLogBudget.fetch_sub(1,
                        std::memory_order_relaxed) > 0))
            {
                PLOG_INFO(
                    "[PULSAR] [L.7 V4.3-diag] PC::FLG param_1=0x{:016X} "
                    "singleton=0x{:016X} formID=0x{:08X} ptrMatch={} fidMatch={}"sv,
                    reinterpret_cast<std::uintptr_t>(a_form),
                    reinterpret_cast<std::uintptr_t>(livePC),
                    formID,
                    isPlayerByPtr ? 1 : 0,
                    isPlayerByFid ? 1 : 0);
            }

            if (isPlayer) {
                s_diag_finishPlayerCalls.fetch_add(1, std::memory_order_relaxed);
                if (pending && !s_inApplyChain) {
                    s_diag_finishSkipped.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }

            if (!s_origPcFinishLoadGame) return true;
            const std::uint64_t hashBefore =
                pending ? HashPlayerRegion() : 0;
            bool ret = true;
            __try {
                ret = s_origPcFinishLoadGame(a_form, a_buf);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ret = false;
            }
            if (pending) {
                const std::uint64_t hashAfter = HashPlayerRegion();
                if (hashAfter != hashBefore) {
                    s_diag_mut_pcFinish.fetch_add(1, std::memory_order_relaxed);
                    if (s_diag_mutLogBudget.fetch_sub(1,
                            std::memory_order_relaxed) > 0)
                    {
                        PLOG_INFO(
                            "[PULSAR] [L.7 V5-mut] PC::FLG MUTATED player! "
                            "formID=0x{:08X} before=0x{:016X} after=0x{:016X}"sv,
                            formID, hashBefore, hashAfter);
                    }
                }
            }
            return ret;
        }

        // L.7 V6 - PerformGameReset + Player REFR re-init hooks.
        // V5.1 evidence: 300-462 bytes mutated in player struct during
        // bridge, but ZERO mutations wrapped by our 4 existing hooks. The
        // engine applies host player state via a code path outside chunk
        // reader / dispatcher / BFCB inner / PC::FLG. RE doc candidates:
        //   FUN_141898AB0 (PerformGameReset, body=6749, RVA 0x1896280)
        //   FUN_141996BB0 (player REFR re-init, RVA 0x19942C0, takes player as 1st arg)
        //
        // Decompile RVAs may be wrong in this build (PC::FLG burned us
        // before - was 0xB230A0 in doc, actually 0x1A4B590 at runtime).
        // For now hook by RVA + log entry to verify they actually fire.
        // If they fire AND wrap the mutation, we have our skip target.
        static REL::Relocation<std::uintptr_t> s_performGameReset{
            REL::Offset{ 0x1896280 } };
        static REL::Relocation<std::uintptr_t> s_playerRefrReInit{
            REL::Offset{ 0x19942C0 } };

        // FUN_141898AB0 - body=6749. Signature unknown beyond "takes some
        // mgr/ctx args". Wrap as 4-arg fastcall for safety.
        using PerformGameResetFn = std::uint64_t(__fastcall*)(
            void*, void*, void*, void*);
        static PerformGameResetFn s_origPerformGameReset = nullptr;
        static std::atomic<bool> s_pgrHooked{ false };
        static std::atomic<std::uint32_t> s_diag_pgrCalls{ 0 };
        static std::atomic<std::uint32_t> s_diag_pgrMut{ 0 };
        static std::atomic<int>           s_diag_pgrLogBudget{ 0 };

        static std::uint64_t __fastcall PerformGameResetDetour(
            void* a_p1, void* a_p2, void* a_p3, void* a_p4) noexcept
        {
            s_diag_pgrCalls.fetch_add(1, std::memory_order_relaxed);
            const bool pending =
                s_preBridgeSnapshotPending.load(std::memory_order_acquire);

            if (s_diag_pgrLogBudget.fetch_sub(1,
                    std::memory_order_relaxed) > 0)
            {
                PLOG_INFO(
                    "[PULSAR] [L.7 V6-pgr] FUN_141898AB0 entry pending={} p1=0x{:016X}"sv,
                    pending ? 1 : 0,
                    reinterpret_cast<std::uintptr_t>(a_p1));
            }

            const std::uint64_t hashBefore =
                pending ? HashPlayerRegion() : 0;
            std::uint64_t ret = 0;
            if (s_origPerformGameReset) {
                __try {
                    ret = s_origPerformGameReset(a_p1, a_p2, a_p3, a_p4);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    ret = 0;
                }
            }
            if (pending) {
                const std::uint64_t hashAfter = HashPlayerRegion();
                if (hashAfter != hashBefore) {
                    s_diag_pgrMut.fetch_add(1, std::memory_order_relaxed);
                    PLOG_INFO(
                        "[PULSAR] [L.7 V6-pgr] PerformGameReset MUTATED player! "
                        "before=0x{:016X} after=0x{:016X}"sv,
                        hashBefore, hashAfter);
                }
            }
            return ret;
        }

        static bool HookPerformGameReset() noexcept
        {
            if (s_pgrHooked.load(std::memory_order_acquire)) return true;
            __try {
                MH_Initialize();
                void* target =
                    reinterpret_cast<void*>(s_performGameReset.address());
                if (!target) return false;
                MH_STATUS st = MH_CreateHook(target,
                    reinterpret_cast<LPVOID>(&PerformGameResetDetour),
                    reinterpret_cast<LPVOID*>(&s_origPerformGameReset));
                if (st != MH_OK) return false;
                st = MH_EnableHook(target);
                if (st != MH_OK) return false;
                s_pgrHooked.store(true, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] [L.7 V6-pgr] PerformGameReset hook installed at 0x{:x}"sv,
                    reinterpret_cast<uintptr_t>(target));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // FUN_141996BB0(player, 1, 0, 0) - player REFR re-init.
        using PlayerRefrReInitFn = std::uint64_t(__fastcall*)(
            void*, std::uint64_t, std::uint64_t, std::uint64_t);
        static PlayerRefrReInitFn s_origPlayerRefrReInit = nullptr;
        static std::atomic<bool> s_prriHooked{ false };
        static std::atomic<std::uint32_t> s_diag_prriCalls{ 0 };
        static std::atomic<std::uint32_t> s_diag_prriPlayerCalls{ 0 };
        static std::atomic<std::uint32_t> s_diag_prriMut{ 0 };
        static std::atomic<int>           s_diag_prriLogBudget{ 0 };

        static std::uint64_t __fastcall PlayerRefrReInitDetour(
            void* a_form, std::uint64_t a_p2,
            std::uint64_t a_p3, std::uint64_t a_p4) noexcept
        {
            s_diag_prriCalls.fetch_add(1, std::memory_order_relaxed);

            void* livePC = nullptr;
            __try {
                livePC = static_cast<void*>(RE::PlayerCharacter::GetSingleton());
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                livePC = nullptr;
            }
            const bool isPlayer = (livePC != nullptr && a_form == livePC);
            const bool pending =
                s_preBridgeSnapshotPending.load(std::memory_order_acquire);

            if (isPlayer) {
                s_diag_prriPlayerCalls.fetch_add(1,
                    std::memory_order_relaxed);
            }

            if (s_diag_prriLogBudget.fetch_sub(1,
                    std::memory_order_relaxed) > 0)
            {
                PLOG_INFO(
                    "[PULSAR] [L.7 V6-prri] FUN_141996BB0 entry isPlayer={} pending={} "
                    "form=0x{:016X} singleton=0x{:016X}"sv,
                    isPlayer ? 1 : 0,
                    pending ? 1 : 0,
                    reinterpret_cast<std::uintptr_t>(a_form),
                    reinterpret_cast<std::uintptr_t>(livePC));
            }

            const std::uint64_t hashBefore =
                pending ? HashPlayerRegion() : 0;
            std::uint64_t ret = 0;
            if (s_origPlayerRefrReInit) {
                __try {
                    ret = s_origPlayerRefrReInit(a_form, a_p2, a_p3, a_p4);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    ret = 0;
                }
            }
            if (pending) {
                const std::uint64_t hashAfter = HashPlayerRegion();
                if (hashAfter != hashBefore) {
                    s_diag_prriMut.fetch_add(1, std::memory_order_relaxed);
                    PLOG_INFO(
                        "[PULSAR] [L.7 V6-prri] PlayerRefrReInit MUTATED player! "
                        "isPlayer={} before=0x{:016X} after=0x{:016X}"sv,
                        isPlayer ? 1 : 0, hashBefore, hashAfter);
                }
            }
            return ret;
        }

        static bool HookPlayerRefrReInit() noexcept
        {
            if (s_prriHooked.load(std::memory_order_acquire)) return true;
            __try {
                MH_Initialize();
                void* target =
                    reinterpret_cast<void*>(s_playerRefrReInit.address());
                if (!target) return false;
                MH_STATUS st = MH_CreateHook(target,
                    reinterpret_cast<LPVOID>(&PlayerRefrReInitDetour),
                    reinterpret_cast<LPVOID*>(&s_origPlayerRefrReInit));
                if (st != MH_OK) return false;
                st = MH_EnableHook(target);
                if (st != MH_OK) return false;
                s_prriHooked.store(true, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] [L.7 V6-prri] PlayerRefrReInit hook installed at 0x{:x}"sv,
                    reinterpret_cast<uintptr_t>(target));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // L.7 V4.4 - BFCB inner-loader hook. FUN_1405E14D0 is what
        // `0x140B230A0` calls when it encounters the BFCB tag (0x42434642)
        // in a parent chunk-switch, but the engine bridge load appears to
        // call FUN_1405E14D0 DIRECTLY on the live player (skipping the
        // outer FinishLoadGame chunk switch - which is what V4.3 evidence
        // suggests: 9009 calls into 0x140B230A0, none with live player).
        // Hook here to catch the actual inventory/AV/skills/perks apply.
        //
        // Signature per m3_chunks_re.txt:1462:
        //   void FUN_1405e14d0(longlong param_1, longlong *param_2,
        //                       undefined8 param_3, int param_4)
        // param_1 = form pointer (player when applying to live player)
        // param_2 = BGSLoadFormBuffer*
        static REL::Relocation<std::uintptr_t> s_bfcbInnerLoader{
            REL::Offset{ 0x5E0930 } };

        using BfcbInnerFn = void(__fastcall*)(std::uintptr_t, void*, std::uint64_t, std::int32_t);
        static BfcbInnerFn s_origBfcbInner = nullptr;
        static std::atomic<bool> s_bfcbInnerHooked{ false };
        static std::atomic<std::uint32_t> s_diag_bfcbCalls{ 0 };
        static std::atomic<std::uint32_t> s_diag_bfcbPlayerCalls{ 0 };
        static std::atomic<std::uint32_t> s_diag_bfcbSkipped{ 0 };
        static std::atomic<int>           s_diag_bfcbLogBudget{ 0 };

        static void __fastcall BfcbInnerDetour(
            std::uintptr_t a_form, void* a_buf,
            std::uint64_t a_p3, std::int32_t a_p4) noexcept
        {
            s_diag_bfcbCalls.fetch_add(1, std::memory_order_relaxed);

            void* livePC = nullptr;
            __try {
                livePC = static_cast<void*>(RE::PlayerCharacter::GetSingleton());
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                livePC = nullptr;
            }

            const bool isPlayerByPtr =
                (livePC != nullptr &&
                 a_form == reinterpret_cast<std::uintptr_t>(livePC));
            const bool pending =
                s_preBridgeSnapshotPending.load(std::memory_order_acquire);

            if (pending && (isPlayerByPtr ||
                    s_diag_bfcbLogBudget.fetch_sub(1,
                        std::memory_order_relaxed) > 0))
            {
                PLOG_INFO(
                    "[PULSAR] [L.7 V4.4-bfcb] FUN_1405E14D0 param_1=0x{:016X} "
                    "singleton=0x{:016X} ptrMatch={}"sv,
                    a_form,
                    reinterpret_cast<std::uintptr_t>(livePC),
                    isPlayerByPtr ? 1 : 0);
            }

            if (isPlayerByPtr) {
                s_diag_bfcbPlayerCalls.fetch_add(1, std::memory_order_relaxed);
                if (pending && !s_inApplyChain) {
                    s_diag_bfcbSkipped.fetch_add(1, std::memory_order_relaxed);
                    PLOG_INFO(
                        "[PULSAR] [L.7 V4.4-bfcb] BFCB inner-loader intercepted "
                        "on PLAYER - skip apply, retain client state"sv);
                    return;  // void return - just don't forward
                }
            }

            if (!s_origBfcbInner) return;
            const std::uint64_t hashBefore =
                pending ? HashPlayerRegion() : 0;
            __try {
                s_origBfcbInner(a_form, a_buf, a_p3, a_p4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // swallow - engine code threw inside orig; not our fault.
            }
            if (pending) {
                const std::uint64_t hashAfter = HashPlayerRegion();
                if (hashAfter != hashBefore) {
                    s_diag_mut_bfcb.fetch_add(1, std::memory_order_relaxed);
                    if (s_diag_mutLogBudget.fetch_sub(1,
                            std::memory_order_relaxed) > 0)
                    {
                        PLOG_INFO(
                            "[PULSAR] [L.7 V5-mut] BFCB inner MUTATED player! "
                            "param_1=0x{:016X} before=0x{:016X} after=0x{:016X}"sv,
                            a_form, hashBefore, hashAfter);
                    }
                }
            }
        }

        static bool HookBfcbInnerLoader() noexcept
        {
            if (s_bfcbInnerHooked.load(std::memory_order_acquire)) return true;
            __try {
                MH_Initialize();
                void* target =
                    reinterpret_cast<void*>(s_bfcbInnerLoader.address());
                if (!target) return false;

                MH_STATUS st = MH_CreateHook(
                    target,
                    reinterpret_cast<LPVOID>(&BfcbInnerDetour),
                    reinterpret_cast<LPVOID*>(&s_origBfcbInner));
                if (st != MH_OK) {
                    PLOG_WARN(
                        "[PULSAR] [L.7 V4.4] MH_CreateHook(FUN_1405E14D0) failed: {}"sv,
                        static_cast<int>(st));
                    return false;
                }
                st = MH_EnableHook(target);
                if (st != MH_OK) {
                    PLOG_WARN(
                        "[PULSAR] [L.7 V4.4] MH_EnableHook(FUN_1405E14D0) failed: {}"sv,
                        static_cast<int>(st));
                    return false;
                }
                s_bfcbInnerHooked.store(true, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] [L.7 V4.4] BFCB inner-loader hook installed at 0x{:x}"sv,
                    reinterpret_cast<uintptr_t>(target));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        static bool HookPcFinishLoadGame() noexcept
        {
            if (s_pcFinishHooked.load(std::memory_order_acquire)) return true;
            __try {
                MH_Initialize();

                void* target =
                    reinterpret_cast<void*>(s_pcFinishLoadGame.address());
                if (!target) return false;

                MH_STATUS st = MH_CreateHook(
                    target,
                    reinterpret_cast<LPVOID>(&PcFinishLoadGameDetour),
                    reinterpret_cast<LPVOID*>(&s_origPcFinishLoadGame));
                if (st != MH_OK) {
                    PLOG_WARN(
                        "[PULSAR] [L.7 V4] MH_CreateHook(PC::FinishLoadGame) failed: {}"sv,
                        static_cast<int>(st));
                    return false;
                }
                st = MH_EnableHook(target);
                if (st != MH_OK) {
                    PLOG_WARN(
                        "[PULSAR] [L.7 V4] MH_EnableHook(PC::FinishLoadGame) failed: {}"sv,
                        static_cast<int>(st));
                    return false;
                }
                s_pcFinishHooked.store(true, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] [L.7 V4] PlayerCharacter::FinishLoadGame hook installed at 0x{:x}"sv,
                    reinterpret_cast<uintptr_t>(target));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // L.7 V3.3 - per-form dispatcher hook. FUN_14181E030 receives
        // recordHeader as its 3rd parameter; formID lives at offset 0x40.
        // RE evidence: local RE notes line 516:
        //   `uVar26 = *(undefined4 *)(param_3 + 0x40);`
        // Then `uVar26` is passed to FUN_1405DEF30 (LookupByID) and
        // FUN_141835670 (type classifier) inside the dispatcher.
        //
        // FUN_14181E030 RVA = 0x14181B8E0 → image-base relative 0x181B8E0.
        static REL::Relocation<std::uintptr_t> s_perFormDispatcher{
            REL::Offset{ 0x181B8E0 } };

        using DispatchFn = std::uint64_t(__fastcall*)(void*, void*, void*, void*);
        static DispatchFn s_origDispatcher = nullptr;
        static std::atomic<bool> s_dispatcherHooked{ false };

        static std::uint64_t __fastcall PerFormDispatcherDetour(
            void* a_loadCtx, void* a_mgr,
            void* a_recordHeader, void* a_loadBuf) noexcept
        {
            std::uint32_t formID = 0;
            __try {
                if (a_recordHeader) {
                    formID = *reinterpret_cast<const std::uint32_t*>(
                        static_cast<const std::uint8_t*>(a_recordHeader) + 0x40);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                formID = 0;
            }

            // Save/restore for nested dispatch (defensive - engine should
            // not re-enter on the same thread, but cheap and safe).
            const std::uint32_t prevFormID = s_engineDispatchFormID;
            s_engineDispatchFormID = formID;

            if (s_diag_dispatchLogBudget.fetch_sub(1,
                    std::memory_order_relaxed) > 0)
            {
                PLOG_INFO(
                    "[PULSAR] [L.7 V3.3-diag] dispatcher: formID=0x{:08X}"sv,
                    formID);
            }

            const bool pending =
                s_preBridgeSnapshotPending.load(std::memory_order_acquire);
            const std::uint64_t hashBefore =
                pending ? HashPlayerRegion() : 0;

            std::uint64_t ret = 0;
            if (s_origDispatcher) {
                __try {
                    ret = s_origDispatcher(a_loadCtx, a_mgr,
                                            a_recordHeader, a_loadBuf);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    ret = 0;
                }
            }

            if (pending) {
                const std::uint64_t hashAfter = HashPlayerRegion();
                if (hashAfter != hashBefore) {
                    s_diag_mut_dispatcher.fetch_add(1,
                        std::memory_order_relaxed);
                    if (s_diag_mutLogBudget.fetch_sub(1,
                            std::memory_order_relaxed) > 0)
                    {
                        PLOG_INFO(
                            "[PULSAR] [L.7 V5-mut] dispatcher MUTATED player! "
                            "formID=0x{:08X} before=0x{:016X} after=0x{:016X}"sv,
                            formID, hashBefore, hashAfter);
                    }
                }
            }

            s_engineDispatchFormID = prevFormID;
            return ret;
        }

        static bool HookPerFormDispatcher() noexcept
        {
            if (s_dispatcherHooked.load(std::memory_order_acquire)) return true;
            __try {
                // MinHook is initialized by Main::OnMessage(kPostLoad) before
                // InventorySync::Init runs. Defensive call here is harmless.
                MH_Initialize();

                void* target =
                    reinterpret_cast<void*>(s_perFormDispatcher.address());
                if (!target) return false;

                MH_STATUS st = MH_CreateHook(
                    target,
                    reinterpret_cast<LPVOID>(&PerFormDispatcherDetour),
                    reinterpret_cast<LPVOID*>(&s_origDispatcher));
                if (st != MH_OK) {
                    PLOG_WARN(
                        "[PULSAR] [L.7 V3.3] MH_CreateHook(FUN_14181E030) failed: {}"sv,
                        static_cast<int>(st));
                    return false;
                }

                st = MH_EnableHook(target);
                if (st != MH_OK) {
                    PLOG_WARN(
                        "[PULSAR] [L.7 V3.3] MH_EnableHook(FUN_14181E030) failed: {}"sv,
                        static_cast<int>(st));
                    return false;
                }

                s_dispatcherHooked.store(true, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] [L.7 V3.3] per-form dispatcher hook installed at 0x{:x}"sv,
                    reinterpret_cast<uintptr_t>(target));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // L.7 V2 - patch the global LoadFB vtable slot 0x06 so the
        // chunk-reader hook runs on engine's own LoadFB instances too. The
        // vtable lives in .rdata (read-only) - VirtualProtect to writable
        // for the single pointer write, restore protection after.
        //
        // Returns true if the patch is in place (or was already). Safe to
        // call multiple times - guarded by s_engineVtPatched.
        static bool PatchEngineVtable() noexcept
        {
            if (s_engineVtPatched.load(std::memory_order_acquire)) return true;

            __try {
                const auto vtAddr = s_loadFBVtable.address();
                if (!vtAddr) return false;
                auto** vt = reinterpret_cast<void**>(vtAddr);

                // Capture original chunk-reader BEFORE patching so the
                // hook can forward passthrough calls to it.
                if (!s_origChunkReader) {
                    s_origChunkReader = vt[kLoadFB_Slot_ChunkReader];
                }

                void* target = &vt[kLoadFB_Slot_ChunkReader];
                DWORD oldProt = 0;
                if (!VirtualProtect(target, sizeof(void*),
                                    PAGE_EXECUTE_READWRITE, &oldProt))
                {
                    return false;
                }
                vt[kLoadFB_Slot_ChunkReader] =
                    reinterpret_cast<void*>(&ChunkReaderHook);
                DWORD ignored = 0;
                VirtualProtect(target, sizeof(void*), oldProt, &ignored);

                s_engineVtPatched.store(true, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] [L.7 V2] engine LoadFB vtable patched "
                    "(slot 0x06 → ChunkReaderHook)"sv);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        static void* GetCustomVtable() noexcept
        {
            if (s_customVtReady.load(std::memory_order_acquire)) {
                return s_customVt;
            }

            __try {
                const auto origAddr = s_loadFBVtable.address();
                if (!origAddr) return nullptr;
                auto** orig = reinterpret_cast<void**>(origAddr);
                std::memcpy(s_customVt, orig, sizeof(s_customVt));
                // Guard: if PatchEngineVtable() already captured the engine's
                // original chunk reader, do NOT overwrite. After global vt
                // patch, slot 6 of the engine vt holds &ChunkReaderHook, so
                // reading it here would set s_origChunkReader = HOOK and
                // cause recursion (V2 crash root cause, DMP 22:45:46).
                if (!s_origChunkReader) {
                    s_origChunkReader = s_customVt[kLoadFB_Slot_ChunkReader];
                }
                s_customVt[kLoadFB_Slot_ChunkReader] =
                    reinterpret_cast<void*>(&ChunkReaderHook);
                s_customVtReady.store(true, std::memory_order_release);
                return s_customVt;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        // SaveLoadManager global (used to read the engine's current
        // saveVersion from offset +0x380 of the dereferenced pointer).
        // Same global Forms::SaveBuffer reads as g_saveCfgGlobal.
        static REL::Relocation<std::uintptr_t> s_saveCfgGlobal{
            REL::Offset{ 0x5E6D6E0 } };

        using LoadFn = void(__fastcall*)(RE::TESForm*, void*);

        static std::uint32_t ReadEngineSaveVersion() noexcept
        {
            __try {
                const auto base = s_saveCfgGlobal.address();
                if (!base) return 0;
                return *reinterpret_cast<std::uint32_t*>(base + 0x380);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        static bool SafeCallVtSlot(RE::TESForm* a_form, void* a_buf,
                                    int a_slot) noexcept
        {
            __try {
                auto** vt = *reinterpret_cast<void***>(a_form);
                auto fn = reinterpret_cast<LoadFn>(vt[a_slot]);
                fn(a_form, a_buf);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Build a properly-laid-out BGSLoadFormBuffer pointing at the blob,
        // then invoke the verified 3-call chain on the form. Returns true
        // only if all three calls completed without SEH.
        //
        // Order: LoadGame (zero state) → FinishLoadGame (parse TLV chunks
        // = real apply) → InitLoadGame (reactivate / cell-register).
        // Confirmed via FUN_140b34ca0 (engine's per-form load loop).
        static bool ApplyBlobToForm(RE::TESForm* a_form,
                                     const std::uint8_t* a_blob,
                                     std::size_t a_blobSize) noexcept
        {
            if (!a_form || !a_blob || a_blobSize == 0) return false;
            if (a_blobSize > 0xFFFFFFFFu) return false;  // u32 capacity field

            // L.7 V2 - mark this thread as "in PULSAR apply chain" so
            // the global ChunkReaderHook (patched into the engine vtable)
            // uses KEEP-allowlist semantics instead of player-drop-all.
            // RAII so it's cleared even on exception unwind.
            struct InApplyGuard {
                InApplyGuard()  noexcept { s_inApplyChain = true;  }
                ~InApplyGuard() noexcept { s_inApplyChain = false; }
            } applyGuard;

            LoadFB buf{};
            std::memset(&buf, 0, sizeof(buf));

            auto* base = buf.bytes;
            // V2 chunk-filter: swap in our custom vtable so the chunk
            // reader (vt[0x06]) drops world-state chunks before they
            // reach FinishLoadGame's switch dispatch. Fall back to the
            // engine vtable if init failed (V1 behavior - overlay all
            // chunks).
            auto* customVt = GetCustomVtable();
            *reinterpret_cast<std::uintptr_t*>(base + kOff_vtable) =
                customVt ? reinterpret_cast<std::uintptr_t>(customVt)
                         : s_loadFBVtable.address();
            *reinterpret_cast<const std::uint8_t**>(base + kOff_buffer) =
                a_blob;
            *reinterpret_cast<std::uint32_t*>(base + kOff_capacity) =
                static_cast<std::uint32_t>(a_blobSize);
            *reinterpret_cast<std::uint32_t*>(base + kOff_position) = 0u;
            // Sentinel @ +0x38 - engine-side aux-array fencepost; init must
            // be 0x80000000 to mark "stack array empty" (engine will grow
            // when needed).
            *reinterpret_cast<std::uint32_t*>(base + kOff_sentinel38) =
                0x80000000u;
            *reinterpret_cast<RE::TESForm**>(base + kOff_form) = a_form;
            *reinterpret_cast<std::uint32_t*>(base + kOff_saveVersion) =
                ReadEngineSaveVersion();
            // Sentinel @ +0xEF - loadFlag-like, expected 0xFF init.
            *reinterpret_cast<std::uint8_t*>(base + kOff_sentinelEF) = 0xFFu;

            // LoadGame (vt[0x1B]) is the engine's "zero state + re-init
            // Actor-Value-Map mutex" pass - designed to run on a freshly
            // constructed (not yet active) actor during save load. On a
            // *live* PlayerCharacter the mutex at +0xC0 is already
            // initialized and possibly held by another thread →
            // re-initializing it threw on first run (logs 16:23:42:
            // load=0 finish=1 init=1). We skip it.
            //
            // FinishLoadGame (vt[0x1D]) is the TLV-chunk parser -
            // actually applies the blob. InitLoadGame (vt[0x1C]) does
            // post-apply cell registration + Activate. Buf.position must
            // be 0 before chunk parse.
            *reinterpret_cast<std::uint32_t*>(base + kOff_position) = 0u;

            // L.7 V1 - diagnostic: hash player memory region BEFORE+AFTER
            // apply to verify whether vt[0x1D]+vt[0x1C] actually overwrites
            // live state on a post-bridge-load actor. If hashBefore == hashAfter,
            // apply is no-op → engine-applied host state persists. Different
            // hash → apply mutates state (then question becomes whether the
            // mutation contains the right blob).
            // Region: player+0x100..+0x600 - typical Actor-state band including
            // ExtraData head, anim state, AV map root, perk array head.
            auto hashRegion = [](void* p) -> std::uint64_t {
                if (!p) return 0;
                std::uint64_t h = 1469598103934665603ULL;
                __try {
                    auto* b = reinterpret_cast<const std::uint8_t*>(p) + 0x100;
                    for (std::size_t i = 0; i < 0x500; ++i) {
                        h ^= b[i];
                        h *= 1099511628211ULL;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    return 0;
                }
                return h;
            };
            const std::uint64_t hashBefore = hashRegion(a_form);

            // V5.1 - compare player byte-by-byte against the snapshot
            // captured in ArmForBridge. Tells us which 64-byte rows the
            // engine bridge mutated (regardless of whether the mutation
            // went through any of our hooked functions).
            {
                std::uint64_t diffMask = 0;
                const auto diffCount = ComparePlayerSnap(&diffMask);
                PLOG_INFO(
                    "[PULSAR] [L.7 V5.1-snap] post-bridge player diff: "
                    "bytesChanged={} rowMask=0x{:016X}"sv,
                    diffCount, diffMask);
            }

            const bool okFinish = SafeCallVtSlot(a_form, &buf, kSlot_FinishLoadGame);

            const std::uint64_t hashAfterFinish = hashRegion(a_form);

            // Re-zero position before InitLoadGame in case Finish moved
            // the cursor - Init may walk the buffer too.
            *reinterpret_cast<std::uint32_t*>(base + kOff_position) = 0u;
            const bool okInit = SafeCallVtSlot(a_form, &buf, kSlot_InitLoadGame);

            const std::uint64_t hashAfterInit = hashRegion(a_form);

            PLOG_INFO(
                "[PULSAR] [InventorySync] apply chain (skip Load, V2-filter={}): finish={} init={}"sv,
                customVt ? "ON" : "OFF",
                okFinish ? 1 : 0, okInit ? 1 : 0);
            PLOG_INFO(
                "[PULSAR] [L.7 diag] apply hash before=0x{:016X} "
                "after-finish=0x{:016X} after-init=0x{:016X} mutated={}"sv,
                hashBefore, hashAfterFinish, hashAfterInit,
                (hashBefore != hashAfterFinish || hashBefore != hashAfterInit)
                    ? "YES" : "NO");
            PLOG_INFO(
                "[PULSAR] [L.7 V3.2-diag] chunk-hook stats: engineCalls={} "
                "nonPlayer={} playerHits={} bfcbDrops={} passthroughs={} pendingMissedPlayer={}"sv,
                s_diag_engineCalls.load(std::memory_order_acquire),
                s_diag_nonPlayerCalls.load(std::memory_order_acquire),
                s_diag_playerHits.load(std::memory_order_acquire),
                s_diag_bfcbDrops.load(std::memory_order_acquire),
                s_diag_passthroughs.load(std::memory_order_acquire),
                s_diag_pendingMissedPlayer.load(std::memory_order_acquire));
            PLOG_INFO(
                "[PULSAR] [L.7 V4.1-diag] PC::FinishLoadGame stats: calls={} playerCalls={} skipped={}"sv,
                s_diag_finishCalls.load(std::memory_order_acquire),
                s_diag_finishPlayerCalls.load(std::memory_order_acquire),
                s_diag_finishSkipped.load(std::memory_order_acquire));
            PLOG_INFO(
                "[PULSAR] [L.7 V4.4-bfcb-diag] BFCB inner stats: calls={} playerCalls={} skipped={}"sv,
                s_diag_bfcbCalls.load(std::memory_order_acquire),
                s_diag_bfcbPlayerCalls.load(std::memory_order_acquire),
                s_diag_bfcbSkipped.load(std::memory_order_acquire));
            PLOG_INFO(
                "[PULSAR] [L.7 V5-mut-summary] player mutations by hook: "
                "chunkReader={} dispatcher={} bfcbInner={} pcFinishLoadGame={}"sv,
                s_diag_mut_chunkReader.load(std::memory_order_acquire),
                s_diag_mut_dispatcher.load(std::memory_order_acquire),
                s_diag_mut_bfcb.load(std::memory_order_acquire),
                s_diag_mut_pcFinish.load(std::memory_order_acquire));
            PLOG_INFO(
                "[PULSAR] [L.7 V6-summary] PerformGameReset calls={} mut={} | "
                "PlayerRefrReInit calls={} playerCalls={} mut={}"sv,
                s_diag_pgrCalls.load(std::memory_order_acquire),
                s_diag_pgrMut.load(std::memory_order_acquire),
                s_diag_prriCalls.load(std::memory_order_acquire),
                s_diag_prriPlayerCalls.load(std::memory_order_acquire),
                s_diag_prriMut.load(std::memory_order_acquire));

            return okFinish && okInit;
        }

        // ─── File format ───────────────────────────────────────────────
        // .psprof file:
        //   [4 bytes "PSPF"]
        //   [u32 version = 1]
        //   [u32 blobSize]
        //   [blobSize bytes raw BGSSaveFormBuffer output]
        constexpr char kMagic[4] = { 'P', 'S', 'P', 'F' };
        constexpr std::uint32_t kVersion       = 1;
        constexpr std::uint32_t kHeaderBytes   = 4 + 4 + 4;
        constexpr std::uint32_t kMaxBlobBytes  = 16u * 1024u * 1024u;

        // ─── State ─────────────────────────────────────────────────────
        std::atomic<bool>          s_initialized{ false };
        std::atomic<bool>          s_pendingSnapshot{ false };
        std::atomic<bool>          s_pendingApply{ false };
        std::atomic<std::uint64_t> s_lastLoadConsumed{ 0 };
        // s_preBridgeSnapshotPending defined above (before ChunkReaderHook
        // so the hook can read it without forward-declaration dance).
        std::mutex                 s_ioMu;

        std::string ProfileDir()
        {
            return GetPluginDir() + "PULSAR_Profiles\\";
        }

        std::string ProfilePath()
        {
            // Single file for now. Multi-character expansion: read player
            // base name or a config-key into the filename.
            return ProfileDir() + "client_state.psprof";
        }

        bool EnsureProfileDir()
        {
            std::error_code ec;
            std::filesystem::create_directories(ProfileDir(), ec);
            return !ec;
        }

        // Write raw blob to disk wrapped with magic + version + size header.
        // Returns true on success.
        bool WriteProfileFile(const std::vector<std::uint8_t>& a_blob,
                              const std::string& a_path)
        {
            if (a_blob.empty()) return false;
            if (a_blob.size() > kMaxBlobBytes) {
                PLOG_WARN(
                    "[PULSAR] [InventorySync] blob oversized {} > cap {}, abort"sv,
                    a_blob.size(), kMaxBlobBytes);
                return false;
            }

            std::ofstream f(a_path, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) {
                PLOG_WARN("[PULSAR] [InventorySync] cannot open {} for write"sv,
                          a_path);
                return false;
            }
            const std::uint32_t blobSize = static_cast<std::uint32_t>(a_blob.size());
            f.write(kMagic, 4);
            f.write(reinterpret_cast<const char*>(&kVersion), 4);
            f.write(reinterpret_cast<const char*>(&blobSize), 4);
            f.write(reinterpret_cast<const char*>(a_blob.data()), blobSize);
            const bool ok = f.good();
            f.close();
            return ok;
        }

        // Read blob from disk. Validates magic + version. Returns empty
        // vector on any error (missing file, bad header, oversized).
        bool ReadProfileFile(std::vector<std::uint8_t>& a_outBlob,
                             const std::string& a_path)
        {
            a_outBlob.clear();

            std::ifstream f(a_path, std::ios::binary | std::ios::ate);
            if (!f.is_open()) return false;

            const auto sz = f.tellg();
            if (sz < static_cast<std::streampos>(kHeaderBytes)) {
                PLOG_WARN("[PULSAR] [InventorySync] {} too small ({}B)"sv,
                          a_path, static_cast<std::int64_t>(sz));
                return false;
            }
            f.seekg(0);

            char magic[4]{};
            std::uint32_t version  = 0;
            std::uint32_t blobSize = 0;
            f.read(magic, 4);
            f.read(reinterpret_cast<char*>(&version), 4);
            f.read(reinterpret_cast<char*>(&blobSize), 4);
            if (std::memcmp(magic, kMagic, 4) != 0) {
                PLOG_WARN("[PULSAR] [InventorySync] {} bad magic"sv, a_path);
                return false;
            }
            if (version != kVersion) {
                PLOG_WARN(
                    "[PULSAR] [InventorySync] {} unsupported version {} (need {})"sv,
                    a_path, version, kVersion);
                return false;
            }
            if (blobSize == 0 || blobSize > kMaxBlobBytes) {
                PLOG_WARN(
                    "[PULSAR] [InventorySync] {} bad blobSize {}"sv,
                    a_path, blobSize);
                return false;
            }
            const auto avail = static_cast<std::uint64_t>(sz) - kHeaderBytes;
            if (avail < blobSize) {
                PLOG_WARN(
                    "[PULSAR] [InventorySync] {} truncated (have {} need {})"sv,
                    a_path, avail, blobSize);
                return false;
            }

            a_outBlob.resize(blobSize);
            f.read(reinterpret_cast<char*>(a_outBlob.data()), blobSize);
            return f.good();
        }

        // Game-thread: serialize current player → write to disk.
        // Caller must guarantee player is loaded into a cell.
        void DoSnapshot()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->parentCell) {
                PLOG_INFO(
                    "[PULSAR] [InventorySync] snapshot: no player/cell, skip"sv);
                return;
            }

            auto* form = static_cast<RE::TESForm*>(player);
            const std::uint32_t fid = form->formID;

            auto blob = Pulsar::Forms::SerializeForm(form, 0xFFFFFFFFu);
            if (blob.empty()) {
                PLOG_WARN(
                    "[PULSAR] [InventorySync] snapshot: SerializeForm empty for 0x{:08X}"sv,
                    fid);
                return;
            }

            std::lock_guard io(s_ioMu);
            if (!EnsureProfileDir()) {
                PLOG_WARN(
                    "[PULSAR] [InventorySync] snapshot: cannot create profile dir"sv);
                return;
            }
            const auto path = ProfilePath();
            if (!WriteProfileFile(blob, path)) {
                PLOG_WARN(
                    "[PULSAR] [InventorySync] snapshot: write failed {}"sv, path);
                return;
            }

            PLOG_INFO(
                "[PULSAR] [InventorySync] snapshot OK: 0x{:08X} → {}B → {}"sv,
                fid, blob.size(), path);
        }

        // Game-thread: read profile from disk → DeserializeForm onto player.
        // Caller must guarantee player is loaded into a cell.
        void DoApply()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->parentCell) {
                PLOG_INFO(
                    "[PULSAR] [InventorySync] apply: no player/cell, skip"sv);
                return;
            }

            std::vector<std::uint8_t> blob;
            std::string path;
            bool haveBlob = false;
            {
                std::lock_guard io(s_ioMu);
                path = ProfilePath();
                haveBlob = ReadProfileFile(blob, path);
            }

            // A missing profile file (first MP connect, or the pre-bridge
            // DoSnapshot skipped because the player had no cell yet) only
            // skips the M.3 blob chain. It must NOT skip the DirectRestore
            // pass below: returning here used to leave the client on the
            // host's bridged loadout and kept the bridge LockCache set, which
            // froze captures for the rest of the session.
            if (haveBlob) {
                auto* form = static_cast<RE::TESForm*>(player);
                const std::uint32_t fid = form->formID;

                // 3-call chain (LoadGame zeros, FinishLoadGame parses TLV chunks,
                // InitLoadGame reactivates). Forms::DeserializeForm only does
                // the LoadGame zero - insufficient for real state restore.
                const bool ok = ApplyBlobToForm(form, blob.data(), blob.size());

                PLOG_INFO(
                    "[PULSAR] [InventorySync] apply: 0x{:08X} ← {}B from {} → {}"sv,
                    fid, blob.size(), path, ok ? "ok" : "FAILED");
            } else {
                PLOG_INFO(
                    "[PULSAR] [InventorySync] apply: no profile at {} - skipping blob "
                    "chain, DirectRestore still runs"sv,
                    path);
            }

            // L.7 Plan B - direct API restore. Runs AFTER the existing
            // M.3 vt[0x1D]/vt[0x1C] chain so any items the chain managed to
            // restore stick; this pass converges any residual delta back to
            // the pre-bridge client state. Bypasses the engine apply
            // mechanism entirely (which V5/V6 evidence showed routes through
            // an unknown code path).
            //
            // (2026-06-20) - dbAuth mid-session bridge (F8 / fast-travel):
            // RestoreClientState MOVES-out + clears g_cache, so the JOIN restore
            // already consumed it. A second bridge restore would then hit
            // "cache is EMPTY - skipping restore" and leave the client wearing
            // the host's loadout. ReapplyCachedDbProfile re-SEEDS the cache from
            // the latest clean snapshot (within-session gains preserved) and
            // runs RestoreClientState itself - the same proven path the both-down
            // recovery uses. Returns false only when there is no dbAuth baseline
            // (bootstrap/legacy save), where the pre-bridge-captured cache is the
            // right source → fall through to the bare RestoreClientState.
            //
            // v2 - the bridge LockCache (set in WorldSync::OnSaveReceived)
            // makes SeedCacheFromProfile skip ("cache locked, skip seed"), so the
            // re-seed silently no-ops and the restore still finds an EMPTY cache.
            // By the time DoApply runs the save-load is complete and we ARE the
            // restore, so the lock has served its purpose - release it here so the
            // deliberate re-seed can repopulate g_cache. (Both-down's
            // ReapplyCachedDbProfile runs on a LOCAL reload with no bridge lock,
            // which is why it was never hit; the F8/fast-travel bridge sets it.)
            DirectRestore::UnlockCache();
            if (!Pulsar::Profile::ReapplyCachedDbProfile()) {
                DirectRestore::RestoreClientState();
            }

            // V1 (2026-05-10): cleared this in OnLoadCompleted (queue time).
            // V2 test (2026-05-11 19:00) showed the race: PKT_POSITION's
            // teleport-strategy decision ran 3ms AFTER OnLoadCompleted but
            // 11ms BEFORE DoApply, so it observed `freshSoloLoad=false` and
            // picked kNone (same-cell skip) instead of kOffsetBehindHost.
            // Clearing here (post-apply) means the flag stays set through
            // the entire LOADED-state strategy decision window.
            s_preBridgeSnapshotPending.store(false, std::memory_order_release);
        }
    }

    // ─── Public API ────────────────────────────────────────────────────

    void Init()
    {
        if (s_initialized.exchange(true)) return;
        std::error_code ec;
        std::filesystem::create_directories(ProfileDir(), ec);
        PLOG_INFO("[PULSAR] [InventorySync] Init - profile dir {}"sv,
                  ProfileDir());

        // L.7 V3 - re-enabled 2026-05-12 after DMP analysis. V2 crash
        // root cause was infinite recursion: GetCustomVtable() captured
        // s_origChunkReader from s_customVt[6] *after* this PatchEngineVtable
        // already swapped engine vt[6] to HOOK → orig == HOOK → recursion.
        // Fixed by guarding the write in GetCustomVtable. Hook behavior also
        // narrowed from drop-ALL-chunks-for-player to drop-only-BFCB for
        // smaller blast radius (DATA/NAME/EDID still pass through engine).
        const bool patched = PatchEngineVtable();
        if (!patched) {
            PLOG_WARN("[PULSAR] [L.7 V3] engine vtable patch FAILED"sv);
        }

        // L.7 V3.3 - also hook the per-form dispatcher so the chunk
        // reader knows which form's chunks it is currently streaming.
        // Without this, the engine bridge's LoadFB instances leave buf+0xD8
        // null/garbage and the buf-side form check misses every player
        // chunk (V3.2 diagnostic: playerHits=0 across 47 calls).
        const bool dispatcherHooked = HookPerFormDispatcher();
        if (!dispatcherHooked) {
            PLOG_WARN(
                "[PULSAR] [L.7 V3.3] per-form dispatcher hook FAILED"sv);
        }

        // L.7 V4 - primary intercept now. PC::FinishLoadGame is the
        // function that actually applies BFCB to live player; hooking it
        // bypasses uncertainty about whether engine routes player through
        // the per-form dispatcher / chunk-reader vtable.
        const bool pcFinishHooked = HookPcFinishLoadGame();
        if (!pcFinishHooked) {
            PLOG_WARN(
                "[PULSAR] [L.7 V4] PC::FinishLoadGame hook FAILED"sv);
        }

        const bool bfcbHooked = HookBfcbInnerLoader();
        if (!bfcbHooked) {
            PLOG_WARN(
                "[PULSAR] [L.7 V4.4] BFCB inner-loader hook FAILED"sv);
        }

        const bool pgrHooked = HookPerformGameReset();
        if (!pgrHooked) {
            PLOG_WARN(
                "[PULSAR] [L.7 V6-pgr] PerformGameReset hook FAILED"sv);
        }
        const bool prriHooked = HookPlayerRefrReInit();
        if (!prriHooked) {
            PLOG_WARN(
                "[PULSAR] [L.7 V6-prri] PlayerRefrReInit hook FAILED"sv);
        }
    }

    void Shutdown()
    {
        s_initialized.store(false);
        s_pendingSnapshot.store(false);
        s_pendingApply.store(false);
    }

    void RequestSnapshot()
    {
        if (!s_initialized.load()) return;
        s_pendingSnapshot.store(true, std::memory_order_release);
    }

    void RequestApply()
    {
        if (!s_initialized.load()) return;
        s_pendingApply.store(true, std::memory_order_release);
    }

    void DrainPending()
    {
        if (!s_initialized.load()) return;

        // Apply runs first so that subsequent L.7 fast-travel-TP corrects
        // the position written by the loaded blob.
        if (s_pendingApply.exchange(false, std::memory_order_acq_rel)) {
            DoApply();
        }
        if (s_pendingSnapshot.exchange(false, std::memory_order_acq_rel)) {
            DoSnapshot();
        }
    }

    void TickPreBridgeSnapshot()
    {
        // Game-thread tick. Fires snapshot ONCE per save-bridge cycle the
        // moment all conditions are right:
        // we are client (not host)
        // network is connected
        // WorldSync state == WAITING_FOR_SAVE (handshake done, no save
        //     received yet)
        // player exists with parentCell (solo save loaded)
        //
        // Resets when state leaves WAITING_FOR_SAVE so the next save-bridge
        // cycle (e.g. on reconnect) gets a fresh snapshot.
        static std::atomic<bool> s_snapshotDoneThisCycle{ false };

        auto& net = Pulsar::Network::GetSingleton();
        if (net.IsHost() || !net.IsConnected()) {
            s_snapshotDoneThisCycle.store(false, std::memory_order_release);
            return;
        }

        const auto state = Pulsar::WorldSync::GetState();
        if (state != Pulsar::WorldSync::State::WAITING_FOR_SAVE) {
            // Not in the snapshot window - reset latch so next cycle re-fires
            s_snapshotDoneThisCycle.store(false, std::memory_order_release);
            return;
        }

        if (s_snapshotDoneThisCycle.load(std::memory_order_acquire)) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;  // wait for load

        // All gates passed - fire snapshot exactly once for this cycle.
        s_snapshotDoneThisCycle.store(true, std::memory_order_release);
        // Signal WorldSync to ignore L.7 same-cell-match for this
        // bridge cycle (cleared on apply or disconnect). Necessary because
        // the L.7 pre-load snapshot will be captured from client's solo
        // save and would otherwise short-circuit the teleport-to-host.
        s_preBridgeSnapshotPending.store(true, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] [InventorySync] TickPreBridgeSnapshot → window open, queue snapshot"sv);
        // L.7 Plan B - capture inventory NOW (game-thread, player
        // alive, pre-bridge solo state). Cached for use in DoApply after
        // the engine bridge overwrites the live inventory with host's.
        DirectRestore::CaptureClientState();
        RequestSnapshot();
    }

    void OnNetworkDisconnect()
    {
        // Called from network thread on peer disconnect. Host: no-op.
        // Client: queue snapshot - captures whatever items/equipment the
        // player had during the just-ended MP session.
        s_preBridgeSnapshotPending.store(false, std::memory_order_release);
        // L.7 Plan B - invalidate direct-restore cache on disconnect
        // so no stale items get re-applied if the user reconnects later.
        DirectRestore::Reset();
        auto& net = Pulsar::Network::GetSingleton();
        if (net.IsHost()) return;
        PLOG_INFO(
            "[PULSAR] [InventorySync] OnNetworkDisconnect → queue snapshot"sv);
        RequestSnapshot();
    }

    bool IsPreBridgeSnapshotPending()
    {
        return s_preBridgeSnapshotPending.load(std::memory_order_acquire);
    }

    // V4.4 - one-shot diagnostic: read PC singleton's vt[0x1D] at runtime
    // and compare to our hooked RVA. Confirms whether 0x140B230A0 is
    // actually PlayerCharacter::FinishLoadGame in this build.
    static void LogPcVtSlot1D()
    {
        static std::atomic<bool> s_logged{ false };
        if (s_logged.exchange(true, std::memory_order_acq_rel)) return;
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                PLOG_INFO("[PULSAR] [L.7 V4.4] PC singleton null at ArmForBridge"sv);
                s_logged.store(false, std::memory_order_release);
                return;
            }
            auto** vt = *reinterpret_cast<void***>(player);
            void* fp = vt[0x1D];
            PLOG_INFO(
                "[PULSAR] [L.7 V4.4] PC singleton=0x{:016X} vtable=0x{:016X} vt[0x1D]=0x{:016X} (hook target=0x{:016X})"sv,
                reinterpret_cast<std::uintptr_t>(player),
                reinterpret_cast<std::uintptr_t>(vt),
                reinterpret_cast<std::uintptr_t>(fp),
                static_cast<std::uintptr_t>(s_pcFinishLoadGame.address()));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] [L.7 V4.4] PC vt[0x1D] lookup faulted"sv);
        }
    }

    void ArmForBridge()
    {
        LogPcVtSlot1D();
        // Called from WorldSync::OnSaveReceived on EVERY bridge (initial,
        // F8-from-host, planet landing, etc.). Without this, the flag is only
        // armed by TickPreBridgeSnapshot on the WAITING_FOR_SAVE transition,
        // which doesn't repeat for subsequent bridges → V3 hook missed BFCB
        // on the 2nd bridge → host items overwrote client (observed in
        // 19:03:33 F8 log).
        s_preBridgeSnapshotPending.store(true, std::memory_order_release);
        // Reset diagnostic counters for a clean per-bridge tally.
        s_diag_engineCalls.store(0, std::memory_order_release);
        s_diag_playerHits.store(0, std::memory_order_release);
        s_diag_bfcbDrops.store(0, std::memory_order_release);
        s_diag_passthroughs.store(0, std::memory_order_release);
        s_diag_nonPlayerCalls.store(0, std::memory_order_release);
        s_diag_pendingMissedPlayer.store(0, std::memory_order_release);
        s_diag_finishCalls.store(0, std::memory_order_release);
        s_diag_finishPlayerCalls.store(0, std::memory_order_release);
        s_diag_finishSkipped.store(0, std::memory_order_release);
        s_diag_finishLogBudget.store(30, std::memory_order_release);
        s_diag_bfcbCalls.store(0, std::memory_order_release);
        s_diag_bfcbPlayerCalls.store(0, std::memory_order_release);
        s_diag_bfcbSkipped.store(0, std::memory_order_release);
        s_diag_bfcbLogBudget.store(30, std::memory_order_release);
        // V5: mutation watchpoint counters + log budget
        s_diag_mut_chunkReader.store(0, std::memory_order_release);
        s_diag_mut_dispatcher.store(0, std::memory_order_release);
        s_diag_mut_bfcb.store(0, std::memory_order_release);
        s_diag_mut_pcFinish.store(0, std::memory_order_release);
        s_diag_mutLogBudget.store(40, std::memory_order_release);
        // V6: PerformGameReset + PlayerRefrReInit counters
        s_diag_pgrCalls.store(0, std::memory_order_release);
        s_diag_pgrMut.store(0, std::memory_order_release);
        s_diag_pgrLogBudget.store(5, std::memory_order_release);
        s_diag_prriCalls.store(0, std::memory_order_release);
        s_diag_prriPlayerCalls.store(0, std::memory_order_release);
        s_diag_prriMut.store(0, std::memory_order_release);
        s_diag_prriLogBudget.store(20, std::memory_order_release);
        // V5.1: capture full 0x1000-byte player snapshot for post-bridge
        // byte-level diff. Dumped in DoApply (after engine bridge done,
        // before our M.3 apply runs).
        CapturePlayerSnap();
        // V3.3: log the first 30 dispatcher entries per bridge so we can
        // verify formID extraction works on real bridge data. After
        // confirmed-correct in production, lower to 0 to silence.
        // V4: dispatcher logs no longer load-bearing - lower to 0.
        s_diag_dispatchLogBudget.store(0, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] [InventorySync] ArmForBridge → preBridgeSnapshotPending=true (diag counters reset)"sv);
    }

    void OnLoadCompleted()
    {
        // Called from game thread when WorldSync state machine reaches
        // LOADED + settle. Caller fires this every tick while settled -
        // we only act on the rising edge (new load tick).
        auto& net = Pulsar::Network::GetSingleton();
        if (net.IsHost()) return;

        const std::uint64_t lastLoad = Pulsar::WorldSync::GetLastLoadCompletedTick();
        if (lastLoad == 0) return;
        if (lastLoad <= s_lastLoadConsumed.load(std::memory_order_acquire)) return;

        s_lastLoadConsumed.store(lastLoad, std::memory_order_release);
        // Do NOT clear s_preBridgeSnapshotPending here - DoApply does it
        // after the actual apply has run. Clearing at queue time created
        // a race where PKT_POSITION's strategy decision (~3ms later)
        // observed false and skipped teleport-to-host.
        PLOG_INFO(
            "[PULSAR] [InventorySync] OnLoadCompleted (load tick {}) → queue apply"sv,
            lastLoad);
        RequestApply();
    }

    void OnHotkeySnapshot()
    {
        PLOG_INFO("[PULSAR] [InventorySync] hotkey snapshot"sv);
        RequestSnapshot();
    }

    void OnHotkeyApply()
    {
        PLOG_INFO("[PULSAR] [InventorySync] hotkey apply"sv);
        RequestApply();
    }

    std::string GetProfilePath() { return ProfilePath(); }
}
