// Background-trait NPC aliases for the joining player (include/Coop/TraitAliases.h).

#include "TestHarness.h"

#include "Coop/TraitAliases.h"

#include <string>

using namespace Pulsar::Coop::TraitAliases;

namespace {
    std::size_t Index(const char* name)
    {
        for (std::size_t i = 0; i < kAliases.size(); ++i)
            if (std::string(kAliases[i].name) == name) return i;
        return 99;
    }
}

TEST(TraitAliases, ReligionsEachKeepTheirOwnKeeper)
{
    Traits universal;   universal.universal = true;
    Traits enlightened; enlightened.enlightened = true;
    CHECK(Needs(Index("KeeperAquilus"), universal));
    CHECK(!Needs(Index("AndySingh"), universal));
    CHECK(Needs(Index("AndySingh"), enlightened));
    CHECK(!Needs(Index("KeeperAquilus"), enlightened));
}

TEST(TraitAliases, ExtrovertIntrovertTaskmasterKeepBothKeepers)
{
    for (int k = 0; k < 3; ++k) {
        Traits t;
        if (k == 0) t.extrovert = true;
        if (k == 1) t.introvert = true;
        if (k == 2) t.taskmaster = true;
        CHECK(Needs(Index("KeeperAquilus"), t));
        CHECK(Needs(Index("AndySingh"), t));
    }
}

TEST(TraitAliases, OtherBackgroundsMapToTheirNpc)
{
    Traits t; t.serpent = true; t.neon = true; t.fc = true; t.uc = true;
    CHECK(Needs(Index("Mirza"), t));
    CHECK(Needs(Index("MadameSauvage"), t));
    CHECK(Needs(Index("MayorCartwright_FC"), t));
    CHECK(Needs(Index("AdministratorRookes_UC"), t));
    Traits none;
    for (std::size_t i = 0; i < kAliases.size(); ++i) CHECK(!Needs(i, none));
}

TEST(TraitAliases, TableMatchesTheScriptsVariableOrder)
{
    // QF_TraitQuest_001687DB variables (decompiled): 5 AndySingh, 7 KeeperAquilus,
    // 8 Mirza, 9 MadameSauvage, 10 AdministratorRookes_UC, 11 MayorCartwright_FC.
    CHECK_EQ(kAliases[Index("AndySingh")].varIndex, 5u);
    CHECK_EQ(kAliases[Index("KeeperAquilus")].varIndex, 7u);
    CHECK_EQ(kAliases[Index("Mirza")].varIndex, 8u);
    CHECK_EQ(kAliases[Index("MadameSauvage")].varIndex, 9u);
    CHECK_EQ(kAliases[Index("AdministratorRookes_UC")].varIndex, 10u);
    CHECK_EQ(kAliases[Index("MayorCartwright_FC")].varIndex, 11u);
}
