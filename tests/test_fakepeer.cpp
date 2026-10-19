// The fake peer (tests/fakepeer) against an in-process host that follows
// NetworkHost's side of the protocol: hello on connect, decrypt everything else,
// answer PKT_HANDSHAKE with a PKT_SAVE_TRANSFER, stream host PKT_POSITION.
// Proves the bot's handshake works end to end without the game.

#include "TestHarness.h"

#include "fakepeer/FakePeer.h"
#include "Network/KeyExchange.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace Pulsar::Network;
using Pulsar::FakePeer::FakePeer;
using Pulsar::FakePeer::Mode;
using Pulsar::FakePeer::Options;

namespace {

    struct EnetGuard {
        EnetGuard() { ok = enet_initialize() == 0; }
        ~EnetGuard() { if (ok) enet_deinitialize(); }
        bool ok = false;
    };

    // Minimal stand-in for NetworkHost in host mode.
    struct MiniHost {
        ENetHost*     host = nullptr;
        ENetPeer*     peer = nullptr;
        CryptoChannel crypto;
        std::vector<uint8_t> secret;
        bool          ready = false;
        size_t        saveSize = 4096;

        bool          gotHandshake = false;
        HandshakeData handshake{};
        int           positions = 0;
        PositionData  lastPos{};
        bool          gotLoadComplete = false;
        bool          gotTimeWeatherReq = false;
        int           decryptFailures = 0;

        ~MiniHost() { if (host) enet_host_destroy(host); }

        uint16_t Listen()
        {
            ENetAddress addr{};
            enet_address_set_host(&addr, "127.0.0.1");
            addr.port = 0;
            host = enet_host_create(&addr, 1, 3, 0, 0);
            if (!host) return 0;
            ENetAddress bound{};
            if (enet_socket_get_address(host->socket, &bound) != 0) return 0;
            return bound.port;
        }

        void Send(const std::vector<uint8_t>& a_plain, int a_channel, bool a_reliable)
        {
            std::vector<uint8_t> enc;
            if (!crypto.Encrypt(a_plain.data(), a_plain.size(), enc)) return;
            enet_peer_send(peer, static_cast<enet_uint8>(a_channel),
                           enet_packet_create(enc.data(), enc.size(), a_reliable ? ENET_PACKET_FLAG_RELIABLE : 0));
        }

        void SendPosition(float x, float y, float z, uint32_t cell)
        {
            PositionData p{};
            p.x = x; p.y = y; p.z = z;
            p.cellFormID = cell;
            SendPositionData(p);
        }

        void SendPositionData(const PositionData& p)
        {
            uint8_t buf[128]{};
            PacketWriter w(PKT_POSITION, buf, sizeof(buf));
            w.Write(p);
            // Reliable here: the tests send ONE anchor position and wait for the
            // bot to react; losing it made the test flaky. (The game streams
            // positions at 30 Hz unreliably, where a lost packet does not matter.)
            Send(std::vector<uint8_t>(buf, buf + w.Size()), 0, true);
        }

        void Service()
        {
            ENetEvent ev;
            while (enet_host_service(host, &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                    peer = ev.peer;
                    crypto.Init();
                    crypto.SetAuthSecret(secret.empty() ? nullptr : secret.data(), secret.size());
                    auto pub = crypto.PublicBlob();
                    std::vector<uint8_t> hello(1 + pub.size());
                    hello[0] = PKT_CRYPTO_HELLO;
                    std::memcpy(hello.data() + 1, pub.data(), pub.size());
                    enet_peer_send(peer, 0, enet_packet_create(hello.data(), hello.size(), ENET_PACKET_FLAG_RELIABLE));
                } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    OnReceive(ev.packet->data, ev.packet->dataLength);
                    enet_packet_destroy(ev.packet);
                }
            }
            enet_host_flush(host);
        }

