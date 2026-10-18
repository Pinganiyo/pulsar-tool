#pragma once

// Which PULSAR builds may play together, and how the host tells a client why it
// was refused. Game-free (unit-tested in tests/test_version_compat.cpp).
//
// Packet layouts change between releases, so both players must run the SAME
// version. Before 0.3.1 the client always sent a hard-coded 0.1.0 and the host
// never checked it; a 0.3.0 peer therefore shows up as 0.1.0 here.

#include <cstdint>
#include <string>

namespace Pulsar::Network::Compat {

    struct Version {
        std::uint8_t major = 0;
        std::uint8_t minor = 0;
        std::uint8_t patch = 0;

        bool operator==(const Version&) const = default;
    };

    inline bool IsCompatible(const Version& a_local, const Version& a_remote)
    {
        return a_local == a_remote;
    }

    inline std::string ToString(const Version& a_v)
    {
        return std::to_string(a_v.major) + "." + std::to_string(a_v.minor) + "." +
               std::to_string(a_v.patch);
    }

    // Peers older than 0.3.1 report this fixed value whatever they really run.
    inline constexpr Version kLegacyReported{ 0, 1, 0 };

    inline std::string DescribeRemote(const Version& a_remote)
    {
        return a_remote == kLegacyReported ? std::string("0.3.0 or older") : ToString(a_remote);
    }

    // ENet disconnect data: 0x56 ('V') tag in the top byte, then the HOST's
    // major/minor/patch, so a refused client can say which version to install.
    // Other reason values (0 = normal disconnect) never carry this tag.
    inline constexpr std::uint32_t kRejectTag = 0x56000000u;

    inline std::uint32_t EncodeVersionReject(const Version& a_host)
    {
        return kRejectTag | (std::uint32_t(a_host.major) << 16) |
               (std::uint32_t(a_host.minor) << 8) | std::uint32_t(a_host.patch);
    }

    inline bool DecodeVersionReject(std::uint32_t a_reason, Version& a_host)
    {
        if ((a_reason & 0xFF000000u) != kRejectTag) return false;
        a_host.major = static_cast<std::uint8_t>((a_reason >> 16) & 0xFF);
        a_host.minor = static_cast<std::uint8_t>((a_reason >> 8) & 0xFF);
        a_host.patch = static_cast<std::uint8_t>(a_reason & 0xFF);
        return true;
    }

    // Copy a fixed-size, possibly unterminated name field into a string.
    inline std::string SafeName(const char* a_field, std::size_t a_size)
    {
        std::size_t n = 0;
        while (n < a_size && a_field[n] != '\0') ++n;
        return std::string(a_field, n);
    }
}
