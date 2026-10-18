// Character profile codec: ProfileData <-> JSON blob (src/Profile/ProfileCodec.cpp).
// This blob is what gets saved to the lobby DB and restored on join, so a codec
// bug means lost or wrong items/perks for the joining player.

#include "TestHarness.h"

#include "Profile/ClientProfile.h"
#include "Profile/ProfileJson.h"

#include <string>
#include <vector>

using namespace Pulsar::Profile;

namespace {

    ProfileData FullProfile()
    {
        ProfileData p;
        p.version         = 2;
        p.playerName      = "Captain Test";
        p.playerLevel     = 42;
        p.raceName        = "Human";
        p.gender          = 1;
        p.credits         = 98765;
        p.o2              = 87.5f;
        p.o2Max           = 125.0f;
        p.healthBase      = 310.0f;
        p.carryWeightBase = 150.0f;
        p.o2Base          = 100.0f;
        p.skillPoints     = 3;
        p.xp              = 12345.0f;
        p.perks           = { { 0x0024AB10u, 3, "", 0 }, { 0x0024AB20u, 1, "", 0 } };
        p.powers          = { 0x00012345u, 0x00054321u };
        p.research        = { 0x000AAAAAu };

        ProfileItem a;
        a.formID      = 0x0001A2B3;
        a.count       = 3;
        a.plugin      = "Starfield.esm";
        a.localID     = 0x1A2B3;
        a.equipped    = true;
        a.mods        = { 111u, 222u, 333u };
        a.modSentinel = 7;
        p.inventory.push_back(a);

        ProfileItem b;
        b.formID  = 0x0000000F;   // credits form
        b.count   = 500;
        b.plugin  = "";
        b.localID = 0xF;
        p.inventory.push_back(b);
        return p;
    }

    ProfileData RoundTrip(const ProfileData& a_in, bool* a_ok = nullptr)
    {
        auto blob = EncodeProfile(a_in);
        ProfileData out;
        const bool ok = DecodeProfile(blob, out);
        if (a_ok) *a_ok = ok;
        return out;
    }

    bool Near(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }

    std::vector<uint8_t> Bytes(const std::string& s) { return { s.begin(), s.end() }; }
}

TEST(ProfileCodec, BuiltInSelfTestPasses)
{
    CHECK(RunSelfTest());
}

TEST(ProfileCodec, FullRoundTripKeepsEveryField)
{
    const auto a = FullProfile();
    bool ok = false;
    const auto b = RoundTrip(a, &ok);
    REQUIRE(ok);
    CHECK_EQ(b.version, a.version);
    CHECK_EQ(b.playerName, a.playerName);
    CHECK_EQ(b.playerLevel, a.playerLevel);
    CHECK_EQ(b.raceName, a.raceName);
    CHECK_EQ(b.gender, a.gender);
    CHECK_EQ(b.credits, a.credits);
    CHECK(Near(b.o2, a.o2));
    CHECK(Near(b.o2Max, a.o2Max));
    CHECK(Near(b.healthBase, a.healthBase));
    CHECK(Near(b.carryWeightBase, a.carryWeightBase));
    CHECK(Near(b.o2Base, a.o2Base));
    CHECK(Near(b.xp, a.xp));
    CHECK_EQ(b.skillPoints, a.skillPoints);
    CHECK(b.powers == a.powers);
    CHECK(b.research == a.research);
    REQUIRE(b.perks.size() == a.perks.size());
    for (size_t i = 0; i < a.perks.size(); ++i) {
        CHECK_EQ(b.perks[i].formID, a.perks[i].formID);
        CHECK_EQ(b.perks[i].rank, a.perks[i].rank);
    }
    REQUIRE(b.inventory.size() == a.inventory.size());
    for (size_t i = 0; i < a.inventory.size(); ++i) {
        const auto& x = a.inventory[i];
        const auto& y = b.inventory[i];
        CHECK_EQ(y.formID, x.formID);
        CHECK_EQ(y.count, x.count);
        CHECK_EQ(y.plugin, x.plugin);
        CHECK_EQ(y.localID, x.localID);
        CHECK_EQ(y.equipped, x.equipped);
        CHECK_EQ(y.modSentinel, x.modSentinel);
        CHECK(y.mods == x.mods);
    }
}

TEST(ProfileCodec, SpecialCharactersInNamesSurvive)
{
    auto a = FullProfile();
    a.playerName = "John \"Ace\" O'Neil \\ the\tthird\nline";
    a.raceName   = "Q\"uote";
    a.inventory[0].plugin = "My \"Fancy\" Mod.esp";
    bool ok = false;
    const auto b = RoundTrip(a, &ok);
    REQUIRE(ok);
    CHECK_EQ(b.playerName, a.playerName);
    CHECK_EQ(b.raceName, a.raceName);
    REQUIRE(!b.inventory.empty());
    CHECK_EQ(b.inventory[0].plugin, a.inventory[0].plugin);
}