        void OnReceive(const uint8_t* d, size_t n)
        {
            if (IsKeyExchangeHello(ready, d, n)) {
                ready = crypto.DeriveFromPeer(d + 1, n - 1);
                return;
            }
            std::vector<uint8_t> plain;
            if (!ready || !crypto.Decrypt(d, n, plain)) { ++decryptFailures; return; }
            const uint8_t type = plain[0];
            if (type == PKT_TIME_WEATHER_REQ) { gotTimeWeatherReq = true; return; }
            PacketReader r(plain.data(), plain.size());
            if (!r.IsValid()) return;
            if (type == PKT_HANDSHAKE && r.Read(handshake)) {
                gotHandshake = true;
                // Host answers with the save (one big reliable packet on channel 1).
                std::vector<uint8_t> save(1 + 4 + saveSize);
                save[0] = PKT_SAVE_TRANSFER;
                const uint32_t sz = static_cast<uint32_t>(saveSize);
                std::memcpy(&save[1], &sz, 4);
                for (size_t i = 0; i < saveSize; ++i) save[5 + i] = static_cast<uint8_t>(i * 7);
                Send(save, 1, true);
            } else if (type == PKT_POSITION && r.Read(lastPos)) {
                ++positions;
            } else if (type == PKT_CLIENT_LOAD_COMPLETE) {
                gotLoadComplete = true;
            }
        }
    };

    bool Pump(MiniHost& h, FakePeer& b, const std::function<bool()>& a_done, int a_timeoutMs = 5000)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(a_timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            h.Service();
            b.Service(1);
            b.Tick();
            if (a_done()) return true;
        }
        if (a_done()) return true;
        std::printf("    Pump timeout: host.ready=%d peer=%p decryptFailures=%d positions=%d "
                    "handshake=%d | bot secure=%d %s\n",
                    h.ready, static_cast<void*>(h.peer), h.decryptFailures, h.positions,
                    h.gotHandshake, b.SecureReady(), b.StatsLine().c_str());
        return false;
    }

    Options BotOptions(uint16_t a_port)
    {
        Options o;
        o.port = a_port;
        o.name = "LoopBot";
        return o;
    }
}

TEST(FakePeer, HandshakeSaveAndPositionsAgainstHost)
{
    EnetGuard g;
    REQUIRE(g.ok);
    MiniHost host;
    host.saveSize = 300 * 1024;   // big enough that ENet fragments it
    const uint16_t port = host.Listen();
    REQUIRE(port != 0);

    FakePeer bot(BotOptions(port));
    REQUIRE(bot.Connect());

    // Key exchange + handshake.
    REQUIRE(Pump(host, bot, [&] { return host.gotHandshake; }));
    CHECK(bot.SecureReady());
    CHECK_EQ(std::string(host.handshake.playerName), std::string("LoopBot"));
    CHECK_EQ(int(host.handshake.dlcFlags), 0x03);
    // The real host (0.3.1+) refuses any other version (Network/VersionCompat.h).
    CHECK_EQ(int(host.handshake.versionMajor), PULSAR_VERSION_MAJOR);
    CHECK_EQ(int(host.handshake.versionMinor), PULSAR_VERSION_MINOR);
    CHECK_EQ(int(host.handshake.versionPatch), PULSAR_VERSION_PATCH);
    CHECK_EQ(host.decryptFailures, 0);

    // The save arrives intact and the bot reports it "loaded".
    REQUIRE(Pump(host, bot, [&] { return host.gotLoadComplete; }));
    CHECK_EQ(bot.GetStats().savesReceived, 1ull);
    CHECK_EQ(bot.GetStats().lastSaveBytes, 300ull * 1024);
    CHECK(host.gotTimeWeatherReq);

    // No positions until the host says where it is.
    CHECK_EQ(host.positions, 0);
    host.SendPosition(1000.0f, 2000.0f, 50.0f, 0x0001ABCD);
    REQUIRE(Pump(host, bot, [&] { return host.positions >= 5; }));
    CHECK(bot.HasHostPosition());
    CHECK_EQ(host.lastPos.cellFormID, 0x0001ABCDu);   // echoes the host's cell
    CHECK_EQ(host.lastPos.x, 1002.0f);                 // stand mode: anchor + default offset 2,0,0 (metres)
    CHECK_EQ(host.lastPos.y, 2000.0f);
    CHECK_EQ(bot.GetStats().recvCount[PKT_POSITION], 1ull);
    CHECK_EQ(bot.GetStats().recvCount[PKT_SAVE_TRANSFER], 1ull);
}

