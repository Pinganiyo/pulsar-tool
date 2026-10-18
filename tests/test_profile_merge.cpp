// Co-op gains merge (src/Profile/ProfileMerge.cpp): what the client keeps from
// a session. A bug here loses or duplicates items in the saved character.

#include "TestHarness.h"

#include "Profile/ProfileMerge.h"

#include <algorithm>

using namespace Pulsar::Profile;

namespace {
    ProfileItem Item(uint32_t fid, int32_t count, bool equipped = false)
    {
        ProfileItem it;
        it.formID   = fid;
        it.count    = count;
        it.equipped = equipped;
        return it;
    }

    const ProfileItem* Find(const std::vector<ProfileItem>& v, uint32_t fid)
    {
        auto it = std::find_if(v.begin(), v.end(), [&](const ProfileItem& i) { return i.formID == fid; });
        return it == v.end() ? nullptr : &*it;
    }

    constexpr uint32_t kRifle = 0x100, kAmmo = 0x200, kMedpack = 0x300, kLoot = 0x400, kHostGun = 0x900;
}

TEST(ProfileMerge, NothingHappenedKeepsTheBaseline)
{
    const std::vector<ProfileItem> base = { Item(kRifle, 1, true), Item(kAmmo, 50) };
    const std::vector<ProfileItem> live = base;                  // restored 1:1
    const std::unordered_map<uint32_t, uint32_t> snap = { { kRifle, 1 }, { kAmmo, 50 } };
    const auto out = DeltaMergeInventory(base, live, snap, {});
    REQUIRE(out.size() == 2);
    CHECK_EQ(Find(out, kRifle)->count, 1);
    CHECK_EQ(Find(out, kAmmo)->count, 50);
}

TEST(ProfileMerge, GainsAndLossesMove)
{
    const std::vector<ProfileItem> base = { Item(kRifle, 1), Item(kAmmo, 50), Item(kMedpack, 3) };
    const std::unordered_map<uint32_t, uint32_t> snap = { { kRifle, 1 }, { kAmmo, 50 }, { kMedpack, 3 } };
    // Shot 20 ammo, used all medpacks, looted something new.
    const std::vector<ProfileItem> live = { Item(kRifle, 1), Item(kAmmo, 30), Item(kLoot, 2) };
    const auto out = DeltaMergeInventory(base, live, snap, {});
    CHECK_EQ(Find(out, kAmmo)->count, 30);
    CHECK(Find(out, kMedpack) == nullptr);   // consumed -> dropped
    REQUIRE(Find(out, kLoot) != nullptr);
    CHECK_EQ(Find(out, kLoot)->count, 2);
}

TEST(ProfileMerge, RestoreArtifactsCancelOut)
{
    // The snapshot holds a stray copy the restore left (e.g. host save had 5
    // extra ammo); live still has it. Only the real change (+10) counts.
    const std::vector<ProfileItem> base = { Item(kAmmo, 50) };
    const std::unordered_map<uint32_t, uint32_t> snap = { { kAmmo, 55 } };
    const std::vector<ProfileItem> live = { Item(kAmmo, 65) };
    const auto out = DeltaMergeInventory(base, live, snap, {});
    CHECK_EQ(Find(out, kAmmo)->count, 60);
}

TEST(ProfileMerge, HostItemsNeverEnterTheProfile)
{
    const std::vector<ProfileItem> base = { Item(kRifle, 1) };
    const std::unordered_map<uint32_t, uint32_t> snap = { { kRifle, 1 } };
    // The host's gun leaked back into live after the snapshot: would look like loot.
    const std::vector<ProfileItem> live = { Item(kRifle, 1), Item(kHostGun, 1, true) };
    const auto out = DeltaMergeInventory(base, live, snap, { kHostGun });
    CHECK(Find(out, kHostGun) == nullptr);
    CHECK_EQ(out.size(), size_t(1));
}

TEST(ProfileMerge, ForeignFormInBaselineIsNotMoved)
{
    // A foreign form the client genuinely owned keeps its baseline count.
    const std::vector<ProfileItem> base = { Item(kHostGun, 1) };
    const std::unordered_map<uint32_t, uint32_t> snap = { { kHostGun, 2 } };
    const std::vector<ProfileItem> live = {};
    const auto out = DeltaMergeInventory(base, live, snap, { kHostGun });
    REQUIRE(Find(out, kHostGun) != nullptr);
    CHECK_EQ(Find(out, kHostGun)->count, 1);
}

TEST(ProfileMerge, EquippedFollowsTheCurrentLoadout)
{
    const std::vector<ProfileItem> base = { Item(kRifle, 1, true), Item(kLoot, 1, false) };
    const std::unordered_map<uint32_t, uint32_t> snap = { { kRifle, 1 }, { kLoot, 1 } };
    const std::vector<ProfileItem> live = { Item(kRifle, 1, false), Item(kLoot, 1, true) };
    const auto out = DeltaMergeInventory(base, live, snap, {});
    CHECK(!Find(out, kRifle)->equipped);
    CHECK(Find(out, kLoot)->equipped);
}

TEST(ProfileMerge, MissingFromLiveButNotInSnapshotIsKept)
{
    // An item the restore failed to re-add is absent from BOTH live and the
    // snapshot: it must stay at the baseline count, not be lost.
    const std::vector<ProfileItem> base = { Item(kRifle, 1), Item(kMedpack, 4) };
    const std::unordered_map<uint32_t, uint32_t> snap = { { kRifle, 1 } };
    const std::vector<ProfileItem> live = { Item(kRifle, 1) };
    const auto out = DeltaMergeInventory(base, live, snap, {});
    REQUIRE(Find(out, kMedpack) != nullptr);
    CHECK_EQ(Find(out, kMedpack)->count, 4);
}

TEST(ProfileMerge, NewItemKeepsItsLiveMods)
{
    ProfileItem looted = Item(kLoot, 1);
    looted.mods = { 7, 8, 9 };
    looted.plugin = "Starfield.esm";
    const auto out = DeltaMergeInventory({}, { looted }, {}, {});
    REQUIRE(out.size() == 1);
    CHECK(out[0].mods == looted.mods);
    CHECK_EQ(out[0].plugin, looted.plugin);
}
