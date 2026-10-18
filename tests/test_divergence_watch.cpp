// Automatic regroup trigger (include/Coop/DivergenceWatch.h). A wrong "true"
// reloads both players' save, so most of these check that it stays quiet.

#include "TestHarness.h"

#include "Coop/DivergenceWatch.h"

#include <algorithm>

using Pulsar::Coop::DivergenceConfig;
using Pulsar::Coop::DivergenceWatch;
using Pulsar::Coop::SameCell;

namespace {
    constexpr std::uint32_t kShip  = 0x0012ABCDu;
    constexpr std::uint32_t kOther = 0x0034EF01u;

    // Feed one mismatch tick per second from a_from to a_to; count requests.
    int Run(DivergenceWatch& w, std::uint64_t a_from, std::uint64_t a_to, bool a_inSpace,
            std::uint32_t a_local, std::uint32_t a_remote)
    {
        int n = 0;
        for (std::uint64_t t = a_from; t <= a_to; t += 1000)
            if (w.Tick(t, a_inSpace, a_local, a_remote, t)) ++n;
        return n;
    }
}

TEST(DivergenceWatch, ProceduralCellsCountAsTheSamePlace)
{
    CHECK(SameCell(kShip, kShip));
    CHECK(SameCell(0xFF001234u, 0xFF00ABCDu));
    CHECK(!SameCell(kShip, kOther));
    CHECK(!SameCell(0xFF001234u, kShip));
}

TEST(DivergenceWatch, MatchingCellsNeverTrigger)
{
    DivergenceWatch w;
    CHECK_EQ(Run(w, 1000, 600000, true, kShip, kShip), 0);
}

TEST(DivergenceWatch, OnlyInSpace)
{
    DivergenceWatch w;
    // On the ground each player may legitimately be elsewhere (own fast travel).
    CHECK_EQ(Run(w, 1000, 600000, false, kShip, kOther), 0);
}

TEST(DivergenceWatch, MismatchMustPersistForTheDwell)
{
    DivergenceWatch w;
    CHECK_EQ(Run(w, 1000, 19000, true, kShip, kOther), 0);   // 18 s of mismatch
    CHECK(!w.Tick(20000, true, kShip, kShip, 20000));       // back in sync: resets
    CHECK_EQ(Run(w, 21000, 39000, true, kShip, kOther), 0); // dwell restarted
    CHECK(w.Tick(41000, true, kShip, kOther, 41000));       // 20 s reached
}

TEST(DivergenceWatch, CooldownAndSessionCap)
{
    DivergenceWatch w;
    // Permanent divergence for an hour: at most maxPerSession (3) requests,
    // spaced by at least the cooldown.
    int n = 0;
    std::uint64_t last = 0, minGap = ~0ull;
    for (std::uint64_t t = 1000; t <= 3600000; t += 1000) {
        if (w.Tick(t, true, kShip, kOther, t)) {
            if (last) minGap = std::min(minGap, t - last);
            last = t;
            ++n;
        }
    }
    CHECK_EQ(n, 3);
    CHECK(minGap >= 120000);
    w.ResetSession();
    CHECK_EQ(Run(w, 4000000, 4030000, true, kShip, kOther), 1);
}

TEST(DivergenceWatch, StaleOrMissingHostCellIsIgnored)
{
    DivergenceWatch w;
    int n = 0;
    for (std::uint64_t t = 10000; t <= 600000; t += 1000)
        if (w.Tick(t, true, kShip, kOther, 1000)) ++n;   // host cell 9+ s old
    CHECK_EQ(n, 0);
    CHECK_EQ(Run(w, 1000, 600000, true, kShip, 0), 0);  // never received
    CHECK_EQ(Run(w, 1000, 600000, true, 0, kOther), 0); // no local cell (loading)
}

TEST(DivergenceWatch, BridgeRestartsTheDwell)
{
    DivergenceWatch w;
    CHECK_EQ(Run(w, 1000, 15000, true, kShip, kOther), 0);
    w.OnBridged(15500);
    CHECK_EQ(Run(w, 16000, 30000, true, kShip, kOther), 0);   // 14 s since bridge
}
