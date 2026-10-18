#include "FakePeer.h"

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace Pulsar::FakePeer
{
    using namespace Pulsar::Network;

    namespace
    {
        constexpr int   kChannels = 3;            // Network::NUM_CHANNELS
        constexpr float kPi       = 3.14159265358979f;

        std::string Fmt(const char* a_fmt, ...)
        {
            char buf[512];
            va_list ap;
            va_start(ap, a_fmt);
            std::vsnprintf(buf, sizeof(buf), a_fmt, ap);
            va_end(ap);
            return buf;
        }

        // Bethesda heading: rotZ = 0 faces +Y, forward = (sin z, cos z).
        float HeadingTo(float a_dx, float a_dy) { return std::atan2(a_dx, a_dy); }

        bool IsZero(const PositionData& p) { return p.x == 0.0f && p.y == 0.0f && p.z == 0.0f; }
    }

    const char* PacketTypeName(uint8_t a_type)
    {
        switch (a_type) {
#define PT_NAME(n) case n: return #n + 4;   // drop the "PKT_" prefix
            PT_NAME(PKT_HANDSHAKE) PT_NAME(PKT_POSITION) PT_NAME(PKT_ANIMATION) PT_NAME(PKT_INPUT)
            PT_NAME(PKT_INTERACT) PT_NAME(PKT_DROP_ITEM) PT_NAME(PKT_INVENTORY) PT_NAME(PKT_ITEM_TAKEN)
            PT_NAME(PKT_ITEM_DENIED) PT_NAME(PKT_CRAFT_ITEM) PT_NAME(PKT_CRAFT_RESULT) PT_NAME(PKT_EQUIP)
            PT_NAME(PKT_EQUIPMENT) PT_NAME(PKT_DIALOG_START) PT_NAME(PKT_DIALOG_VOTE) PT_NAME(PKT_DIALOG_END)
            PT_NAME(PKT_XP_GAIN) PT_NAME(PKT_PLAYER_STATUS) PT_NAME(PKT_LOADSCREEN_START)
            PT_NAME(PKT_LOADSCREEN_READY) PT_NAME(PKT_SHIP_BLUEPRINT) PT_NAME(PKT_SHIP_BUILDER_ADD)
            PT_NAME(PKT_SHIP_BUILDER_REM) PT_NAME(PKT_SHIP_BUILDER_MOV) PT_NAME(PKT_SHIP_BUILDER_END)
            PT_NAME(PKT_BONES) PT_NAME(PKT_FIRE) PT_NAME(PKT_CLIENT_PROFILE) PT_NAME(PKT_SAVE_TRANSFER)
            PT_NAME(PKT_NPC_STATE) PT_NAME(PKT_VEHICLE_POS) PT_NAME(PKT_LOOT_PICKUP)
            PT_NAME(PKT_CONTAINER_TAKE) PT_NAME(PKT_LOOT_DROP) PT_NAME(PKT_DOOR_STATE) PT_NAME(PKT_LOCK_STATE)
            PT_NAME(PKT_CONTAINER_OPEN) PT_NAME(PKT_GRAB_STATE) PT_NAME(PKT_ACTI_STATE) PT_NAME(PKT_FORM_BLOB)
            PT_NAME(PKT_TRANSIT) PT_NAME(PKT_SPACE_TRANSITION) PT_NAME(PKT_CELL_CONTENT_BATCH)
            PT_NAME(PKT_SHIP_POS) PT_NAME(PKT_FLIGHT_STATE) PT_NAME(PKT_FLIGHT_INPUT) PT_NAME(PKT_PMO_PAYLOAD)
            PT_NAME(PKT_SHIP_TRANSFORM) PT_NAME(PKT_SHIP_FIRE) PT_NAME(PKT_NPC_SHIP_BATCH)
            PT_NAME(PKT_CRUISE_STATE) PT_NAME(PKT_CRUISE_ORIENT) PT_NAME(PKT_DOCK_FINISHED)
            PT_NAME(PKT_DOCK_EXTRATELEPORT) PT_NAME(PKT_DOCK_UNDOCKED) PT_NAME(PKT_DOCK_STATION_XT)
            PT_NAME(PKT_DOCK_FORWARD_BRIDGE_EXPECTED) PT_NAME(PKT_FAST_TRAVEL_SYNC)
            PT_NAME(PKT_CLIENT_LOAD_COMPLETE) PT_NAME(PKT_HOST_REQUEST_RECONNECT)
            PT_NAME(PKT_CIVILIAN_SNAPSHOT) PT_NAME(PKT_QUEST_STAGE) PT_NAME(PKT_QUEST_START)
            PT_NAME(PKT_QUEST_STOP) PT_NAME(PKT_NPC_DAMAGE) PT_NAME(PKT_PLAYER_DOWNED)
            PT_NAME(PKT_PLAYER_REVIVE) PT_NAME(PKT_PLAYER_COMBAT) PT_NAME(PKT_EQUIP_V2)
            PT_NAME(PKT_CORPSE_LOOT) PT_NAME(PKT_BRIDGE_REQUEST) PT_NAME(PKT_KEY_MIRROR) PT_NAME(PKT_SHIP_AV)
            PT_NAME(PKT_TIME_WEATHER) PT_NAME(PKT_TIME_WEATHER_REQ) PT_NAME(PKT_CRYPTO_HELLO)
            PT_NAME(PKT_KILL_XP) PT_NAME(PKT_NPC_SPAWN) PT_NAME(PKT_VOICE)
#undef PT_NAME
            default: return nullptr;
        }
    }

    FakePeer::FakePeer(Options a_opts) : m_opts(std::move(a_opts))
    {
        m_opts.rateHz = std::clamp(m_opts.rateHz, 1, 240);
    }

    FakePeer::~FakePeer() { Disconnect(); }

    bool FakePeer::Connect()
    {
        Disconnect();
        m_disconnected = m_wantReconnect = m_handshakeSent = m_haveHostPos = m_haveLast = false;
        m_hostHistory.clear();

        if (m_opts.e2e) {
            if (!m_crypto.Init()) { Log("crypto init failed"); return false; }
            m_crypto.SetAuthSecret(m_opts.secret.empty() ? nullptr : reinterpret_cast<const uint8_t*>(m_opts.secret.data()),
                                   m_opts.secret.size());
        }

        m_host = enet_host_create(nullptr, 1, kChannels, 0, 0);
        if (!m_host) { Log("enet_host_create failed"); return false; }

        ENetAddress addr{};
        if (enet_address_set_host(&addr, m_opts.address.c_str()) != 0) {
            Log(Fmt("cannot resolve %s", m_opts.address.c_str()));
            return false;
        }
        addr.port = m_opts.port;
        m_peer = enet_host_connect(m_host, &addr, kChannels, 0);
        if (!m_peer) { Log("enet_host_connect failed"); return false; }
        Log(Fmt("connecting to %s:%u (e2e=%s, secret=%s)", m_opts.address.c_str(), m_opts.port,
                m_opts.e2e ? "on" : "OFF", m_opts.secret.empty() ? "none" : "set"));
        return true;
    }

    void FakePeer::Disconnect()
    {
        if (m_host && m_peer && m_connected) {
            enet_peer_disconnect(m_peer, 0);
            // Give the host a moment to see a graceful disconnect instead of a timeout.
            ENetEvent ev;
            const auto end = Clock::now() + std::chrono::milliseconds(300);
            while (Clock::now() < end && enet_host_service(m_host, &ev, 10) >= 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
                if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
            }
        }
        if (m_host) enet_host_destroy(m_host);
        m_host = nullptr;
        m_peer = nullptr;
        m_connected = false;
        m_crypto.Reset();
    }

    void FakePeer::Service(uint32_t a_timeoutMs)
    {
        if (!m_host) return;
        ENetEvent ev;
        uint32_t wait = a_timeoutMs;
        while (m_host && enet_host_service(m_host, &ev, wait) > 0) {
            wait = 0;
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    m_connected = true;
                    m_peer = ev.peer;
                    Log("connected; starting key exchange");
                    if (m_opts.e2e) SendHello();
                    else SendAfterSecure();   // host with e2eEncryption=0 talks plaintext right away
                    break;
                case ENET_EVENT_TYPE_RECEIVE:
                    OnReceive(ev.packet->data, ev.packet->dataLength);
                    enet_packet_destroy(ev.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    Log(Fmt("disconnected by host (data=%u)", ev.data));
                    m_connected = false;
                    m_disconnected = true;
                    m_wantReconnect = true;
                    m_peer = nullptr;
                    m_crypto.Reset();
                    break;
                default:
                    break;
            }
        }
        if (m_host) enet_host_flush(m_host);
    }

    void FakePeer::SendHello()
    {
        auto pub = m_crypto.PublicBlob();
        std::vector<uint8_t> pkt(1 + pub.size());
        pkt[0] = PKT_CRYPTO_HELLO;
        std::memcpy(pkt.data() + 1, pub.data(), pub.size());
        enet_peer_send(m_peer, 0, enet_packet_create(pkt.data(), pkt.size(), ENET_PACKET_FLAG_RELIABLE));
    }

    void FakePeer::SendApp(const uint8_t* a_data, size_t a_len, bool a_reliable, int a_channel)
    {
        if (!m_peer || !m_connected) return;
        const enet_uint32 flags = a_reliable ? ENET_PACKET_FLAG_RELIABLE : enet_uint32{ 0 };
        if (!m_opts.e2e) {
            enet_peer_send(m_peer, static_cast<enet_uint8>(a_channel), enet_packet_create(a_data, a_len, flags));
            return;
        }
        std::vector<uint8_t> enc;
        if (!m_crypto.Encrypt(a_data, a_len, enc)) return;
        enet_peer_send(m_peer, static_cast<enet_uint8>(a_channel), enet_packet_create(enc.data(), enc.size(), flags));
    }

    void FakePeer::SendAfterSecure()
    {
        if (m_opts.sendHandshake) {
            // Same fields as ClientSendHandshake() in Main.cpp.
            HandshakeData hs{};
            hs.versionMajor = 0;
            hs.versionMinor = 1;
            hs.versionPatch = 0;
            hs.dlcFlags     = 0x03;
            strncpy_s(hs.playerName, sizeof(hs.playerName), m_opts.name.c_str(), _TRUNCATE);
            uint8_t buf[64]{};
            PacketWriter w(PKT_HANDSHAKE, buf, sizeof(buf));
            w.Write(hs);
            SendApp(w.Data(), w.Size(), true);
            m_handshakeSent = true;
            Log(Fmt("PKT_HANDSHAKE sent (name='%s'); the host now quicksaves and sends it", hs.playerName));
        }
        if (m_opts.sendTimeWeatherReq) {
            const uint8_t b = PKT_TIME_WEATHER_REQ;
            SendApp(&b, 1, true);
        }
    }

    void FakePeer::OnReceive(const uint8_t* a_data, size_t a_len)
    {
        if (a_len == 0) return;
        if (a_data[0] == PKT_CRYPTO_HELLO && m_opts.e2e && !m_crypto.Ready()) {
            ++m_stats.recvCount[PKT_CRYPTO_HELLO];
            m_stats.recvBytes[PKT_CRYPTO_HELLO] += a_len;
            if (a_len > 1 && m_crypto.DeriveFromPeer(a_data + 1, a_len - 1)) {
                Log("secure channel established");
                SendAfterSecure();
            } else {
                Log("key derivation failed");
            }
            return;
        }
        if (!m_opts.e2e) { OnPlain(a_data, a_len); return; }
        if (!m_crypto.Ready()) { ++m_stats.decryptFailures; return; }

        std::vector<uint8_t> plain;
        if (!m_crypto.Decrypt(a_data, a_len, plain)) {
            // Wrong --secret (or host has E2ERelayAuth with a lobby secret) looks like this.
            if (m_stats.decryptFailures++ == 0)
                Log("decrypt FAILED - check --secret / --no-e2e against the host's settings");
            return;
        }
        OnPlain(plain.data(), plain.size());
    }

    void FakePeer::OnPlain(const uint8_t* a_data, size_t a_len)
    {
        if (a_len == 0) return;
        const uint8_t type = a_data[0];
        const bool first = m_stats.recvCount[type]++ == 0;
        m_stats.recvBytes[type] += a_len;
        if (first) {
            const char* n = PacketTypeName(type);
            Log(Fmt("first %s (0x%02X), %zu bytes", n ? n : "UNKNOWN", type, a_len));
        }

        switch (type) {
            case PKT_POSITION: {
                PacketReader r(a_data, a_len);
                PositionData p{};
                if (!r.IsValid() || !r.Read(p) || IsZero(p)) break;
                if (!m_haveHostPos || p.cellFormID != m_hostLast.cellFormID) {
                    // First fix, or the host changed cell: re-anchor stand/circle there.
                    m_hostFirst = p;
                    Log(Fmt("host at (%.1f, %.1f, %.1f) cell 0x%08X", p.x, p.y, p.z, p.cellFormID));
                }
                m_haveHostPos = true;
                m_hostLast = p;
                if (m_opts.mode == Mode::kFollow) {
                    const auto now = Clock::now();
                    m_hostHistory.push_back({ now, p });
                    const auto keep = now - std::chrono::milliseconds(m_opts.followDelayMs + 2000);
                    while (m_hostHistory.size() > 1 && m_hostHistory[1].t < keep) m_hostHistory.pop_front();
                }
                break;
            }
            case PKT_SAVE_TRANSFER: {
                // [0x70][uint32 size][.sfs bytes] - own framing, no PacketHeader.
                if (a_len < 5) break;
                uint32_t size = 0;
                std::memcpy(&size, a_data + 1, 4);
                const bool whole = size == a_len - 5;
                ++m_stats.savesReceived;
                m_stats.lastSaveBytes = size;
                Log(Fmt("save transfer: %u bytes%s", size, whole ? "" : " (SIZE MISMATCH)"));
                if (!m_opts.saveOut.empty() && whole) {
                    std::ofstream f(m_opts.saveOut, std::ios::binary | std::ios::trunc);
                    f.write(reinterpret_cast<const char*>(a_data + 5), size);
                    Log(Fmt("  written to %s", m_opts.saveOut.c_str()));
                }
                if (m_opts.sendLoadComplete) {
                    // Pretend we loaded it. Host answers by force-respawning the clone.
                    uint8_t buf[32]{};
                    PacketWriter w(PKT_CLIENT_LOAD_COMPLETE, buf, sizeof(buf));
                    w.Write(uint8_t{ 0 });   // reason 0 = initial connect
                    w.Write(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now().time_since_epoch()).count()));
                    SendApp(w.Data(), w.Size(), true);
                    Log("PKT_CLIENT_LOAD_COMPLETE sent");
                }
                break;
            }
            case PKT_HOST_REQUEST_RECONNECT:
                Log("host asked us to reconnect");
                m_wantReconnect = true;
                break;
            default:
                break;
        }
    }

    bool FakePeer::NextPosition(PositionData& a_out)
    {
        a_out = PositionData{};
        float base[3];
        uint32_t cell = 0;
        if (m_opts.hasAnchor) {
            std::copy(m_opts.anchor, m_opts.anchor + 3, base);
            cell = m_opts.anchorCell;
        } else if (m_haveHostPos) {
            base[0] = m_hostFirst.x; base[1] = m_hostFirst.y; base[2] = m_hostFirst.z;
            cell = m_hostLast.cellFormID;   // must match the host's cell or interiors suppress the apply
        } else {
            return false;
        }

        const float t = std::chrono::duration<float>(Clock::now() - m_start).count();
        float x = base[0] + m_opts.offset[0];
        float y = base[1] + m_opts.offset[1];
        float z = base[2] + m_opts.offset[2];
        float heading = 0.0f;

        switch (m_opts.mode) {
            case Mode::kStand:
                if (m_haveHostPos) heading = HeadingTo(m_hostLast.x - x, m_hostLast.y - y);   // face the host
                break;
            case Mode::kCircle: {
                const float a = 2.0f * kPi * t / std::max(m_opts.periodS, 1.0f);
                x += m_opts.radius * std::cos(a);
                y += m_opts.radius * std::sin(a);
                heading = HeadingTo(-std::sin(a), std::cos(a));   // tangent, counter-clockwise
                break;
            }
            case Mode::kFollow: {
                if (m_hostHistory.empty()) {
                    if (!m_haveHostPos) return false;
                    x = m_hostLast.x + m_opts.offset[0]; y = m_hostLast.y + m_opts.offset[1];
                    z = m_hostLast.z + m_opts.offset[2]; heading = m_hostLast.rotZ;
                    break;
                }
                const auto target = Clock::now() - std::chrono::milliseconds(m_opts.followDelayMs);
                const HostSample* s = &m_hostHistory.front();
                for (const auto& h : m_hostHistory) { if (h.t <= target) s = &h; else break; }
                x = s->pos.x + m_opts.offset[0];
                y = s->pos.y + m_opts.offset[1];
                z = s->pos.z + m_opts.offset[2];
                heading = s->pos.rotZ;
                cell = s->pos.cellFormID;
                break;
            }
        }

        // Ground speed from the last sent point drives the walk animation.
        float speed = 0.0f;
        if (m_haveLast) {
            const float dx = x - m_lastXYZ[0], dy = y - m_lastXYZ[1];
            speed = std::sqrt(dx * dx + dy * dy) * static_cast<float>(m_opts.rateHz);
            if (speed > 2000.0f) speed = 0.0f;   // teleport/re-anchor, not movement
        }
        m_lastXYZ[0] = x; m_lastXYZ[1] = y; m_lastXYZ[2] = z;
        m_haveLast = true;

        const bool moving = speed > 5.0f;
        a_out.x = x; a_out.y = y; a_out.z = z;
        a_out.rotZ = heading;
        a_out.speed = speed;
        a_out.cellFormID = cell;
        a_out.vars.speed = speed;
        a_out.vars.currentSpeed = speed;
        a_out.vars.iSyncIdleLocomotion = moving ? 1 : 0;
        a_out.vars.boolFlags = moving ? 0 : 1;               // bit 0 = IsCompletelyStopped
        a_out.animFlags = static_cast<uint16_t>((moving ? 1 : 0) | (moving ? 0 : (1 << 7)));
        return true;
    }

    void FakePeer::Tick()
    {
        if (!m_connected || !SecureReady()) return;
        const auto now = Clock::now();
        const auto period = std::chrono::microseconds(1000000 / m_opts.rateHz);
        if (now < m_nextSend) return;
        m_nextSend = (m_nextSend + period < now) ? now + period : m_nextSend + period;

        if (m_stats.sentPositions == 0) m_start = now;
        PositionData p{};
        if (!NextPosition(p)) return;
        uint8_t buf[128]{};
        PacketWriter w(PKT_POSITION, buf, sizeof(buf));
        w.Write(p);
        SendApp(w.Data(), w.Size(), false);   // unreliable, like the game
        if (m_stats.sentPositions++ == 0)
            Log(Fmt("streaming PKT_POSITION at %d Hz from (%.1f, %.1f, %.1f) cell 0x%08X",
                    m_opts.rateHz, p.x, p.y, p.z, p.cellFormID));
    }

    std::string FakePeer::StatsLine() const
    {
        std::string s = Fmt("sent POSITION=%llu | recv", static_cast<unsigned long long>(m_stats.sentPositions));
        bool any = false;
        for (int i = 0; i < 256; ++i) {
            if (!m_stats.recvCount[i]) continue;
            const char* n = PacketTypeName(static_cast<uint8_t>(i));
            s += n ? Fmt(" %s=%llu", n, static_cast<unsigned long long>(m_stats.recvCount[i]))
                   : Fmt(" 0x%02X=%llu", i, static_cast<unsigned long long>(m_stats.recvCount[i]));
            any = true;
        }
        if (!any) s += " (nothing)";
        if (m_stats.decryptFailures)
            s += Fmt(" | decrypt failures=%llu", static_cast<unsigned long long>(m_stats.decryptFailures));
        return s;
    }
}
