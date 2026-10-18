// Loopback networking: two ENet peers in one process, on 127.0.0.1, running the
// same connect -> PKT_CRYPTO_HELLO exchange -> encrypted traffic -> disconnect
// sequence NetworkHost uses. No game involved.

#include "TestHarness.h"

#include "Network/Crypto.h"
#include "Network/PacketTypes.h"

#include <enet/enet.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace Pulsar::Network;

namespace {

    struct EnetGuard {
        EnetGuard() { ok = enet_initialize() == 0; }
        ~EnetGuard() { if (ok) enet_deinitialize(); }
        bool ok = false;
    };

    // One side of a session: ENet host + crypto channel + the state it went through.
    struct Peer {
        ENetHost*     host = nullptr;
        ENetPeer*     peer = nullptr;
        CryptoChannel crypto;
        bool          connected    = false;
        bool          disconnected = false;
        std::vector<std::vector<uint8_t>> received;   // decrypted payloads
        int           decryptFailures = 0;

        ~Peer() { if (host) enet_host_destroy(host); }

        void SendHello()
        {
            auto pub = crypto.PublicBlob();
            std::vector<uint8_t> pkt(1 + pub.size());
            pkt[0] = PKT_CRYPTO_HELLO;
            std::memcpy(pkt.data() + 1, pub.data(), pub.size());
            enet_peer_send(peer, 0, enet_packet_create(pkt.data(), pkt.size(), ENET_PACKET_FLAG_RELIABLE));
        }

        bool SendEncrypted(const std::vector<uint8_t>& a_plain)
        {
            std::vector<uint8_t> enc;
            if (!crypto.Encrypt(a_plain.data(), a_plain.size(), enc)) return false;
            enet_peer_send(peer, 0, enet_packet_create(enc.data(), enc.size(), ENET_PACKET_FLAG_RELIABLE));
            return true;
        }

        void Service()
        {
            ENetEvent ev;
            while (enet_host_service(host, &ev, 0) > 0) {
                switch (ev.type) {
                    case ENET_EVENT_TYPE_CONNECT:
                        peer = ev.peer;
                        connected = true;
                        SendHello();
                        break;
                    case ENET_EVENT_TYPE_RECEIVE: {
                        const auto* d = ev.packet->data;
                        const size_t n = ev.packet->dataLength;
                        if (!crypto.Ready() && n > 1 && d[0] == PKT_CRYPTO_HELLO) {
                            crypto.DeriveFromPeer(d + 1, n - 1);
                        } else {
                            std::vector<uint8_t> plain;
                            if (crypto.Decrypt(d, n, plain)) received.push_back(std::move(plain));
                            else ++decryptFailures;
                        }
                        enet_packet_destroy(ev.packet);
                        break;
                    }
                    case ENET_EVENT_TYPE_DISCONNECT:
                        disconnected = true;
                        break;
                    default:
                        break;
                }
            }
        }
    };

