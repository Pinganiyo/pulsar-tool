// Shared vehicle entry guard (include/VehicleSync/RidePolicy.h).

#include "TestHarness.h"

#include "VehicleSync/RidePolicy.h"

using namespace Pulsar::VehicleSync;

namespace {
    constexpr std::uint32_t kRover = 0x00ABCDEFu;
    HostVehicle Driving(std::uint64_t at) { return { kRover, 100.0f, 200.0f, 0.0f, at }; }
    // Positions in metres: the client stands ~5 m from the host's vehicle.
}

TEST(RidePolicy, ParsesModes)
{
    CHECK(ParseRideMode("off") == RideMode::kOff);
    CHECK(ParseRideMode("Carry") == RideMode::kCarry);
    CHECK(ParseRideMode("ENTER") == RideMode::kEnter);
    CHECK(ParseRideMode("1") == RideMode::kEnter);
    CHECK(ParseRideMode("nonsense") == RideMode::kOff);
    CHECK(ParseRideMode("nonsense", RideMode::kCarry) == RideMode::kCarry);
    CHECK_EQ(std::string(RideModeName(RideMode::kCarry)), std::string("carry"));
}

TEST(RidePolicy, BlocksTheClientFromTheHostsVehicle)
{
    const auto host = Driving(10000);
    CHECK(ShouldBlockEntry(RideMode::kOff, true, kRover, 103, 204, 0, host, 10500));
    CHECK(ShouldBlockEntry(RideMode::kCarry, true, kRover, 103, 204, 0, host, 10500));
}

TEST(RidePolicy, EnterModeKeepsTheOldBehaviour)
{
    CHECK(!ShouldBlockEntry(RideMode::kEnter, true, kRover, 103, 204, 0, Driving(10000), 10500));
}

TEST(RidePolicy, NeverBlocksTheHost)
{
    CHECK(!ShouldBlockEntry(RideMode::kOff, false, kRover, 103, 204, 0, Driving(10000), 10500));
}

TEST(RidePolicy, OnlyWhileTheHostIsInTheVehicle)
{
    // Last PKT_VEHICLE_POS 3 s ago: the host got out, the vehicle is free.
    CHECK(!ShouldBlockEntry(RideMode::kOff, true, kRover, 103, 204, 0, Driving(10000), 13000));
    // Never received.
    CHECK(!ShouldBlockEntry(RideMode::kOff, true, kRover, 103, 204, 0, HostVehicle{}, 13000));
}

TEST(RidePolicy, OtherVehiclesAreFree)
{
    const auto host = Driving(10000);
    // A different vehicle type next to the host.
    CHECK(!ShouldBlockEntry(RideMode::kOff, true, 0x00111111u, 103, 204, 0, host, 10500));
    // Same type, but far away (another rover elsewhere).
    CHECK(!ShouldBlockEntry(RideMode::kOff, true, kRover, 100 + 60, 200, 0, host, 10500));
}
