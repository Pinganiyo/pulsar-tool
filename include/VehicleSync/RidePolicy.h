#pragma once

// What happens when the client walks up to the vehicle the host is driving.
// Game-free (tests/test_ride_policy.cpp).
//
// Formally entering the host's vehicle as the client crashes the client (the
// engine's vehicle occupant list, see VehicleSync.cpp). Until that is fixed the
// default refuses that entry with a message instead of crashing.
//   off   - (default) the client cannot enter the vehicle the host is in.
//   carry - entry refused too, but walking up to it "boards" the client, who is
//           then carried on the passenger seat each tick (experimental).
//   enter - old behaviour: formal entry allowed (known crash).

#include <cctype>
#include <cstdint>
#include <string>

namespace Pulsar::VehicleSync {

    enum class RideMode : std::uint8_t { kOff, kCarry, kEnter };

    inline RideMode ParseRideMode(std::string a_value, RideMode a_default = RideMode::kOff)
    {
        for (auto& c : a_value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (a_value == "off" || a_value == "0" || a_value == "false") return RideMode::kOff;
        if (a_value == "carry") return RideMode::kCarry;
        if (a_value == "enter" || a_value == "1" || a_value == "true") return RideMode::kEnter;
        return a_default;
    }

    inline const char* RideModeName(RideMode a_mode)
    {
        switch (a_mode) {
            case RideMode::kCarry: return "carry";
            case RideMode::kEnter: return "enter";
            default:               return "off";
        }
    }

    struct HostVehicle {
        std::uint32_t baseID = 0;        // client-local base form of the host's vehicle
        float         x = 0, y = 0, z = 0;
        std::uint64_t lastUpdateMs = 0;  // last PKT_VEHICLE_POS (0 = never)
    };

    // Metres. Activation reach plus vehicle size, generously.
    inline constexpr float         kSameVehicleRadius = 20.0f;
    // PKT_VEHICLE_POS only flows while the host occupies the vehicle.
    inline constexpr std::uint64_t kHostInVehicleMs   = 2000;

    inline bool HostIsDriving(const HostVehicle& a_host, std::uint64_t a_nowMs)
    {
        return a_host.baseID != 0 && a_host.lastUpdateMs != 0 && a_nowMs >= a_host.lastUpdateMs &&
               a_nowMs - a_host.lastUpdateMs <= kHostInVehicleMs;
    }

    // Should the CLIENT's activation of this ref be refused?
    inline bool ShouldBlockEntry(RideMode a_mode, bool a_isClient, std::uint32_t a_refBaseID,
                                 float a_refX, float a_refY, float a_refZ,
                                 const HostVehicle& a_host, std::uint64_t a_nowMs)
    {
        if (a_mode == RideMode::kEnter || !a_isClient) return false;
        if (!HostIsDriving(a_host, a_nowMs) || a_refBaseID != a_host.baseID) return false;
        const float dx = a_refX - a_host.x, dy = a_refY - a_host.y, dz = a_refZ - a_host.z;
        return dx * dx + dy * dy + dz * dz <= kSameVehicleRadius * kSameVehicleRadius;
    }
}
