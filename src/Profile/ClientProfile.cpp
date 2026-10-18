#include "PCH.h"
#include "Profile/ClientProfile.h"
#include "Profile/ProfileJson.h"
#include "Config.h"
#include "NPCSync/PluginLookup.h"
#include "WorldSync/LootSync.h"
#include "InventorySync/DirectRestore.h"   // A Phase 3: seed cache from DB profile
#include "Coop/CoopMortality.h"             ///gate clean-baseline cache on not-downed

#include <RE/B/BGSPerk.h>                   // Phase C0: walk player perks
#include <RE/S/SpellItem.h>                 // Powers (gap #2): GetSpellType filter
#include <RE/M/MagicSystem.h>               // MagicSystem::SpellType

#include <chrono>                            ///poison-window timestamps
#include <excpt.h>                          // Phase C0: SEH (EXCEPTION_EXECUTE_HANDLER)
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>                    // Phase C3: gains delta-merge
#include <unordered_set>                    // Powers: gains set-delta
#include <utility>

using namespace std::string_view_literals;

namespace Pulsar::Profile {

    // JSON helpers shared with the codec (ProfileCodec.cpp).
    using Json::ExtractInt;
    using Json::ExtractString;
    using Json::ExtractUInt;
    static std::string EscapeJSON(const std::string& s) { return Json::Escape(s); }

