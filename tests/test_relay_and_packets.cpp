// Wire formats: relay control packets (include/Network/RelayProtocol.h, shared
// with the C# relay) and the game packet table (include/Network/PacketTypes.h).

#include "TestHarness.h"

#include "Network/PacketTypes.h"
#include "Network/RelayProtocol.h"

#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace Relay = Pulsar::Network::Relay;
using namespace Pulsar::Network;

// ─── Relay protocol ───

TEST(Relay, DecodeSessionAcceptsHexInEitherCase)
{
    uint8_t out[Relay::kSessionBytes]{};
    REQUIRE(Relay::DecodeSession("00112233445566778899aabbccddeeff", out));
    CHECK_EQ(int(out[0]), 0x00);
    CHECK_EQ(int(out[1]), 0x11);
    CHECK_EQ(int(out[15]), 0xFF);
    uint8_t up[Relay::kSessionBytes]{};
    REQUIRE(Relay::DecodeSession("00112233445566778899AABBCCDDEEFF", up));
    CHECK(std::memcmp(out, up, sizeof(out)) == 0);
}

TEST(Relay, DecodeSessionRejectsBadInput)
{
    uint8_t out[Relay::kSessionBytes]{};
    CHECK(!Relay::DecodeSession("", out));
    CHECK(!Relay::DecodeSession("0011", out));                                // too short
    CHECK(!Relay::DecodeSession("00112233445566778899aabbccddeeff00", out));  // too long
    CHECK(!Relay::DecodeSession("00112233445566778899aabbccddeegg", out));    // bad char
    CHECK(!Relay::DecodeSession("0011223344556677 899aabbccddeeff", out));    // space
}

TEST(Relay, ControlPacketLayout)
{
    uint8_t s[Relay::kSessionBytes];
    for (size_t i = 0; i < sizeof(s); ++i) s[i] = static_cast<uint8_t>(i + 1);
    auto b = Relay::MakeControl(Relay::kTypeJoin, s);
    CHECK_EQ(b.size(), size_t(25));
    CHECK(std::memcmp(b.data(), "PULSARLY", 8) == 0);
    CHECK_EQ(int(b[8]), int(Relay::kTypeJoin));
    CHECK(std::memcmp(b.data() + 9, s, sizeof(s)) == 0);
    CHECK(Relay::IsControlMagic(b.data(), b.size()));
}

TEST(Relay, PeerPacketRoundTrip)
{
    uint8_t s[Relay::kSessionBytes]{};
    const uint32_t ip   = 0x0100007F;   // 127.0.0.1 in network order
    const uint16_t port = 7777;
    auto b = Relay::MakePeer(s, ip, port);
    CHECK_EQ(int(b[29]), 7777 >> 8);    // big-endian port
    CHECK_EQ(int(b[30]), 7777 & 0xFF);
    uint32_t ipOut = 0;
    uint16_t portOut = 0;
    REQUIRE(Relay::ParsePeer(b.data(), b.size(), ipOut, portOut));
    CHECK_EQ(ipOut, ip);
    CHECK_EQ(portOut, port);
}

TEST(Relay, ClassifierRejectsNonControlTraffic)
{
    uint8_t s[Relay::kSessionBytes]{};
    auto b = Relay::MakeControl(Relay::kTypeRegister, s);
    CHECK(!Relay::IsControlMagic(nullptr, 0));
    CHECK(!Relay::IsControlMagic(b.data(), b.size() - 1));   // short
    auto wrongMagic = b;
    wrongMagic[0] = 'X';
    CHECK(!Relay::IsControlMagic(wrongMagic.data(), wrongMagic.size()));
    auto unknownType = b;
    unknownType[8] = 99;
    CHECK(!Relay::IsControlMagic(unknownType.data(), unknownType.size()));
    uint32_t ip; uint16_t port;
    CHECK(!Relay::ParsePeer(b.data(), b.size(), ip, port));  // REGISTER, not PEER
}

// ─── Game packets ───

TEST(Packets, HeaderIsThreeBytes)
{
    CHECK_EQ(HEADER_SIZE, size_t(3));
}

TEST(Packets, PacketTypeIdsAreUnique)
{
    // Two packet types sharing an id would route one feature's packets into
    // another's handler. The enum cannot be enumerated at runtime, so read it.
    std::ifstream f(PULSAR_SOURCE_DIR "/include/Network/PacketTypes.h");
    REQUIRE(f.is_open());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    const auto enumStart = text.find("enum PacketType");
    const auto enumEnd   = text.find("};", enumStart);
    REQUIRE(enumStart != std::string::npos && enumEnd != std::string::npos);
    const std::string body = text.substr(enumStart, enumEnd - enumStart);

    std::regex re(R"((PKT_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+))");
    std::map<unsigned long, std::string> seen;
    int count = 0;
    for (std::sregex_iterator it(body.begin(), body.end(), re), end; it != end; ++it) {
        ++count;
        const auto name = (*it)[1].str();
        const auto id   = std::stoul((*it)[2].str(), nullptr, 16);
        auto [pos, inserted] = seen.emplace(id, name);
        if (!inserted)
            ::pt::Fail(__FILE__, __LINE__, name + " reuses id of " + pos->second);
        CHECK(id <= 0xFF);
    }
    CHECK(count > 50);
}

TEST(Packets, PositionPacketSurvivesSerialization)
{
    PositionData p{};
    p.x = 1.5f; p.y = -2.25f; p.z = 100.0f;
    p.rotZ = 3.14f;
    p.animFlags = 0x0203;
    p.vars.speed = 4.0f;
    p.vars.iSyncSprintState = 1;
    p.cellFormID = 0xFF012345;

    std::vector<uint8_t> wire(HEADER_SIZE + sizeof(PositionData));
    PacketHeader h{ PKT_POSITION, static_cast<uint16_t>(sizeof(PositionData)) };
    std::memcpy(wire.data(), &h, HEADER_SIZE);
    std::memcpy(wire.data() + HEADER_SIZE, &p, sizeof(p));

    PacketHeader h2{};
    std::memcpy(&h2, wire.data(), HEADER_SIZE);
    CHECK_EQ(int(h2.type), int(PKT_POSITION));
    CHECK_EQ(size_t(h2.size), sizeof(PositionData));
    PositionData q{};
    std::memcpy(&q, wire.data() + HEADER_SIZE, sizeof(q));
    CHECK_EQ(q.x, p.x);
    CHECK_EQ(q.y, p.y);
    CHECK_EQ(q.rotZ, p.rotZ);
    CHECK_EQ(q.animFlags, p.animFlags);
    CHECK_EQ(q.vars.speed, p.vars.speed);
    CHECK_EQ(q.vars.iSyncSprintState, p.vars.iSyncSprintState);
    CHECK_EQ(q.cellFormID, p.cellFormID);
}
