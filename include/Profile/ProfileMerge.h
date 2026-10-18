#pragma once

// Co-op gains merge for the character profile. Game-free so it can be
// unit-tested (tests/test_profile_merge.cpp).

#include "Profile/ClientProfile.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pulsar::Profile {

    // result = DB baseline + (live - post-restore snapshot), per form.
    //   a_baseline: the profile this session restored FROM
    //   a_live:     the current inventory
    //   a_snap:     form -> count, taken ~10 s after the restore (it carries the
    //               same restore artifacts and host leftovers as a_live, so they
    //               cancel and only real session gains/losses move)
    //   a_foreign:  host-exclusive forms; never move a count or enter the result
    // Items whose merged count is <= 0 are dropped. Order is unspecified.
    std::vector<ProfileItem> DeltaMergeInventory(
        const std::vector<ProfileItem>& a_baseline,
        const std::vector<ProfileItem>& a_live,
        const std::unordered_map<uint32_t, uint32_t>& a_snap,
        const std::unordered_set<uint32_t>& a_foreign);
}