    // Phase C0 - SEH-isolated perk walk. Mirrors DirectRestore::WalkPerks: the
    // perk array can be mid-mutation right after a bridge, and __try cannot share
    // a function with C++ unwinding objects, so this fills a POD buffer that
    // CaptureProfile then copies into ProfilePerk records (with strings) outside
    // the __try.
    struct PerkPair { std::uint32_t formID; std::int8_t rank; };
    static bool SafeWalkPerks(RE::PlayerCharacter* a_player,
                              std::vector<PerkPair>& a_out) noexcept
    {
        __try {
            auto* pk = a_player->perks;
            if (pk && pk->perkRanks) {
                for (const auto& prd : *pk->perkRanks) {
                    if (prd.perk) a_out.push_back({ prd.perk->formID, prd.currentRank });
                }
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Powers (gap #2) - SEH-isolated walk of the actor's added-spell array
    // (begin=*(actor+0x2c8), end=*(actor+0x2d0), SpellItem* entries) filtered to
    // power-type spells. In Starfield the Starborn powers are SpellType **kSpell(0)**
    // (verified: a 24-power char's list had exactly 24 type-0 spells; type-4 kAbility
    // = gear/trait abilities, excluded). Keep kPower/kLesserPower/kVoicePower too
    // (defensive). Mirrors DirectRestore::IsPowerType. POD-only frame (a_out is a ref).
    static bool SafeWalkPowers(RE::PlayerCharacter* a_player,
                               std::vector<std::uint32_t>& a_out) noexcept
    {
        __try {
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(a_player);
            auto* begin = *reinterpret_cast<RE::SpellItem***>(base + 0x2c8);
            auto* end   = *reinterpret_cast<RE::SpellItem***>(base + 0x2d0);
            if (begin && end > begin) {
                std::size_t n = static_cast<std::size_t>(end - begin);
                if (n > 4096) n = 4096;
                for (std::size_t i = 0; i < n; ++i) {
                    RE::SpellItem* s = begin[i];
                    if (!s) continue;
                    const auto t = s->GetSpellType();
                    if (t == RE::MagicSystem::SpellType::kSpell ||
                        t == RE::MagicSystem::SpellType::kPower ||
                        t == RE::MagicSystem::SpellType::kLesserPower ||
                        t == RE::MagicSystem::SpellType::kVoicePower) {
                        a_out.push_back(s->formID);
                    }
                }
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Research-manager singleton global (RVA 0x61E26A0). At file scope: a static
    // REL::Relocation inside a __try frame trips C2712 (its init-guard needs unwinding).
    static REL::Relocation<std::uintptr_t*> g_researchMgrGlobal{ REL::Offset(0x61E26A0) };

    // Research (2026-06-19, gap #1) - walk the research-manager's std::map and
    // collect the formIDs of COMPLETED projects (state byte == 2). RE'd + in-game
    // verified: singleton global @ RVA 0x61E26A0; map @ mgr+0x18; MSVC _Tree node:
    // _Left@+0x00 _Parent@+0x08 _Right@+0x10 _Isnil@+0x19 key(formID)@+0x20
    // value-ptr@+0x28; per-project state byte @ *(value)+0x08 (==2 -> researched).
    // POD-only __try frame (raw RB-tree walk); pre-order DFS via an explicit stack,
    // nil children (the _Myhead sentinel) skipped by the _Isnil check on pop.
    // Returns total nodes visited (-1 on fault); a_done fills with completed formIDs.
    static int SafeWalkResearch(std::vector<std::uint32_t>& a_done) noexcept
    {
        int total = 0;
        std::uint32_t buf[1024];
        int n = 0;
        __try {
            const std::uintptr_t mgr = *g_researchMgrGlobal;
            if (mgr) {
                const std::uintptr_t head = *reinterpret_cast<std::uintptr_t*>(mgr + 0x18);
                if (head) {
                    std::uintptr_t stack[512];
                    int sp = 0;
                    const std::uintptr_t root = *reinterpret_cast<std::uintptr_t*>(head + 0x08);
                    if (root) stack[sp++] = root;
                    while (sp > 0 && total < 4096) {
                        std::uintptr_t node = stack[--sp];
                        if (!node) continue;
                        if (*reinterpret_cast<std::uint8_t*>(node + 0x19) != 0) continue; // _Isnil
                        ++total;
                        std::uintptr_t val = *reinterpret_cast<std::uintptr_t*>(node + 0x28);
                        if (val && *reinterpret_cast<std::uint8_t*>(val + 0x08) == 2) {
                            if (n < 1024) buf[n++] = *reinterpret_cast<std::uint32_t*>(node + 0x20);
                        }
                        if (sp < 510) {
                            std::uintptr_t l = *reinterpret_cast<std::uintptr_t*>(node + 0x00);
                            std::uintptr_t r = *reinterpret_cast<std::uintptr_t*>(node + 0x10);
                            if (l) stack[sp++] = l;
                            if (r) stack[sp++] = r;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        a_done.assign(buf, buf + n);
        return total;
    }

    // Phase C0 - unspent skill points live at PlayerCharacter + 0xF60 (same offset
    // DirectRestore reads). SEH-isolated single read.
    static bool SafeReadSkillPoints(RE::PlayerCharacter* a_player,
                                    std::uint32_t& a_out) noexcept
    {
        __try {
            a_out = *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(a_player) + 0xF60);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // V3 (2026-06-19) - read the player's character level via the player's NPC
    // (TESActorBaseData::GetLevel). SEH-isolated: GetNPC()/As<> + the engine call
    // can touch a mid-bridge-mutating actor. Returns 0 on failure. INSTRUMENTATION
    // STEP: we log this to verify it returns hall's real level before building
    // level persistence on top (capture-only, option A).
    static std::uint32_t SafeReadPlayerLevel(RE::PlayerCharacter* a_player) noexcept
    {
        // CommonLibSF TESActorBaseData::GetLevel has REL::ID{0} (unbound placeholder)
        // → "invalid id 0" crash. Read the field RAW instead: TESNPC inherits
        // TESActorBaseData; actorData (ACTOR_BASE_DATA) is at +0x08, .level (uint16)
        // at +0x06 within it. GetNPC()/GetBaseObject() are plain field reads (safe).
        std::uint32_t lvl = 0;
        __try {
            if (auto* npc = a_player->GetNPC()) lvl = npc->actorData.level;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            lvl = 0;
        }
        return lvl;
    }

}  // namespace Pulsar::Profile

namespace Pulsar::Profile {

    static std::string GetProfilePath(const std::string& a_playerName)
    {
        std::string dir = GetPluginDir() + "PULSAR_Profiles\\";
        std::filesystem::create_directories(dir);
        return dir + a_playerName + ".json";
    }

    bool Save(const ProfileData& a_data)
    {
        if (a_data.playerName.empty()) return false;

        std::string path = GetProfilePath(a_data.playerName);
        std::ofstream file(path);
        if (!file.is_open()) {
            PLOG_INFO("[PULSAR] ClientProfile: Failed to write {}"sv, path);
            return false;
        }

        file << "{\n";
        file << "  \"version\": 1,\n";
        file << "  \"playerName\": \"" << EscapeJSON(a_data.playerName) << "\",\n";
        file << "  \"playerLevel\": " << a_data.playerLevel << ",\n";
        file << "  \"raceName\": \"" << EscapeJSON(a_data.raceName) << "\",\n";
        file << "  \"gender\": " << a_data.gender << ",\n";
        file << "  \"weaponFormID\": " << a_data.weaponFormID << ",\n";

        // Armor array
        file << "  \"armorFormIDs\": [";
        for (size_t i = 0; i < a_data.armorFormIDs.size(); ++i) {
            if (i > 0) file << ", ";
            file << a_data.armorFormIDs[i];
        }
        file << "],\n";

        // Inventory array
        file << "  \"inventory\": [\n";
        for (size_t i = 0; i < a_data.inventory.size(); ++i) {
            auto& item = a_data.inventory[i];
            file << "    {\"formID\": " << item.formID << ", \"count\": " << item.count << "}";
            if (i + 1 < a_data.inventory.size()) file << ",";
            file << "\n";
        }
        file << "  ]\n";
        file << "}\n";

        file.close();
        PLOG_INFO("[PULSAR] ClientProfile: Saved {} ({} items, weapon=0x{:08X}, {} armor)"sv,
            a_data.playerName, a_data.inventory.size(), a_data.weaponFormID, a_data.armorFormIDs.size());
        return true;
    }

    // ─── Simple JSON reader ───

    static std::string ReadFileToString(const std::string& a_path)
    {
        std::ifstream file(a_path);
        if (!file.is_open()) return "";
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    bool Load(const std::string& a_playerName, ProfileData& a_out)
    {
        if (a_playerName.empty()) return false;

        std::string path = GetProfilePath(a_playerName);
        std::string json = ReadFileToString(path);
        if (json.empty()) {
            PLOG_INFO("[PULSAR] ClientProfile: No profile at {}"sv, path);
            return false;
        }

        a_out.playerName  = ExtractString(json, "playerName");
        a_out.playerLevel = ExtractUInt(json, "playerLevel");
        a_out.raceName    = ExtractString(json, "raceName");
        a_out.gender      = static_cast<uint16_t>(ExtractUInt(json, "gender"));
        a_out.weaponFormID = ExtractUInt(json, "weaponFormID");

        // Parse armorFormIDs array
        a_out.armorFormIDs.clear();
        auto armorPos = json.find("\"armorFormIDs\"");
        if (armorPos != std::string::npos) {
            auto arrStart = json.find('[', armorPos);
            auto arrEnd = json.find(']', arrStart);
            if (arrStart != std::string::npos && arrEnd != std::string::npos) {
                std::string arr = json.substr(arrStart + 1, arrEnd - arrStart - 1);
                size_t p = 0;
                while (p < arr.size()) {
                    while (p < arr.size() && (arr[p] == ' ' || arr[p] == ',' || arr[p] == '\n' || arr[p] == '\r')) ++p;
                    if (p >= arr.size()) break;
                    uint32_t val = static_cast<uint32_t>(std::strtoul(arr.c_str() + p, nullptr, 10));
                    if (val > 0) a_out.armorFormIDs.push_back(val);
                    while (p < arr.size() && arr[p] != ',' && arr[p] != ']') ++p;
                }
            }
        }

        // Parse inventory array
        a_out.inventory.clear();
        auto invPos = json.find("\"inventory\"");
        if (invPos != std::string::npos) {
            auto arrStart = json.find('[', invPos);
            auto arrEnd = json.rfind(']');
            if (arrStart != std::string::npos && arrEnd != std::string::npos) {
                std::string arr = json.substr(arrStart + 1, arrEnd - arrStart - 1);
                // Find each {...} object
                size_t p = 0;
                while (p < arr.size()) {
                    auto objStart = arr.find('{', p);
                    if (objStart == std::string::npos) break;
                    auto objEnd = arr.find('}', objStart);
                    if (objEnd == std::string::npos) break;
                    std::string obj = arr.substr(objStart, objEnd - objStart + 1);
                    ProfileItem item;
                    item.formID = ExtractUInt(obj, "formID");
                    item.count  = ExtractInt(obj, "count");
                    if (item.formID > 0 && item.count > 0) {
                        a_out.inventory.push_back(item);
                    }
                    p = objEnd + 1;
                }
            }
        }

        PLOG_INFO("[PULSAR] ClientProfile: Loaded {} ({} items, weapon=0x{:08X}, {} armor)"sv,
            a_out.playerName, a_out.inventory.size(), a_out.weaponFormID, a_out.armorFormIDs.size());
        return true;
    }

    static constexpr uint32_t kCreditsFormID = 0x0000000F;  // see DirectRestore.cpp:678

    // Phase C3 (co-op gains) - the authoritative DB profile this dbAuth session
    // restored FROM. The gains-capture starts from THIS (frozen, faithful) and
    // applies only the session delta on top, so existing items/mods/perks/credits
    // never drift. Set by ApplyProfileData at a dbAuth join; consumed by
    // CaptureProfile. Producer + consumer both live in this file (no cross-module
    // struct sharing). Valid only in dbAuth mode.
    static ProfileData g_dbBaseline;
    static bool        g_dbBaselineValid = false;

    ///LATEST clean (host-free, not-downed) capture this session = the both-down
    // recovery source. Updated on every clean capture (both bootstrap AND dbAuth) so a
    // both-down restores the client's CURRENT progress (items/level/XP/skills/powers), not the
    // frozen join state - within-session gains survive the game-over reload (≤ capture cadence
    // loss). NOT updated during the both-down/reload poison window (host save loaded) so a
    // host-bridged capture can't overwrite the good snapshot. Game-thread only.
    static ProfileData g_lastCleanBaseline;
    static bool        g_lastCleanBaselineValid = false;
    // Suppress clean-baseline caching until this steady-clock ms (set on a save-load start;
    // covers the reload -> ~5s settle -> re-restore window where the live state is host items).
    static uint64_t    g_suppressCleanCacheUntilMs = 0;
    static inline uint64_t NowMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    // Called from the loadMenu RISING edge (Main.cpp): a save-load just started -> the live
    // state is about to become the host-bridged save -> don't cache it as the clean snapshot.
    void NotifySaveLoadStarted()
    {
        g_suppressCleanCacheUntilMs = NowMs() + 15000;
    }

    // Delta-merge: result = DB baseline + (live − post-restore snapshot). The
    // snapshot carries the SAME restore artifacts + host credits as `live`, so
    // they cancel; only TRUE session gains/losses move. New distinct items arrive
    // with their faithful live mods; existing items take count deltas; equipped
    // reflects the current loadout.
    static std::vector<ProfileItem> DeltaMergeInventory(
        const std::vector<ProfileItem>& a_baseline,
        const std::vector<ProfileItem>& a_live,
        const std::unordered_map<uint32_t, uint32_t>& a_snap,
        const std::unordered_set<uint32_t>& a_foreign)
    {
        std::unordered_map<uint32_t, ProfileItem> merged;
        merged.reserve(a_baseline.size() + a_live.size());
        for (const auto& b : a_baseline) merged[b.formID] = b;

        std::unordered_map<uint32_t, bool> liveSeen;
        for (const auto& L : a_live) {
            // #poison-guard (overnight 2026-06-24): a HOST-exclusive form the bridge
            // placed in our inventory must NEVER feed the DB gains-upload. The +10s
            // snapshot cancels host items only if they're present at snapshot time;
            // one that leaks back into live AFTER the snapshot would otherwise look
            // exactly like a looted gain (snapCount=0, delta>0 → added). Skip it
            // entirely - leave merged at the client's DB baseline for this form.
            if (a_foreign.count(L.formID)) continue;
            liveSeen[L.formID] = true;
            auto snapIt = a_snap.find(L.formID);
            const int32_t snapCount = (snapIt != a_snap.end())
                ? static_cast<int32_t>(snapIt->second) : 0;
            const int32_t delta = L.count - snapCount;       // session gain(+)/loss(−)
            auto it = merged.find(L.formID);
            if (it != merged.end()) {
                it->second.count   += delta;                 // ammo/resource delta
                it->second.equipped = L.equipped;            // current loadout
            } else if (delta > 0) {
                ProfileItem n = L;                           // NEW gain → faithful live mods
                n.count = delta;
                merged[L.formID] = std::move(n);
            }
        }
        // Items present at snapshot but gone now → consumed/dropped this session.
        for (const auto& [fid, cnt] : a_snap) {
            if (a_foreign.count(fid)) continue;              // #poison - never let a host form move the count
            if (liveSeen.count(fid)) continue;
            auto it = merged.find(fid);
            if (it != merged.end()) it->second.count -= static_cast<int32_t>(cnt);
        }

        std::vector<ProfileItem> out;
        out.reserve(merged.size());
        for (auto& [fid, item] : merged)
            if (item.count > 0) out.push_back(std::move(item));
        return out;
    }

    // LEGACY / UNUSED: ClientProfile is now seed-only (it builds the
    // DirectRestore seed at line ~420; it no longer applies items itself). The
    // mod RE-APPLY lives in DirectRestore.cpp behind ITS own
    // kApplyModsOnRestore, which is now ENABLED and drives the engine's native
    // attach (LootSync::AttachItemModsNative - fixed 2026-06-18). The blob
    // round-trips the mod arrays for the seed. Kept here only to document the
    // history; this constant is not read anywhere.
    static constexpr bool kApplyModsOnRestore = false;

    // Credits APPLY is gated off too: AddObjectToContainer does NOT drive money
    // (DirectRestore.cpp:678 - null-extra add zeroed credits). Credits are still
    // captured into the blob; re-enable once a proper money API is RE'd.
    static constexpr bool kApplyCreditsOnRestore = false;

    ProfileData CaptureProfile()
    {
        ProfileData data;
        data.version = 2;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            PLOG_WARN("[PROFILE] CaptureProfile: no player"sv);
            return data;
        }

        // Build the equipped-form set (and legacy weapon/armor fields the
        // existing connect-send path still reads).
        std::vector<uint32_t> equippedForms;
        player->ForEachEquippedItem([&](const RE::BGSInventoryItem& item) -> RE::BSContainer::ForEachResult {
            if (!item.object) return RE::BSContainer::ForEachResult::kContinue;
            const uint32_t fid = item.object->GetFormID();
            equippedForms.push_back(fid);
            if (item.object->IsWeapon() && data.weaponFormID == 0) {
                data.weaponFormID = fid;
            } else if (item.object->GetFormType() == RE::FormType::kARMO) {
                data.armorFormIDs.push_back(fid);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        auto isEquipped = [&](uint32_t fid) {
            for (uint32_t e : equippedForms) if (e == fid) return true;
            return false;
        };

        player->ForEachInventoryItem([&](const RE::BGSInventoryItem& a_item) -> RE::BSContainer::ForEachResult {
            if (!a_item.object) return RE::BSContainer::ForEachResult::kContinue;
            const uint32_t fid = a_item.object->GetFormID();
            int32_t total = 0;
            for (auto& stack : a_item.stacks) total += static_cast<int32_t>(stack.count);
            if (total <= 0) return RE::BSContainer::ForEachResult::kContinue;

            // Credits go into their own field, not the item list.
            if (fid == kCreditsFormID) {
                data.credits = static_cast<uint32_t>(total);
                return RE::BSContainer::ForEachResult::kContinue;
            }

            ProfileItem it;
            it.formID   = fid;
            it.count    = total;
            it.equipped = isEquipped(fid);

            // T34-encode the item form so it resolves on another machine / load order.
            char pluginBuf[260] = {};
            if (Pulsar::NPCSync::PluginLookup::FormIDToPluginName(fid, pluginBuf, sizeof(pluginBuf))) {
                it.plugin  = pluginBuf;
                it.localID = Pulsar::NPCSync::PluginLookup::FormIDToLocalID(fid);
            }

            // Mods (raw uint32 array + sentinel) via the M.2 primitive.
            Pulsar::WorldSync::Loot::ReadItemMods(player, fid, it.mods, it.modSentinel);

            data.inventory.push_back(std::move(it));
            return RE::BSContainer::ForEachResult::kContinue;
        });

        // O2 - exact pattern from DirectRestore.cpp:511-519.
        if (auto* o2av = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                RE::BSFixedString("Oxygen"))) {
            data.o2     = player->GetActorValue(*o2av);
            data.o2Max  = player->GetPermanentActorValue(*o2av);
            data.o2Base = player->GetBaseActorValue(*o2av);   // Phase C: the value DirectRestore restores
        }
        // v3 (A Phase 1) - level-derived survivability BASE (no mods), mirrors
        // DirectRestore's Health/CarryWeight capture so the profile is complete.
        if (auto* hp = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                RE::BSFixedString("Health"))) {
            data.healthBase = player->GetBaseActorValue(*hp);
        }
        if (auto* cw = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                RE::BSFixedString("CarryWeight"))) {
            data.carryWeightBase = player->GetBaseActorValue(*cw);
        }

        // Phase C0 - perks/skills (the real "progress") + unspent skill points, so
        // the DB profile is a COMPLETE restore source for Phase C. SEH-isolated walk
        // fills a POD buffer; T34 plugin encoding (reserved) happens here outside __try.
        {
            std::vector<PerkPair> perkPairs;
            if (SafeWalkPerks(player, perkPairs)) {
                data.perks.reserve(perkPairs.size());
                for (const auto& pp : perkPairs) {
                    ProfilePerk pk;
                    pk.formID = pp.formID;
                    pk.rank   = pp.rank;
                    data.perks.push_back(std::move(pk));
                }
            }
            std::uint32_t sp = 0;
            if (SafeReadSkillPoints(player, sp)) data.skillPoints = sp;
        }

        // Powers (gap #2) - capture the player's power-type spells (Starborn/lesser/
        // voice powers). Deduped (the array can briefly hold transient duplicates).
        {
            std::vector<std::uint32_t> pw;
            if (SafeWalkPowers(player, pw)) {
                std::unordered_set<std::uint32_t> seen;
                for (auto fid : pw) if (seen.insert(fid).second) data.powers.push_back(fid);
            }
        }

        // Research (gap #1) - capture completed research projects (state==2). The
        // walk returns total map nodes visited; data.research holds the completed
        // formIDs (deduped - the map is unique-keyed, but guard anyway).
        {
            std::vector<std::uint32_t> done;
            const int totalNodes = SafeWalkResearch(done);
            if (totalNodes > 0) {
                std::unordered_set<std::uint32_t> seen;
                for (auto fid : done) if (seen.insert(fid).second) data.research.push_back(fid);
            }
        }

        // V3 - capture the player's character level (instrumentation step: logged
        // so we can confirm GetNPC()->GetLevel() returns the real level before
        // building level persistence). Capture-only; the dbAuth delta-merge keeps
        // the DB baseline level (co-op live level = host's, not applied - option A).
        data.playerLevel = SafeReadPlayerLevel(player);

        // V3 - XP is the "Experience" ActorValue (verified via `player.modav
        // experience`). Read like O2. Captured as a co-op delta (live − +10s
        // snapshot cancels the host body's XP).
        if (auto* xpav = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                RE::BSFixedString("Experience"))) {
            data.xp = player->GetActorValue(*xpav);
        }
        PLOG_INFO("[PROFILE] V3 level/xp read: level={} xp={:.0f}"sv,
                  data.playerLevel, data.xp);

        PLOG_INFO("[PROFILE] Captured: {} items, {} perks, {} powers, {} research, skillPts={}, credits={}, o2={:.1f}/{:.1f} hp={:.1f} cw={:.1f} ({} equipped)"sv,
                  data.inventory.size(), data.perks.size(), data.powers.size(), data.research.size(),
                  data.skillPoints, data.credits, data.o2, data.o2Max,
                  data.healthBase, data.carryWeightBase, equippedForms.size());

        // Phase C3 - DB-AUTHORITATIVE gains delta-capture. Don't upload the raw
        // live capture (it drifts: restore artifacts on stackables, host credits).
        // Instead start from the frozen DB baseline and apply only the session
        // delta (live − post-restore snapshot): new looted items with faithful
        // mods, ammo/credit deltas. perks/AVs/level stay = DB baseline (V2 adds
        // perk/level gains). Existing items/mods/credits never drift.
        if (g_dbBaselineValid &&
            Pulsar::InventorySync::DirectRestore::IsDbAuthoritative()) {
            std::unordered_map<uint32_t, uint32_t> snap;
            uint32_t snapCredits = 0;
            uint32_t snapLevel   = 0;
            float    snapXP      = 0.0f;
            if (Pulsar::InventorySync::DirectRestore::GetGainsSnapshot(snap, snapCredits, snapLevel, snapXP)) {
                ProfileData merged = g_dbBaseline;          // frozen faithful DB profile
                const size_t baseN = merged.inventory.size();
                // #poison-guard - exclude host-exclusive forms recorded across this
                // session's bridges, so a leaked host item never uploads as a gain.
                std::unordered_set<uint32_t> foreignForms;
                Pulsar::InventorySync::DirectRestore::GetForeignItemSet(foreignForms);
                merged.inventory = DeltaMergeInventory(
                    g_dbBaseline.inventory, data.inventory, snap, foreignForms);
                // #poison-trace (overnight 2026-06-24) - ALWAYS log upload provenance
                // with the host-item discrepancy so a poison attempt is visible in real
                // time WITH a timestamp. foreignInLive = host-exclusive forms currently
                // sitting in the client's LIVE inventory: any >0 is the exact moment a
                // host item leaked in (the foreign-guard excludes it from the upload,
                // but this pinpoints WHEN, so we don't have to watch closely).
                {
                    std::size_t foreignInLive = 0;
                    std::uint32_t firstForeign = 0;
                    for (const auto& L : data.inventory) {
                        if (foreignForms.count(L.formID)) {
                            if (foreignInLive == 0) firstForeign = L.formID;
                            ++foreignInLive;
                        }
                    }
                    if (foreignInLive > 0) {
                        PLOG_WARN("[PROFILE][load-trace] DISCREPANCY at upload: {} HOST "
                                  "item(s) in live inventory (e.g. 0x{:08X}) - EXCLUDED "
                                  "from DB by foreign-guard [baseline={} live={} merged={} "
                                  "foreignSet={}] [#poison]"sv,
                                  foreignInLive, firstForeign, baseN,
                                  data.inventory.size(), merged.inventory.size(),
                                  foreignForms.size());
                    } else {
                        PLOG_INFO("[PROFILE][load-trace] clean upload - no host items in "
                                  "live [baseline={} live={} merged={} foreignSet={}]"sv,
                                  baseN, data.inventory.size(), merged.inventory.size(),
                                  foreignForms.size());
                    }
                }
                const int creditDelta =
                    static_cast<int>(data.credits) - static_cast<int>(snapCredits);
                const int mc = static_cast<int>(g_dbBaseline.credits) + creditDelta;
                merged.credits = static_cast<uint32_t>(mc > 0 ? mc : 0);
                // O2/AV sanity - keep the DB baseline's O2 UNLESS the live value is
                // plausibly healthy (suit applied, not a mid-restore collapse ~5).
                // Never persists a collapsed O2; a healthy live value recovers a
                // previously-bad baseline.
                constexpr float kO2SaneMin = 30.0f;
                if (data.o2Max  > kO2SaneMin) merged.o2Max  = data.o2Max;
                if (data.o2     > kO2SaneMin) merged.o2     = data.o2;
                if (data.o2Base > kO2SaneMin) merged.o2Base = data.o2Base;
                // V2 - capture perk + skill-point GAINS. Perks/skillPts are restored
                // to the client's values (ApplyPerksReplace + skillPts seed), so the
                // live capture = client's + co-op gains, and taking live is safe.
                // ANTI-POISON FIX (2026-06-19): this ONLY holds when the DB baseline
                // already had perks - then ApplyPerksReplace actually set them. If the
                // baseline is EMPTY (incomplete/partial DB profile), ApplyPerksReplace
                // KEEPS the host's perks (its "snapshot EMPTY → don't strip" safety),
                // so the live perks are the HOST'S - taking them would upload the host's
                // perks into the client's DB (observed: a 0-perk profile absorbed the
                // host's 27 perks). So require a non-empty baseline before taking live;
                // a perk-less profile must be re-bootstrapped solo, not filled in co-op.
                // #poison-guard (overnight 2026-06-24) - drop HOST-exclusive perks
                // before taking the live perk set, exactly like items/powers. The
                // anti-poison fix above only covers the empty-baseline case; a host
                // perk that lingers in live after ApplyPerksReplace (non-empty
                // baseline) would still be taken as ours. The foreign-perk set
                // (recorded at each bridge) excludes those. A genuine co-op-gained
                // perk is NOT foreign, so it still persists.
                std::unordered_set<uint32_t> foreignPerks;
                Pulsar::InventorySync::DirectRestore::GetForeignPerkSet(foreignPerks);
                std::vector<ProfilePerk> clientPerks;
                clientPerks.reserve(data.perks.size());
                std::size_t droppedHostPerks = 0;
                for (const auto& p : data.perks) {
                    if (foreignPerks.count(p.formID)) { ++droppedHostPerks; continue; }
                    clientPerks.push_back(p);
                }
                if (droppedHostPerks)
                    PLOG_WARN("[PROFILE][load-trace] DISCREPANCY: dropped {} HOST perk(s) "
                              "from DB upload (foreign-perk guard) [#poison]"sv,
                              droppedHostPerks);
                bool tookPerks = false;
                if (!g_dbBaseline.perks.empty() &&
                    !clientPerks.empty() &&
                    clientPerks.size() >= g_dbBaseline.perks.size()) {
                    merged.perks       = std::move(clientPerks);
                    merged.skillPoints = data.skillPoints;
                    tookPerks = true;
                }
                // V3 - level DELTA (capture-only, option A). In co-op the live level
                // is the host body's (e.g. 4), same as the +10s snapshot - so
                // (live − snap) = the level-ups hall earned THIS session (host value
                // cancels, like credits). Add that to hall's baseline. Clamp >= 0 and
                // never below baseline (level doesn't drop). NOTE: hall's baseline
                // level is only correct after a bootstrap (solo, empty DB); for an
                // established DB it tracks gains from the stored baseline.
                const int lvlDelta =
                    static_cast<int>(data.playerLevel) - static_cast<int>(snapLevel);
                const int newLvl = static_cast<int>(g_dbBaseline.playerLevel) +
                                   (lvlDelta > 0 ? lvlDelta : 0);
                merged.playerLevel = static_cast<uint32_t>(newLvl);
                // V3 - XP DELTA, same logic (Experience AV; co-op live = host body's
                // XP = snapshot → cancels; (live − snap) = hall's session XP gain).
                const float xpDelta = data.xp - snapXP;
                merged.xp = g_dbBaseline.xp + (xpDelta > 0.0f ? xpDelta : 0.0f);
                // Powers (gap #2) - SET-DELTA. merged starts = baseline powers; add
                // only powers gained THIS session (live − +10s snapshot). Powers in
                // the snapshot (the loaded/foreign save's, re-applied baseline) cancel
                // → foreign powers never poison the DB; new temple powers persist.
                std::size_t powerGain = 0;
                {
                    std::vector<std::uint32_t> snapPowers;
                    if (Pulsar::InventorySync::DirectRestore::GetGainsSnapshotPowers(snapPowers)) {
                        std::unordered_set<std::uint32_t> snapSet(snapPowers.begin(), snapPowers.end());
                        std::unordered_set<std::uint32_t> have(merged.powers.begin(), merged.powers.end());
                        for (auto fid : data.powers) {
                            if (!snapSet.count(fid) && have.insert(fid).second) {
                                merged.powers.push_back(fid);
                                ++powerGain;
                            }
                        }
                    }
                }
                // Research (gap #1) - SET-DELTA, same logic as powers. merged starts =
                // baseline research; add only projects researched THIS session
                // (live − +10s snapshot). Foreign/host save's research (in the snapshot)
                // cancels → never poisons the DB; session research persists. Monotonic.
                std::size_t researchGain = 0;
                {
                    std::vector<std::uint32_t> snapResearch;
                    if (Pulsar::InventorySync::DirectRestore::GetGainsSnapshotResearch(snapResearch)) {
                        std::unordered_set<std::uint32_t> snapSet(snapResearch.begin(), snapResearch.end());
                        std::unordered_set<std::uint32_t> have(merged.research.begin(), merged.research.end());
                        for (auto fid : data.research) {
                            if (!snapSet.count(fid) && have.insert(fid).second) {
                                merged.research.push_back(fid);
                                ++researchGain;
                            }
                        }
                    }
                }
                PLOG_INFO("[PROFILE] gains delta-capture: baseline {} -> {} items "
                          "(live {}), credits {}{:+d}={}, perks {}->{} skillPts {}->{} ({}), "
                          "level {}{:+d}={} (snap={} live={}), xp {:.0f}+{:.0f}={:.0f}, "
                          "powers {}->{} (+{} this session)"sv,
                          baseN, merged.inventory.size(), data.inventory.size(),
                          g_dbBaseline.credits, creditDelta, merged.credits,
                          g_dbBaseline.perks.size(), merged.perks.size(),
                          g_dbBaseline.skillPoints, merged.skillPoints,
                          tookPerks ? "live" : "baseline",
                          g_dbBaseline.playerLevel, (lvlDelta > 0 ? lvlDelta : 0),
                          merged.playerLevel, snapLevel, data.playerLevel,
                          g_dbBaseline.xp, (xpDelta > 0.0f ? xpDelta : 0.0f), merged.xp,
                          g_dbBaseline.powers.size(), merged.powers.size(), powerGain);
                PLOG_INFO("[PROFILE] gains delta-capture: research {}->{} (+{} this session)"sv,
                          g_dbBaseline.research.size(), merged.research.size(), researchGain);
                return merged;
            }
            // dbAuth but the +10s snapshot isn't taken yet - NEVER full-capture in
            // dbAuth (would drift). Return the frozen DB baseline unchanged
            // (idempotent); LobbyHandoff also gates the upload on snapshot validity.
            PLOG_INFO("[PROFILE] gains delta-capture: snapshot not ready -> frozen DB baseline"sv);
            return g_dbBaseline;
        }
        return data;
    }

    ProfileData SnapshotFromPlayer()
    {
        return CaptureProfile();
    }

    // ─── v2 apply ───

    // Re-equip helper - mirrors DirectRestore.cpp:86-101 (verified REL::IDs for 1.16.236).
    static bool ProfileEquipForm(RE::Actor* a_actor, RE::TESBoundObject* a_form)
    {
        if (!a_actor || !a_form) return false;
        static REL::Relocation<RE::ActorEquipManager**> singleton{ REL::ID(938503) };
        auto* mgr = *singleton;
        if (!mgr) return false;
        using EquipFn = bool(*)(RE::ActorEquipManager*, RE::Actor*,
            const RE::BGSObjectInstance&, const RE::BGSEquipSlot*,
            bool, bool, bool, bool, bool);
        static REL::Relocation<EquipFn> equipFn{ REL::ID(101949) };
        RE::BGSObjectInstance inst(a_form, nullptr);
        return equipFn(mgr, a_actor, inst, nullptr, false, true, false, true, false);
    }

    // FormID -> TESBoundObject*. Do NOT use LookupByID<TESBoundObject>: it calls
    // As<TESBoundObject>() which does an EXACT GetFormType()==FORMTYPE match, and
    // TESBoundObject is an abstract base (weapons are kWEAP, armor kARMO, ...) so
    // it ALWAYS returns null. Use the non-templated lookup + IsBoundObject() cast.
    static RE::TESBoundObject* LookupBound(uint32_t fid)
    {
        if (fid == 0) return nullptr;
        auto* form = RE::TESForm::LookupByID(fid);
        return (form && form->IsBoundObject()) ? static_cast<RE::TESBoundObject*>(form) : nullptr;
    }

    // Resolve a profile item to a live bound object: prefer T34 (plugin+localID),
    // fall back to the stored raw formID (correct on the same machine).
    static RE::TESBoundObject* ResolveItemBound(const ProfileItem& it)
    {
        if (!it.plugin.empty()) {
            uint32_t fid = Pulsar::NPCSync::PluginLookup::ResolveCrossPlugin(it.plugin.c_str(), it.localID);
            if (auto* b = LookupBound(fid)) return b;
        }
        return LookupBound(it.formID);  // same-machine fallback
    }

    static int32_t CurrentCount(RE::PlayerCharacter* player, uint32_t formID)
    {
        int32_t total = 0;
        player->ForEachInventoryItem([&](const RE::BGSInventoryItem& a_item) -> RE::BSContainer::ForEachResult {
            if (a_item.object && a_item.object->GetFormID() == formID) {
                for (auto& s : a_item.stacks) total += static_cast<int32_t>(s.count);
                return RE::BSContainer::ForEachResult::kStop;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        return total;
    }

    bool ApplyProfileData(const ProfileData& a_data)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            PLOG_WARN("[PROFILE] ApplyProfileData: no player"sv);
            return false;
        }

        // 2026-06-18 - save-identity check. Is the loaded save the client's OWN
        // character, or a foreign/HOST save (e.g. the co-op auto-save the engine
        // makes on leaving = the host's bridged character)? Compare the loaded
        // character's Health base to the DB profile's healthBase. Mismatch →
        // foreign → tell DirectRestore to keep the DB seed (the client's loadout)
        // as the restore source instead of capturing the host items, AND to skip
        // uploading this save to the DB. Match (or no signature) → own save →
        // save-authoritative (the live capture wins, as confirmed working).
        {
            bool isOwn = true;
            if (a_data.healthBase > 1.0f) {
                if (auto* hp = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                        RE::BSFixedString("Health"))) {
                    const float liveBase = player->GetBaseActorValue(*hp);
                    float d = liveBase - a_data.healthBase;
                    if (d < 0.0f) d = -d;
                    isOwn = d < 10.0f;
                    PLOG_INFO("[PROFILE] save-identity: live hpBase={:.1f} profile hpBase={:.1f} -> {}"sv,
                              liveBase, a_data.healthBase,
                              isOwn ? "OWN save (save-authoritative)"sv
                                    : "FOREIGN/host save (DB seed authoritative)"sv);
                }
            }
            Pulsar::InventorySync::DirectRestore::SetLoadedSaveIsOwn(isOwn);
        }

        // Suppress M.2 inventory broadcast while we mutate (same guard DirectRestore uses).
        const bool prevReplay = Pulsar::WorldSync::Loot::SetReplayingFromPacket(true);

        int skipped = 0;
        // A unification Phase 4 - collapse the two item-apply paths. ClientProfile
        // NO LONGER additively writes items to the live player. That path merged
        // the DB profile INTO the live save on join (AddObjectToContainer per item),
        // and the clean-capture then re-uploaded the union → the DB profile grew
        // unbounded and accumulated host weapons EVERY session (observed 2026-06-18:
        // a fresh "Cutter only" save came back as 26 items incl. 3 host weapons with
        // engine-rolled random mods). Items are now owned SOLELY by DirectRestore:
        // the seed below feeds its cache, and the per-bridge replace-restore is the
        // single place items land on the live player. Here we ONLY translate the
        // profile into that clean seed - no live mutation, so the fresh save stays
        // exactly as loaded and the clean-capture uploads the TRUE inventory.
        std::vector<Pulsar::InventorySync::DirectRestore::SeedItem> seed;
        seed.reserve(a_data.inventory.size());
        for (const auto& it : a_data.inventory) {
            RE::TESBoundObject* bound = ResolveItemBound(it);
            if (!bound) {
                ++skipped;
                PLOG_WARN("[PROFILE] apply: unresolved form plugin={} local=0x{:X}"sv,
                          it.plugin, it.localID);
                continue;
            }
            // Phase B  - carry the item's M.2 mod array so the bridge
            // restore can re-apply it (DB-restored items aren't naked base forms).
            seed.push_back({ bound, it.count, it.equipped, it.mods, it.modSentinel });
        }

        // Credits - capture is reliable, but APPLY is gated off: AddObjectToContainer
        // does NOT drive money (DirectRestore.cpp:678 - null-extra add zeroed credits).
        // Credits stay in the blob; re-enable once a proper money API is RE'd.
        if (kApplyCreditsOnRestore) {
            const int32_t have = CurrentCount(player, kCreditsFormID);
            const int32_t delta = static_cast<int32_t>(a_data.credits) - have;
            if (delta != 0) {
                if (auto* creditsForm = LookupBound(kCreditsFormID)) {
                    player->AddObjectToContainer(creditsForm, nullptr,
                        static_cast<std::int32_t>(delta), nullptr,
                        RE::ITEM_TRANSFER_REASON::kNone);
                }
            }
        }

        // AV apply (O2/O2Max/Health/CarryWeight) REMOVED 2026-06-18.
        // Save-authoritative: the survivability AVs come from DirectRestore's
        // LIVE pre-bridge capture of the client's own save, NOT the DB profile.
        // Applying them here injected a STALE/inflated DB value BEFORE that
        // capture - the O2 case grew every session (525→530→…): ClientProfile
        // set O2 to the DB's o2Max, the capture grabbed the inflated value, the
        // clean-capture re-uploaded it bigger, repeat (an O2 accumulating-poison
        // loop; client showed 530 O2 while the real save had ~125 like the host).
        // DirectRestore restores Health/CarryWeight/O2 BASE from the live capture;
        // this path is redundant + harmful, so it's gone. (Credits apply above
        // stays gated off; the seed below stays - items fallback only.)

        // Phase C - DB-AUTHORITATIVE restore. When the toggle is on AND the DB
        // profile is non-empty, the DB is the restore source for THIS join: seed
        // the AV baselines + perks + skill points so RestoreClientState applies the
        // DB's progression (not the loaded save's), and flag DirectRestore so
        // CaptureClientState is suppressed (the seeds survive the bridge). This is
        // what makes co-op gains survive loading ANY save. Empty DB → dbAuth false
        // → bootstrap (legacy save-authoritative path captures the loaded save and
        // the clean-capture uploads it to fill the DB).
        const bool dbAuth =
            Pulsar::LoadConfig().dbAuthoritative && !a_data.inventory.empty();
        Pulsar::InventorySync::DirectRestore::SetDbAuthoritative(dbAuth);
        if (dbAuth) {
            Pulsar::InventorySync::DirectRestore::SeedAVsFromProfile(
                a_data.o2Base, a_data.o2Max, a_data.healthBase, a_data.carryWeightBase);
            Pulsar::InventorySync::DirectRestore::SeedCreditsFromProfile(a_data.credits);
            std::vector<std::pair<uint32_t, int8_t>> perkPairs;
            perkPairs.reserve(a_data.perks.size());
            for (const auto& pk : a_data.perks)
                perkPairs.push_back({ pk.formID, pk.rank });
            Pulsar::InventorySync::DirectRestore::SeedPerksFromProfile(
                perkPairs, a_data.skillPoints);
            // Powers (gap #2) - seed the DB's powers for additive re-apply.
            Pulsar::InventorySync::DirectRestore::SeedPowersFromProfile(a_data.powers);
            // Research (gap #1) - seed the DB's completed projects for the unlock replay.
            Pulsar::InventorySync::DirectRestore::SeedResearchFromProfile(a_data.research);
            // Phase C3 - stash the authoritative DB profile as the gains-capture
            // merge base (frozen, faithful). The capture adds only the session
            // delta on top of this.
            g_dbBaseline      = a_data;
            g_dbBaselineValid = true;
            PLOG_INFO("[PROFILE] DB-AUTHORITATIVE join: DB is the restore source "
                      "({} items, {} perks, {} powers, {} research, skillPts={})"sv,
                      a_data.inventory.size(), a_data.perks.size(),
                      a_data.powers.size(), a_data.research.size(), a_data.skillPoints);
        } else {
            g_dbBaselineValid = false;   // bootstrap/legacy → full-capture path
        }

        // A unification Phase 3 - seed the DirectRestore cache from THIS clean DB
        // profile so the per-bridge restore sources host-free items (Bug A). Done
        // after apply so the live inventory + the seed agree at this point.
        Pulsar::InventorySync::DirectRestore::SeedCacheFromProfile(seed);

        Pulsar::WorldSync::Loot::SetReplayingFromPacket(prevReplay);
        PLOG_INFO("[PROFILE] Applied (AVs + seed-only; items via DirectRestore): {} seeded, {} skipped"sv,
                  seed.size(), skipped);
        return skipped == 0;
    }

    ///re-apply the cached DB profile after a LOCAL client reload (both-down
    // game-over reload). RequestApply/DoApply alone fails there: the DirectRestore cache
    // is already EMPTY by then (a prior bridge restore consumed it), so RestoreClientState
    // skips. This re-SEEDS the cache from the frozen DB baseline (ApplyProfileData ->
    // SeedCacheFromProfile + the save-identity check that flags the loaded host save as
    // FOREIGN) and then runs RestoreClientState to wipe the host items and restore the
    // client's DB loadout. Frozen-baseline is correct for both-down: the reload already
    // reverted any session gains, so the join loadout is the right recovery. Returns false
    // if no dbAuth baseline exists this session (bootstrap/legacy save).
    bool ReapplyCachedDbProfile()
    {
        // Prefer the LATEST clean snapshot (has within-session progress); fall back to the
        // frozen DB join-baseline only if no clean capture has happened yet this session.
        const ProfileData* src = nullptr;
        const char*        srcName = "";
        if (g_lastCleanBaselineValid && !g_lastCleanBaseline.inventory.empty()) {
            src = &g_lastCleanBaseline; srcName = "latest clean snapshot";
        } else if (g_dbBaselineValid)          { src = &g_dbBaseline; srcName = "DB join-baseline"; }
        if (!src || src->inventory.empty()) {
            PLOG_INFO("[PROFILE] ReapplyCachedDbProfile: no recovery baseline this session - skip"sv);
            return false;
        }
        PLOG_INFO("[PROFILE] ReapplyCachedDbProfile - re-seed + restore from {} after local reload "
                  "({} items, {} perks)"sv, srcName, src->inventory.size(), src->perks.size());
        ProfileData cached = *src;   // copy: ApplyProfileData reassigns g_dbBaseline
        ApplyProfileData(cached);    // re-seeds DirectRestore cache + dbAuth + save-identity
        Pulsar::InventorySync::DirectRestore::RestoreClientState();  // wipe host items + restore loadout
        return true;
    }

    // ─── v2 convenience wrappers (the blob contract for Teil 2) ───

    std::vector<uint8_t> SerializeProfile()
    {
        ProfileData d = CaptureProfile();
        // O2-collapse guard - never persist a suit-less / mid-restore-churn
        // collapsed O2 (o2Max ~5). For a BOOTSTRAP (non-dbAuth, no baseline to fall
        // back to) skip the upload entirely; the periodic capture retries once O2
        // has settled (suit applied). This is what stops a collapsed o2Base=0 from
        // getting baked into a fresh DB profile. (dbAuth captures already keep the
        // baseline O2 on collapse, in CaptureProfile.)
        if (!Pulsar::InventorySync::DirectRestore::IsDbAuthoritative() &&
            !d.inventory.empty() && d.o2Max < 30.0f) {
            PLOG_WARN("[PROFILE] SerializeProfile: O2 collapsed (o2Max={:.1f}) - "
                      "skip bootstrap upload, retry when settled"sv, d.o2Max);
            return {};
        }

        // ── POISON-GUARD + PROVENANCE-TRACE (2026-06-21) ───────────────
        // Root-cause of "client DB holds the HOST's items": the BOOTSTRAP upload
        // (empty DB → !dbAuth) serialises the LIVE inventory. After a co-op
        // bridge the live inventory is the HOST's items (the bridge copies them
        // for co-location, and in bootstrap there is no DB loadout for
        // RestoreClientState to wipe-and-replace them with). Uploading that
        // poisons the client's DB profile; once dbAuth latches on the poisoned
        // profile the gains-delta re-persists it forever.
        //
        // GUARD: never serialise a bootstrap profile once a bridge has happened
        // this session - the DB may only be seeded from a clean, own, pre-bridge
        // (solo) state. TRACE: log the full provenance of EVERY upload so a
        // future leak is attributable to the exact state that produced it.
        {
            const bool dbAuth_      = Pulsar::InventorySync::DirectRestore::IsDbAuthoritative();
            const bool bridged_     = Pulsar::InventorySync::DirectRestore::HasBridgedThisSession();
            const bool ownSave_     = Pulsar::InventorySync::DirectRestore::LoadedSaveIsOwn();
            const bool restoreDone_ = Pulsar::InventorySync::DirectRestore::RestoreComplete();
            uint32_t eq0 = 0, eq1 = 0;
            int eqn = 0;
            for (const auto& it : d.inventory) {
                if (!it.equipped) continue;
                if (eqn == 0) eq0 = it.formID;
                else if (eqn == 1) eq1 = it.formID;
                ++eqn;
            }
            PLOG_INFO("[PROFILE][poison-trace] upload provenance: mode={} bridged={} ownSave={} "
                      "restoreDone={} items={} equipped={} eq0=0x{:08X} eq1=0x{:08X} credits={}"sv,
                      dbAuth_ ? "dbAuth" : "BOOTSTRAP", bridged_, ownSave_, restoreDone_,
                      d.inventory.size(), eqn, eq0, eq1, d.credits);
            if (!dbAuth_ && bridged_ && !d.inventory.empty()) {
                PLOG_WARN("[PROFILE][POISON-GUARD] REFUSED upload - bootstrap(!dbAuth) AND bridged "
                          "this session: live inventory is the HOST's bridged items, not the "
                          "client's own. Seed the DB from a clean SOLO load instead. (items={})"sv,
                          d.inventory.size());
                return {};
            }
        }

        ///keep g_lastCleanBaseline = the LATEST clean capture (the both-down
        // recovery source). Updated on every clean capture so within-session progress
        // (items/level/XP/skills/powers) survives a both-down game-over reload. Gate: non-empty,
        // neither player downed, and NOT in the post-save-load poison window (where the live
        // state is the host-bridged save). Works in both bootstrap and dbAuth.
        if (!d.inventory.empty() &&
            !Pulsar::Coop::Mortality::IsLocalDowned() &&
            !Pulsar::Coop::Mortality::IsRemoteDowned() &&
            NowMs() >= g_suppressCleanCacheUntilMs) {
            const bool first = !g_lastCleanBaselineValid;
            g_lastCleanBaseline      = d;
            g_lastCleanBaselineValid = true;
            if (first) {
                PLOG_INFO("[PROFILE] clean-snapshot baseline armed for both-down recovery "
                          "({} items, {} perks, lvl {})"sv,
                          d.inventory.size(), d.perks.size(), d.playerLevel);
            }
        }
        return EncodeProfile(d);
    }

    bool ApplyProfile(std::span<const uint8_t> a_blob)
    {
        ProfileData d;
        if (!DecodeProfile(a_blob, d)) {
            PLOG_WARN("[PROFILE] ApplyProfile: decode failed"sv);
            return false;
        }
        return ApplyProfileData(d);
    }

    void RunInGameRoundTripTest()
    {
        ProfileData before = CaptureProfile();
        std::vector<uint8_t> blob = EncodeProfile(before);
        ProfileData after;
        const bool decoded = DecodeProfile(blob, after);
        const bool match =
            decoded &&
            after.inventory.size() == before.inventory.size() &&
            after.credits == before.credits;
        PLOG_INFO("[PROFILE-TEST] in-game round-trip {}: items {}->{}, credits {}->{}, blob={} bytes"sv,
                  match ? "PASS" : "FAIL",
                  before.inventory.size(), after.inventory.size(),
                  before.credits, after.credits, blob.size());
        // Apply the decoded blob back (idempotent: additive converge to the same counts).
        ApplyProfileData(after);
    }

    // ─── F10 = save loadout snapshot, F11 = restore it (real restore demo) ───

    static std::vector<uint8_t> g_snapshotBlob;

    void SaveProfileBlobTest()
    {
        g_snapshotBlob = SerializeProfile();
        PLOG_INFO("[PROFILE-TEST] F10 saved snapshot: {} bytes"sv, g_snapshotBlob.size());
    }

    void RestoreProfileBlobTest()
    {
        if (g_snapshotBlob.empty()) {
            PLOG_WARN("[PROFILE-TEST] F11 restore: no snapshot yet - press F10 first"sv);
            return;
        }
        PLOG_INFO("[PROFILE-TEST] F11 restoring snapshot: {} bytes"sv, g_snapshotBlob.size());
        ApplyProfile(g_snapshotBlob);
    }

}  // namespace Pulsar::Profile
