// Partner-counts-as-follower bookkeeping (include/Coop/FollowerBonus.h).
// The global is owned by SQ_FollowersScript, which overwrites it with the real
// (whole-number) count at any time; these check we never drift it.

#include "TestHarness.h"

#include "Coop/FollowerBonus.h"

using Pulsar::Coop::FollowerBonus;

namespace {
    // One tick against a simulated global; applies the write if any.
    void Step(bool want, float& global)
    {
        if (auto v = FollowerBonus::Tick(want, global)) global = *v;
    }

    // The vanilla TRAIT_CND_HasHumanFollower with Vasco's FOL_ActiveFollower.
    bool HasHumanFollower(float count, bool vascoActive)
    {
        return count >= 1.0f && (!vascoActive || count >= 2.0f);
    }
}

TEST(FollowerBonus, PartnerMakesTheTraitConditionTrue)
{
    float g = 0.0f;                                  // no recruited followers
    CHECK(!HasHumanFollower(g, false));
    Step(true, g);
    CHECK(HasHumanFollower(g, false));
    Step(true, g);
    CHECK_EQ(g, 1.5f);                               // no repeated writes
    Step(false, g);
    CHECK_EQ(g, 0.0f);                               // back to the real count
}

TEST(FollowerBonus, CountsOnTopOfVasco)
{
    float g = 1.0f;                                  // Vasco alone: condition false
    CHECK(!HasHumanFollower(g, true));
    Step(true, g);
    CHECK(HasHumanFollower(g, true));
    Step(false, g);
    CHECK_EQ(g, 1.0f);
}

TEST(FollowerBonus, ReappliesAfterTheScriptRecounts)
{
    float g = 0.0f;
    Step(true, g);
    g = 0.0f;                                        // SetValueInt(real count)
    Step(true, g);
    CHECK_EQ(g, 1.5f);
    g = 1.0f;                                        // a real follower was recruited
    Step(true, g);
    CHECK_EQ(g, 2.5f);                               // partner on top
    Step(false, g);
    CHECK_EQ(g, 1.0f);                               // the real follower stays counted
}

TEST(FollowerBonus, ScriptValueEqualToOursIsNotMistakenForOurs)
{
    // The case a +1 marker could not handle: we wrote 0+1, then the script
    // writes 1 for a newly recruited follower. With the .5 mark they differ.
    float g = 0.0f;
    Step(true, g);
    g = 1.0f;                                        // script recount
    Step(false, g);
    CHECK_EQ(g, 1.0f);                               // real follower kept
}

TEST(FollowerBonus, NeverChangesTheScriptsValue)
{
    float g = 3.0f;
    for (int i = 0; i < 5; ++i) Step(false, g);
    CHECK_EQ(g, 3.0f);
    Step(true, g);
    g = 2.0f;                                        // script recount meanwhile
    Step(false, g);
    CHECK_EQ(g, 2.0f);
}

TEST(FollowerBonus, LeftoverFromASaveIsRemoved)
{
    float g = 1.5f;                                  // saved while the partner was near
    Step(false, g);                                  // solo after loading it
    CHECK_EQ(g, 0.0f);
}

TEST(FollowerBonus, IntReadersSeeOneMore)
{
    // A script calling GetValueInt truncates: 1.5 -> 1, 2.5 -> 2.
    CHECK_EQ(static_cast<int>(1.5f), 1);
    CHECK_EQ(static_cast<int>(2.5f), 2);
}