TEST(FakePeer, CircleModeMovesAndAnimates)
{
    EnetGuard g;
    REQUIRE(g.ok);
    MiniHost host;
    const uint16_t port = host.Listen();
    REQUIRE(port != 0);

    Options o = BotOptions(port);
    o.mode = Mode::kCircle;
    o.sendHandshake = false;
    o.offset[0] = 0.0f;
    o.radius = 3.0f;
    o.periodS = 2.0f;
    FakePeer bot(o);
    REQUIRE(bot.Connect());
    REQUIRE(Pump(host, bot, [&] { return host.ready && bot.SecureReady(); }));
    CHECK(!host.gotHandshake);

    host.SendPosition(0.0f, 0.0f, 10.0f, 0x00000123);
    REQUIRE(Pump(host, bot, [&] { return host.positions >= 10; }));
    const float r = std::sqrt(host.lastPos.x * host.lastPos.x + host.lastPos.y * host.lastPos.y);
    CHECK(r > 2.99f && r < 3.01f);
    CHECK(host.lastPos.speed > 5.0f);                // ~ 2*pi*3/2 = 9.4 m/s
    CHECK_EQ(host.lastPos.vars.iSyncIdleLocomotion, 1);

    // Faces the way it moves (counter-clockwise tangent), heading = atan2(dx, dy).
    const float a = std::atan2(host.lastPos.y, host.lastPos.x);
    const float want = std::atan2(-std::sin(a), std::cos(a));
    float diff = host.lastPos.rotZ - want;
    while (diff > 3.14159265f) diff -= 6.28318531f;
    while (diff < -3.14159265f) diff += 6.28318531f;
    CHECK(std::fabs(diff) < 0.01f);
    CHECK_EQ(host.lastPos.vars.direction, 0.0f);      // forward relative to facing
}

TEST(FakePeer, FollowModeReplaysTheHostsAnimationState)
{
    // Regression: follow mode used to send Direction = 0 ("forward") with the
    // host's facing, so a strafing host became a clone running forward while
    // sliding sideways. It must replay the host's own animation values.
    EnetGuard g;
    REQUIRE(g.ok);
    MiniHost host;
    const uint16_t port = host.Listen();
    REQUIRE(port != 0);

    Options o = BotOptions(port);
    o.mode = Mode::kFollow;
    o.sendHandshake = false;
    o.followDelayMs = 0;
    o.offset[0] = 1.0f; o.offset[1] = 0.0f; o.offset[2] = 0.0f;
    FakePeer bot(o);
    REQUIRE(bot.Connect());
    REQUIRE(Pump(host, bot, [&] { return host.ready && bot.SecureReady(); }));

    PositionData strafe{};
    strafe.x = 10.0f; strafe.y = 20.0f; strafe.z = 1.0f;
    strafe.rotZ = 1.25f;                              // facing
    strafe.speed = 6.0f;
    strafe.direction = 0.25f;                         // moving to the facing's right
    strafe.animFlags = 0x0005;
    strafe.vars.speed = 6.0f;
    strafe.vars.direction = 0.25f;
    strafe.vars.iSyncSprintState = 1;
    strafe.vars.iSyncIdleLocomotion = 1;
    strafe.cellFormID = 0x0001ABCD;
    host.SendPositionData(strafe);

    REQUIRE(Pump(host, bot, [&] { return host.positions >= 5; }));
    CHECK_EQ(host.lastPos.x, 11.0f);                  // offset applied
    CHECK_EQ(host.lastPos.y, 20.0f);
    CHECK_EQ(host.lastPos.rotZ, 1.25f);
    CHECK_EQ(host.lastPos.vars.direction, 0.25f);
    CHECK_EQ(host.lastPos.direction, 0.25f);
    CHECK_EQ(host.lastPos.vars.iSyncSprintState, 1);
    CHECK_EQ(host.lastPos.vars.speed, 6.0f);
    CHECK_EQ(int(host.lastPos.animFlags), 0x0005);
    CHECK_EQ(host.lastPos.cellFormID, 0x0001ABCDu);
}

TEST(FakePeer, WrongSecretIsVisibleAsDecryptFailures)
{
    EnetGuard g;
    REQUIRE(g.ok);
    MiniHost host;
    host.secret = { 'l', 'o', 'b', 'b', 'y' };
    const uint16_t port = host.Listen();
    REQUIRE(port != 0);

    Options o = BotOptions(port);
    o.secret = "wrong";
    FakePeer bot(o);
    REQUIRE(bot.Connect());
    REQUIRE(Pump(host, bot, [&] { return host.decryptFailures > 0; }));
    CHECK(!host.gotHandshake);
}

TEST(FakePeer, MatchingSecretWorks)
{
    EnetGuard g;
    REQUIRE(g.ok);
    MiniHost host;
    host.secret = { 'l', 'o', 'b', 'b', 'y' };
    const uint16_t port = host.Listen();
    REQUIRE(port != 0);

    Options o = BotOptions(port);
    o.secret = "lobby";
    FakePeer bot(o);
    REQUIRE(bot.Connect());
    REQUIRE(Pump(host, bot, [&] { return host.gotHandshake; }));
    CHECK_EQ(host.decryptFailures, 0);
}
