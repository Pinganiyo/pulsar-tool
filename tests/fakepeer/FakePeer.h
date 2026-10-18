#pragma once

// Headless PULSAR client ("fake peer"). Connects to a PULSAR host over ENet, does
// the same PKT_CRYPTO_HELLO key exchange and PKT_HANDSHAKE as the in-game client,
// then streams PKT_POSITION so the host spawns and drives the remote-player clone.
// No game code: links only ENet + Network/Crypto.cpp. See docs/TESTING.md.
//
// Single-threaded. The caller pumps it: Service() for network events, Tick() for
// the outgoing position stream. tests/fakepeer/main.cpp is the command-line front
// end; tests/test_fakepeer.cpp drives the same class against an in-process host.

#include "Network/Crypto.h"
#include "Network/PacketTypes.h"

#include <enet/enet.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace Pulsar::FakePeer
{
    enum class Mode { kStand, kCircle, kFollow };

    struct Options
    {
        std::string address = "127.0.0.1";
        uint16_t    port    = 7777;            // Network::DEFAULT_PORT
        std::string secret;                    // E2E auth secret (lobby handoff); empty = none (direct-IP play)
        bool        e2e     = true;            // must match the host's e2eEncryption
        std::string name    = "FakePeer";
        bool        sendHandshake    = true;   // PKT_HANDSHAKE: host answers with a quicksave transfer
        bool        sendLoadComplete = true;   // PKT_CLIENT_LOAD_COMPLETE after the save arrives
        bool        sendTimeWeatherReq = true; // PKT_TIME_WEATHER_REQ once, like the real client
        Mode        mode   = Mode::kStand;
        // Starfield positions are in metres (verified in game: interior coords
        // in the tens, NPC walk/run thresholds 0.5 / 3.5 m/s).
        float       offset[3] = { 2.0f, 0.0f, 0.0f };     // from the anchor, metres
        float       radius   = 3.0f;           // circle mode, metres
        float       periodS  = 12.0f;          // circle mode: seconds per lap
        int         followDelayMs = 1000;      // follow mode
        int         rateHz   = 30;
        // Fixed anchor instead of the host's first position (hasAnchor = true).
        bool        hasAnchor = false;
        float       anchor[3] = {};
        uint32_t    anchorCell = 0;
        std::string saveOut;                   // write a received save here (empty = discard)
    };

    // Per-type counters for everything received (index = packet type byte).
    struct Stats
    {
        std::array<uint64_t, 256> recvCount{};
        std::array<uint64_t, 256> recvBytes{};
        uint64_t sentPositions   = 0;
        uint64_t decryptFailures = 0;
        uint64_t savesReceived   = 0;
        uint64_t lastSaveBytes   = 0;
    };

    const char* PacketTypeName(uint8_t a_type);

    class FakePeer
    {
    public:
        using Clock = std::chrono::steady_clock;

        explicit FakePeer(Options a_opts);
        ~FakePeer();
        FakePeer(const FakePeer&) = delete;
        FakePeer& operator=(const FakePeer&) = delete;

        // Optional line logger (no trailing newline). Unset = silent.
        std::function<void(const std::string&)> log;

        // Creates the ENet client and starts connecting. enet_initialize() is the
        // caller's job. Returns false if the host/peer could not be created.
        bool Connect();
        void Disconnect();

        // Drain ENet events, waiting up to a_timeoutMs for the first one.
        void Service(uint32_t a_timeoutMs = 0);
        // Send the next position if one is due. Call often (>= rateHz).
        void Tick();

        bool Connected() const { return m_connected; }
        bool SecureReady() const { return !m_opts.e2e || m_crypto.Ready(); }
        bool HandshakeSent() const { return m_handshakeSent; }
        bool HasHostPosition() const { return m_haveHostPos; }
        // Host sent PKT_HOST_REQUEST_RECONNECT or the link dropped. The front end
        // reconnects after a delay, like the real client.
        bool WantsReconnect() const { return m_wantReconnect; }
        bool DisconnectedByHost() const { return m_disconnected; }
        const Stats& GetStats() const { return m_stats; }
        const Options& GetOptions() const { return m_opts; }

        // "type=count ..." for every type seen so far.
        std::string StatsLine() const;

    private:
        void OnReceive(const uint8_t* a_data, size_t a_len);
        void OnPlain(const uint8_t* a_data, size_t a_len);
        void SendHello();
        void SendApp(const uint8_t* a_data, size_t a_len, bool a_reliable, int a_channel = 0);
        void SendAfterSecure();     // handshake + time/weather request
        bool NextPosition(Network::PositionData& a_out);
        void Log(const std::string& a_line) const { if (log) log(a_line); }

        struct HostSample { Clock::time_point t; Network::PositionData pos; };

        Options                 m_opts;
        ENetHost*               m_host = nullptr;
        ENetPeer*               m_peer = nullptr;
        Network::CryptoChannel  m_crypto;
        bool                    m_connected     = false;
        bool                    m_disconnected  = false;
        bool                    m_wantReconnect = false;
        bool                    m_handshakeSent = false;
        bool                    m_haveHostPos   = false;
        Network::PositionData   m_hostFirst{};
        Network::PositionData   m_hostLast{};
        std::deque<HostSample>  m_hostHistory;     // follow mode
        Clock::time_point       m_start{};
        Clock::time_point       m_nextSend{};
        float                   m_lastXYZ[3] = {};
        bool                    m_haveLast = false;
        Stats                   m_stats;
    };
}
