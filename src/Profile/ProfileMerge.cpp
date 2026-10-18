// Co-op gains merge (see include/Profile/ProfileMerge.h). Game-free.

#include "PCH.h"
#include "Profile/ProfileMerge.h"

namespace Pulsar::Profile {

    // Delta-merge: result = DB baseline + (live − post-restore snapshot). The
    // snapshot carries the SAME restore artifacts + host credits as `live`, so
    // they cancel; only TRUE session gains/losses move. New distinct items arrive
    // with their faithful live mods; existing items take count deltas; equipped
    // reflects the current loadout.
    std::vector<ProfileItem> DeltaMergeInventory(
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
}