TEST(ProfileCodec, EmptyProfileRoundTrips)
{
    ProfileData a;
    bool ok = false;
    const auto b = RoundTrip(a, &ok);
    CHECK(ok);
    CHECK(b.inventory.empty());
    CHECK(b.perks.empty());
    CHECK(b.powers.empty());
}

TEST(ProfileCodec, ZeroCountItemsAreDropped)
{
    auto a = FullProfile();
    ProfileItem z;
    z.formID = 0x1234;
    z.count  = 0;
    a.inventory.push_back(z);
    const auto b = RoundTrip(a);
    CHECK_EQ(b.inventory.size(), a.inventory.size() - 1);
}

TEST(ProfileCodec, NegativePerkRankSurvivesAsSignedByte)
{
    auto a = FullProfile();
    a.perks = { { 0x1u, -1, "", 0 }, { 0x2u, 127, "", 0 } };
    const auto b = RoundTrip(a);
    REQUIRE(b.perks.size() == 2);
    CHECK_EQ(b.perks[0].rank, int8_t(-1));
    CHECK_EQ(b.perks[1].rank, int8_t(127));
}

TEST(ProfileCodec, LargeInventoryRoundTrips)
{
    ProfileData a = FullProfile();
    a.inventory.clear();
    for (uint32_t i = 1; i <= 2000; ++i) {
        ProfileItem it;
        it.formID  = 0x00100000u + i;
        it.count   = static_cast<int32_t>(i);
        it.plugin  = "Starfield.esm";
        it.localID = i;
        it.mods    = { i, i + 1 };
        a.inventory.push_back(it);
    }
    bool ok = false;
    const auto b = RoundTrip(a, &ok);
    REQUIRE(ok);
    REQUIRE(b.inventory.size() == a.inventory.size());
    CHECK_EQ(b.inventory.back().formID, a.inventory.back().formID);
    CHECK_EQ(b.inventory.back().count, a.inventory.back().count);
    CHECK(b.inventory.back().mods == a.inventory.back().mods);
}

TEST(ProfileCodec, RejectsEmptyAndGarbage)
{
    ProfileData out;
    CHECK(!DecodeProfile({}, out));
    auto garbage = Bytes("hello, this is not a profile");
    CHECK(!DecodeProfile(garbage, out));
    auto obj = Bytes("{ \"something\": 1 }");
    CHECK(!DecodeProfile(obj, out));
    auto noInv = Bytes("{ \"version\": 2, \"playerName\": \"x\" }");
    CHECK(!DecodeProfile(noInv, out));
}

TEST(ProfileCodec, RejectsEveryTruncation)
{
    // A blob cut off at any point (interrupted file write, partial transfer) must
    // be rejected instead of decoding as a partial profile.
    const auto blob = EncodeProfile(FullProfile());
    // Trailing whitespace after the closing brace is not content.
    size_t full = blob.size();
    while (full > 0 && (blob[full - 1] == '\n' || blob[full - 1] == ' ')) --full;
    int accepted = 0;
    for (size_t len = 1; len < full; ++len) {
        std::vector<uint8_t> cut(blob.begin(), blob.begin() + len);
        ProfileData out;
        if (DecodeProfile(cut, out)) ++accepted;
    }
    CHECK_EQ(accepted, 0);
}

TEST(ProfileCodec, DecodeResetsOutputFirst)
{
    ProfileData out = FullProfile();
    ProfileData small;
    small.playerName = "Small";
    auto blob = EncodeProfile(small);
    REQUIRE(DecodeProfile(blob, out));
    CHECK(out.inventory.empty());
    CHECK(out.perks.empty());
    CHECK_EQ(out.playerName, std::string("Small"));
}

TEST(ProfileJson, ExtractStringHandlesEscapesAndMissingKeys)
{
    using namespace Pulsar::Profile::Json;
    const std::string j = R"({"a": "x\"y\\z", "b": 12, "c": -5, "d": "unterminated)";
    CHECK_EQ(ExtractString(j, "a"), std::string("x\"y\\z"));
    CHECK_EQ(ExtractUInt(j, "b"), 12u);
    CHECK_EQ(ExtractInt(j, "c"), -5);
    CHECK_EQ(ExtractString(j, "missing"), std::string(""));
    CHECK_EQ(ExtractUInt(j, "missing"), 0u);
    CHECK_EQ(ExtractString(j, "d"), std::string(""));
    CHECK_EQ(ExtractString(j, "b"), std::string(""));   // not a string value
    CHECK(HasKey(j, "a"));
    CHECK(!HasKey(j, "zz"));
}

TEST(ProfileJson, EscapeThenExtractIsIdentity)
{
    using namespace Pulsar::Profile::Json;
    const std::string samples[] = { "", "plain", "\"", "\\", "a\\\"b", "tab\there", "nl\nx", "\r\n" };
    for (const auto& s : samples) {
        const std::string j = "{\"k\": \"" + Escape(s) + "\"}";
        CHECK_EQ(ExtractString(j, "k"), s);
    }
}
