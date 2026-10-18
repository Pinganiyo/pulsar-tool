// PULSAR.ini parsing (src/Config.cpp, ParseConfig). Every setting a tester is
// told to change lives here; a typo in a key name silently keeps the default.

#include "TestHarness.h"

#include "Config.h"
#include "VehicleSync/RidePolicy.h"

#include <sstream>

using Pulsar::Config;
using Pulsar::ParseConfig;

namespace {
    Config Parse(const std::string& a_ini)
    {
        std::istringstream in(a_ini);
        return ParseConfig(in);
    }
}

TEST(Config, EmptyFileGivesTheDefaults)
{
    const auto c = Parse("");
    const Config d;
    CHECK_EQ(c.isHost, d.isHost);
    CHECK_EQ(c.port, d.port);
    CHECK_EQ(c.hotkeyManualBridge, 0x77);   // F8
    CHECK_EQ(c.hotkeyBuddy, 0x78);          // F9
    CHECK(!c.autoRegroupInSpace);
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kOff));
    CHECK(c.syncCloneWeapons);
    CHECK(c.dbAuthoritative);
}

TEST(Config, NewSettingsAreRead)
{
    const auto c = Parse(
        "[PULSAR]\n"
        "AutoRegroupInSpace = 1\n"
        "SharedVehicleRide = carry\n"
        "SyncCloneWeapons = 0\n"
        "HotkeyBuddy = F6\n"
        "BuddyBaseFormID = 0x0001234A\n");
    CHECK(c.autoRegroupInSpace);
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kCarry));
    CHECK(!c.syncCloneWeapons);
    CHECK_EQ(c.hotkeyBuddy, 0x75);
    CHECK_EQ(c.buddyBaseFormID, 0x0001234Au);
}

TEST(Config, KeysAreCaseInsensitiveAndAcceptSnakeCase)
{
    const auto c = Parse("auto_regroup_in_space=true\nSHAREDVEHICLERIDE=enter\nsync_clone_weapons=off\n");
    CHECK(c.autoRegroupInSpace);
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kEnter));
    CHECK(!c.syncCloneWeapons);
}

TEST(Config, CommentsAndJunkAreIgnored)
{
    const auto c = Parse(
        "; a comment\n"
        "# another\n"
        "garbage line without equals\n"
        "UnknownKey = 5\n"
        "AutoRegroupInSpace = 1 ; inline comment\n"
        "  SharedVehicleRide   =   carry   \r\n");
    CHECK(c.autoRegroupInSpace);
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kCarry));
}

TEST(Config, BadValuesKeepSafeDefaults)
{
    const auto c = Parse("SharedVehicleRide = sometimes\nHotkeyBuddy = NotAKey\n");
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kOff));
    CHECK_EQ(c.hotkeyBuddy, 0x78);
}
