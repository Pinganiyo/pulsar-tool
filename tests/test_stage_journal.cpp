// Quest stages missed during a short disconnect (include/QuestSync/StageJournal.h).

#include "TestHarness.h"

#include "QuestSync/StageJournal.h"

using namespace Pulsar::QuestSync;

TEST(StageJournal, ReplaysOnlyTheGap)
{
    StageJournal j;
    j.Record(0xA, 10, 1000);
    j.Record(0xA, 20, 5000);   // after the disconnect at 4000
    j.Record(0xB, 5, 6000);
    const auto gap = j.Since(4000);
    REQUIRE(gap.size() == 2);
    CHECK_EQ(gap[0].quest, 0xAu);
    CHECK_EQ(gap[0].stage, uint16_t(20));
    CHECK_EQ(gap[1].quest, 0xBu);
}

TEST(StageJournal, KeepsOrderAndDropsRepeats)
{
    StageJournal j;
    j.Record(0xA, 10, 100);
    j.Record(0xA, 10, 110);    // engine re-fire
    j.Record(0xA, 20, 120);
    j.Record(0xA, 10, 130);    // set again later: replay once, at first position
    CHECK_EQ(j.Size(), size_t(3));
    const auto all = j.Since(0);
    REQUIRE(all.size() == 2);
    CHECK_EQ(all[0].stage, uint16_t(10));
    CHECK_EQ(all[1].stage, uint16_t(20));
}

TEST(StageJournal, ClientSkipsStagesItAlreadySet)
{
    StageJournal client;
    client.Record(0xA, 20, 5500);   // the client got there on its own during the gap
    CHECK(client.ContainsSince(0xA, 20, 4000));
    CHECK(!client.ContainsSince(0xA, 30, 4000));
    CHECK(!client.ContainsSince(0xA, 20, 6000));   // older than this gap
}

TEST(StageJournal, IsBounded)
{
    StageJournal j;
    for (std::uint32_t i = 0; i < StageJournal::kMaxEntries + 100; ++i)
        j.Record(i, 1, i);
    CHECK_EQ(j.Size(), StageJournal::kMaxEntries);
    CHECK(!j.ContainsSince(0, 1, 0));                       // oldest dropped
    CHECK(j.ContainsSince(StageJournal::kMaxEntries + 99, 1, 0));
}
