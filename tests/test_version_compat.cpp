// Version handshake rules (include/Network/VersionCompat.h) and the release
// bookkeeping that keeps the version in sync across the build products.

#include "TestHarness.h"

#include "Network/PacketTypes.h"
#include "Network/VersionCompat.h"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

using namespace Pulsar::Network;

namespace {
    std::string ReadRepoFile(const char* a_rel)
    {
        std::ifstream f(std::string(PULSAR_SOURCE_DIR) + "/" + a_rel);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string FirstMatch(const std::string& a_text, const char* a_re)
    {
        std::smatch m;
        if (std::regex_search(a_text, m, std::regex(a_re))) return m[1].str();
        return "";
    }
}

TEST(VersionCompat, OnlyIdenticalVersionsPlayTogether)
{
    const Compat::Version v031{ 0, 3, 1 };
    CHECK(Compat::IsCompatible(v031, v031));
    CHECK(!Compat::IsCompatible(v031, { 0, 3, 0 }));
    CHECK(!Compat::IsCompatible(v031, { 0, 4, 1 }));
    CHECK(!Compat::IsCompatible(v031, { 1, 3, 1 }));
    CHECK(!Compat::IsCompatible(v031, Compat::kLegacyReported));
}

TEST(VersionCompat, LegacyPeersAreNamedAsOld)
{
    CHECK_EQ(Compat::DescribeRemote(Compat::kLegacyReported), std::string("0.3.0 or older"));
    CHECK_EQ(Compat::DescribeRemote({ 0, 4, 0 }), std::string("0.4.0"));
}

TEST(VersionCompat, RejectReasonRoundTrips)
{
    const Compat::Version host{ 0, 7, 12 };
    Compat::Version out{};
    REQUIRE(Compat::DecodeVersionReject(Compat::EncodeVersionReject(host), out));
    CHECK(out == host);
}

TEST(VersionCompat, OrdinaryDisconnectsAreNotVersionRejects)
{
    Compat::Version out{};
    CHECK(!Compat::DecodeVersionReject(0, out));   // graceful
    CHECK(!Compat::DecodeVersionReject(1, out));
    CHECK(!Compat::DecodeVersionReject(2, out));
    CHECK(!Compat::DecodeVersionReject(0xFFFFFFFFu, out));
}

TEST(VersionCompat, UnterminatedNameIsBounded)
{
    HandshakeData hs{};
    for (auto& c : hs.playerName) c = 'A';   // no terminator at all
    const auto name = Compat::SafeName(hs.playerName, sizeof(hs.playerName));
    CHECK_EQ(name.size(), sizeof(hs.playerName));
    CHECK_EQ(Compat::SafeName("Bob\0junk", 8), std::string("Bob"));
}

TEST(Release, VersionIsTheSameEverywhere)
{
    // Both players must run the same version, and the handshake compares
    // Plugin.h's. A bump that misses one file ships a mislabelled build.
    const auto plugin = ReadRepoFile("include/Plugin.h");
    const auto maj = FirstMatch(plugin, R"(REL::Version\{\s*(\d+),)");
    const auto min = FirstMatch(plugin, R"(REL::Version\{\s*\d+,\s*(\d+),)");
    const auto pat = FirstMatch(plugin, R"(REL::Version\{\s*\d+,\s*\d+,\s*(\d+),)");
    REQUIRE(!maj.empty() && !min.empty() && !pat.empty());
    const std::string v = maj + "." + min + "." + pat;

    CHECK_EQ(FirstMatch(ReadRepoFile("CMakeLists.txt"), R"(VERSION\s+(\d+\.\d+\.\d+))"), v);
    CHECK_EQ(FirstMatch(ReadRepoFile("lobby/pulsar/Pulsar.csproj"), R"(<Version>([^<]+)</Version>)"), v);
    CHECK_EQ(FirstMatch(ReadRepoFile("lobby/companion/Companion.csproj"), R"(<Version>([^<]+)</Version>)"), v);
    CHECK_EQ(FirstMatch(ReadRepoFile("lobby/pulsar/Pulsar.csproj"), R"(<AssemblyVersion>([^<]+)</AssemblyVersion>)"), v + ".0");
    CHECK_EQ(FirstMatch(ReadRepoFile("lobby/companion/Companion.csproj"), R"(<FileVersion>([^<]+)</FileVersion>)"), v + ".0");
}

TEST(Release, ChangelogHasAnEntryForThisVersion)
{
    const auto plugin = ReadRepoFile("include/Plugin.h");
    const auto v = FirstMatch(plugin, R"(REL::Version\{\s*(\d+),)") + "." +
                   FirstMatch(plugin, R"(REL::Version\{\s*\d+,\s*(\d+),)") + "." +
                   FirstMatch(plugin, R"(REL::Version\{\s*\d+,\s*\d+,\s*(\d+),)");
    const auto log = ReadRepoFile("CHANGELOG.md");
    CHECK(log.find("## " + v) != std::string::npos);
}