    bool PumpUntil(Peer& a, Peer& b, const std::function<bool()>& a_done, int a_timeoutMs = 3000)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(a_timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            a.Service();
            b.Service();
            if (a_done()) return true;
            enet_host_flush(a.host);
            enet_host_flush(b.host);
        }
        return a_done();
    }

    // Host listens on an OS-chosen localhost port, client connects to it.
    bool Connect(Peer& host, Peer& client, const std::vector<uint8_t>& a_hostSecret,
                 const std::vector<uint8_t>& a_clientSecret)
    {
        ENetAddress addr{};
        enet_address_set_host(&addr, "127.0.0.1");
        addr.port = 0;
        host.host = enet_host_create(&addr, 1, 3, 0, 0);
        if (!host.host) return false;
        ENetAddress bound{};
        if (enet_socket_get_address(host.host->socket, &bound) != 0) return false;

        client.host = enet_host_create(nullptr, 1, 3, 0, 0);
        if (!client.host) return false;

        if (!host.crypto.Init() || !client.crypto.Init()) return false;
        host.crypto.SetAuthSecret(a_hostSecret.empty() ? nullptr : a_hostSecret.data(), a_hostSecret.size());
        client.crypto.SetAuthSecret(a_clientSecret.empty() ? nullptr : a_clientSecret.data(), a_clientSecret.size());

        ENetAddress target{};
        enet_address_set_host(&target, "127.0.0.1");
        target.port = bound.port;
        client.peer = enet_host_connect(client.host, &target, 3, 0);
        if (!client.peer) return false;

        return PumpUntil(host, client, [&] { return host.crypto.Ready() && client.crypto.Ready(); });
    }

    std::vector<uint8_t> PositionPacket(float x, float y, float z)
    {
        PositionData p{};
        p.x = x; p.y = y; p.z = z;
        p.cellFormID = 0x0001ABCD;
        std::vector<uint8_t> wire(HEADER_SIZE + sizeof(p));
        PacketHeader h{ PKT_POSITION, static_cast<uint16_t>(sizeof(p)) };
        std::memcpy(wire.data(), &h, HEADER_SIZE);
        std::memcpy(wire.data() + HEADER_SIZE, &p, sizeof(p));
        return wire;
    }
}

TEST(Loopback, ConnectAndAgreeOnKeys)
{
    EnetGuard g;
    REQUIRE(g.ok);
    Peer host, client;
    REQUIRE(Connect(host, client, {}, {}));
    CHECK(host.connected);
    CHECK(client.connected);
    CHECK(host.crypto.Ready());
    CHECK(client.crypto.Ready());
}

TEST(Loopback, EncryptedPositionPacketsArriveIntact)
{
    EnetGuard g;
    REQUIRE(g.ok);
    Peer host, client;
    const std::vector<uint8_t> secret = { 's', 'e', 'c', 'r', 'e', 't' };
    REQUIRE(Connect(host, client, secret, secret));

    constexpr int kCount = 50;
    for (int i = 0; i < kCount; ++i)
        REQUIRE(client.SendEncrypted(PositionPacket(float(i), float(-i), 10.0f)));
    REQUIRE(PumpUntil(host, client, [&] { return host.received.size() == kCount; }));
    CHECK_EQ(host.decryptFailures, 0);

    for (int i = 0; i < kCount; ++i) {
        const auto& w = host.received[i];
        REQUIRE(w.size() == HEADER_SIZE + sizeof(PositionData));
        PacketHeader h{};
        std::memcpy(&h, w.data(), HEADER_SIZE);
        CHECK_EQ(int(h.type), int(PKT_POSITION));
        PositionData p{};
        std::memcpy(&p, w.data() + HEADER_SIZE, sizeof(p));
        CHECK_EQ(p.x, float(i));   // reliable channel keeps order
        CHECK_EQ(p.cellFormID, 0x0001ABCDu);
    }

    // And the other direction.
    REQUIRE(host.SendEncrypted(PositionPacket(7, 8, 9)));
    REQUIRE(PumpUntil(host, client, [&] { return client.received.size() == 1; }));
}

TEST(Loopback, MismatchedSecretsCannotTalk)
{
    // Models a relay in the middle without the lobby's secret: both sides
    // "agree" on keys, but nothing decrypts.
    EnetGuard g;
    REQUIRE(g.ok);
    Peer host, client;
    REQUIRE(Connect(host, client, { 1, 2, 3 }, { 9, 9, 9 }));
    REQUIRE(client.SendEncrypted(PositionPacket(1, 2, 3)));
    REQUIRE(PumpUntil(host, client, [&] { return host.decryptFailures == 1; }));
    CHECK(host.received.empty());
}

TEST(Loopback, DisconnectIsSeenByTheOtherSide)
{
    EnetGuard g;
    REQUIRE(g.ok);
    Peer host, client;
    REQUIRE(Connect(host, client, {}, {}));
    enet_peer_disconnect(client.peer, 0);
    REQUIRE(PumpUntil(host, client, [&] { return host.disconnected; }));
    CHECK(host.disconnected);
}
