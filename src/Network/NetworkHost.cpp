#include "PCH.h"
#include "Network/NetworkHost.h"
#include "Network/RelayProtocol.h"
#include "Network/PacketTypes.h"
#include "VoiceBridge/VoiceBridge.h"
#include "Config.h"
#include "Companion/CompanionManager.h"
#include "Companion/InteractionManager.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "FlashlightSync/FlashlightSync.h"
#include "SaveScanner/SaveScanner.h"
#include "SaveScanner/SavePatcher.h"
#include "WorldSync/WorldSync.h"
#include "Cell3DAttach.h"
#include "NPCSync/NPCSync.h"
#include "VehicleSync/VehicleSync.h"
#include "WorldSync/LootSync.h"
#include "WorldSync/DoorSync.h"
#include "WorldSync/ActiSync.h"
#include "WorldSync/LockSync.h"
#include "WorldSync/GrabSync.h"
#include "Forms/SaveBuffer.h"
#include "InventorySync/InventorySync.h"
#include "InventorySync/DirectRestore.h"
#include "Lobby/LobbyHandoff.h"
#include "TransitSync/TransitSync.h"
#include "SpaceSync/FlightSync.h"
#include "Network/VersionCompat.h"
#include "Network/KeyExchange.h"
#include "Plugin.h"
#include "SpaceSync/ShipCombatSync.h"
#include "SpaceSync/SpaceSync.h"
#include "SpaceSync/DockSync.h"
#include "SpaceSync/FastTravelSync.h"
#include "CellContentSync/CellContentSync.h"
#include "QuestSync/QuestSync.h"
#include "DialogSync/DialogSync.h"
#include "SpawnSync/SpawnSync.h"
#include "Coop/CoopMortality.h"
#include "Coop/KillXpSync.h"
#include "TimeWeatherSync/TimeWeatherSync.h"
#include "RemotePlayer/EquipSync.h"   // F.2-v2: PKT_EQUIP_V2 receiver
#include "NASLog.h"

#include <fstream>
#include <cstring>

#include <RE/P/PlayerCharacter.h>
#include <RE/N/NiPoint.h>
#include "Animation/BoneSync.h"

using namespace std::string_view_literals;

// Defined in Main.cpp - camera pitch from engine thread hook
extern std::atomic<float> g_playerPitch;

namespace Pulsar::Network
{
    static NetworkHost g_network;

    // Packet tracking counters
    static std::atomic<uint64_t> s_pktSent{0};
    static std::atomic<uint64_t> s_pktRecv{0};
    static std::atomic<uint64_t> s_pktRecvPos{0};
    static uint64_t s_lastPktLog{0};

    // ── P2P hole-punch rendezvous (set by the ENet intercept, read by Update) ──
    // Peer endpoint is packed (ip<<16 | port) into ONE atomic so the reader never
    // sees a torn ip/port pair.
    static std::atomic<bool>     g_rzPeerKnown{ false };
    static std::atomic<uint64_t> g_rzPeer{ 0 };       // (ipNetOrder << 16) | portHostOrder
    // Expected relay identity, set in SetRelay - used to AUTHENTICATE incoming PEER.
    static std::atomic<uint32_t> g_expectRelayHost{ 0 };
    static std::atomic<uint16_t> g_expectRelayPort{ 0 };
    static uint8_t               g_expectSession[Relay::kSessionBytes] = {};

    // ENet calls this for every raw datagram BEFORE protocol processing. We only
    // honor a PEER that (a) actually came FROM the relay endpoint and (b) carries
    // OUR session - otherwise an attacker could forge a PEER to point us at any IP
    // (connection hijack / DDoS reflection). Consume our control magic (return 1);
    // pass everything else to ENet (return 0). Runs on the ENet service thread.
    static int ENET_CALLBACK RelayIntercept(ENetHost* a_host, ENetEvent* /*a_event*/)
    {
        const auto*  d = a_host->receivedData;
        const size_t n = a_host->receivedDataLength;
        if (!Relay::IsControlMagic(d, n)) return 0;     // not ours → let ENet handle
        // (a) source must be the relay (only the relay legitimately sends PEER).
        if (a_host->receivedAddress.host != g_expectRelayHost.load(std::memory_order_relaxed) ||
            a_host->receivedAddress.port != g_expectRelayPort.load(std::memory_order_relaxed))
            return 1;                                   // spoofed/foreign - drop, don't trust
        uint32_t ip; uint16_t port;
        if (Relay::ParsePeer(d, n, ip, port)) {
            // (b) session must match ours (PEER carries the 16-byte session at d[9..24]).
            if (std::memcmp(d + 9, g_expectSession, Relay::kSessionBytes) != 0) return 1;
            g_rzPeer.store((static_cast<uint64_t>(ip) << 16) | port, std::memory_order_release);
            g_rzPeerKnown.store(true, std::memory_order_release);
        }
        return 1;  // swallow authenticated/stray control
    }

    // Reject endpoints we must never punch/connect to (defence-in-depth against a
    // forged-or-compromised relay aiming us at a reflection victim / junk address).
    static bool IsSaneEndpoint(uint32_t ipNetOrder, uint16_t port)
    {
        if (port == 0) return false;
        const uint8_t a = ipNetOrder & 0xFF;            // first octet (net order = a.b.c.d)
        if (a == 0 || a == 127 || a >= 224) return false;        // 0/8, loopback, multicast/255
        if (ipNetOrder == 0xFFFFFFFF) return false;              // broadcast
        return true;
    }

    NetworkHost& GetSingleton()
    {
        return g_network;
    }

    // Raw SEH-safe read of player->currentProcess->middleHigh->occupiedFurniture
    // handle value. Non-zero = seated. Avoids GetSmartPointer which is stale
    // on 1.16.236 for TESObjectREFR handles.
    static uint32_t SafeReadPlayerFurnHandle(RE::Actor* a_actor)
    {
        __try {
            if (!a_actor) return 0;
            auto* a = reinterpret_cast<uint8_t*>(a_actor);
            auto* proc = *reinterpret_cast<uint8_t**>(a + 0x228);
            if (!proc) return 0;
            auto* mh = *reinterpret_cast<uint8_t**>(proc + 0x08);
            if (!mh) return 0;
            return *reinterpret_cast<uint32_t*>(mh + 0x428);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static void FillAnimationData(RE::Actor* a_actor, PositionData& a_pos)
    {
        if (!a_actor) return;

        RE::BSFixedString speedVar("Speed");
        RE::BSFixedString dirVar("Direction");
        RE::BSFixedString idleLocoVar("iSyncIdleLocomotion");
        RE::BSFixedString sprintVar("iSyncSprintState");
        RE::BSFixedString relaxVar("iSyncRelaxReadySneak");
        RE::BSFixedString stoppedVar("IsCompletelyStopped");
        RE::BSFixedString moveStartVar("bPlayerMoveStartActive");

        RE::BSFixedString lookVar("LookHeadingOffset");

        bool speedOk = a_actor->GetGraphVariableImplFloat(speedVar, a_pos.speed);
        bool dirOk = a_actor->GetGraphVariableImplFloat(dirVar, a_pos.direction);
        a_actor->GetGraphVariableImplFloat(lookVar, a_pos.lookHeading);

        int32_t idleLoco = 0, sprint = 0, relax = 0;
        bool stopped = false, moveStart = false;

        a_actor->GetGraphVariableImplInt(idleLocoVar, idleLoco);
        a_actor->GetGraphVariableImplInt(sprintVar, sprint);
        a_actor->GetGraphVariableImplInt(relaxVar, relax);
        a_actor->GetGraphVariableImplBool(stoppedVar, stopped);
        a_actor->GetGraphVariableImplBool(moveStartVar, moveStart);

        // 2026-04-20: capture fire-state graph vars for shooting sync
        RE::BSFixedString gunActionsVar("iSyncGunActions");
        RE::BSFixedString gunDownVar("iSyncGunDown");
        int32_t gunActions = 0, gunDown = 0;
        a_actor->GetGraphVariableImplInt(gunActionsVar, gunActions);
        a_actor->GetGraphVariableImplInt(gunDownVar, gunDown);
        a_pos.vars.iSyncGunActions = gunActions;
        a_pos.vars.iSyncGunDown    = gunDown;

        // Weapon state: actorState2 bits 0-1 (proven to work for player)
        uint8_t weaponState = static_cast<uint8_t>(a_actor->actorState2 & 0x3);
        uint32_t flags0xF8 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(a_actor) + 0xF8);
        bool engineWeapDrawn = ((flags0xF8 & 0x1E0000) == 0x60000);
        static bool s_weaponDrawnSticky = false;
        static uint64_t s_sheatheStart = 0;
        if (weaponState >= 2) {
            s_weaponDrawnSticky = true;
            s_sheatheStart = 0;
        } else if (s_weaponDrawnSticky && weaponState == 0) {
            uint64_t now = GetTickCount64();
            if (s_sheatheStart == 0) s_sheatheStart = now;
            if (now - s_sheatheStart > 50) {
                s_weaponDrawnSticky = false;
            }
        }

        // Sneak detection via real engine IsSneaking (REL::ID 100547, RVA 0x18E7660)
        using IsSneaking_t = bool(*)(RE::Actor*);
        static REL::Relocation<IsSneaking_t> realIsSneaking{ REL::ID(100547) };
        bool isSneaking = realIsSneaking(a_actor);

        if (isSneaking) {
            relax = 2;  // Sneak takes priority for crouch animation
        } else if (s_weaponDrawnSticky) {
            relax = 1;  // Weapon drawn (standing)
        }

        // Read LookAt graph variables for head tracking sync
        float lookAtPitch = 0.0f, lookAtHeading = 0.0f;
        bool headTrackActive = false;
        RE::BSFixedString lookAtPitchVar("LookAtPitchVar");
        RE::BSFixedString lookAtHeadingVar("LookAtHeadingVar");
        RE::BSFixedString headTrackVar("bHeadTrackingActive");
        a_actor->GetGraphVariableImplFloat(lookAtPitchVar, lookAtPitch);
        a_actor->GetGraphVariableImplFloat(lookAtHeadingVar, lookAtHeading);
        a_actor->GetGraphVariableImplBool(headTrackVar, headTrackActive);

        // Debug log
        static uint64_t s_lastFillLog = 0;
        uint64_t fillNow = GetTickCount64();
        bool localSeated = (SafeReadPlayerFurnHandle(a_actor) != 0);
        if (fillNow - s_lastFillLog > 3000) {
            s_lastFillLog = fillNow;
            PLOG_INFO("[PULSAR] FillAnim: spd={:.1f} dir={:.1f} as2weap={} relax={} snk={} seated={} pitch={:.2f}"sv,
                a_pos.speed, a_pos.direction, weaponState, relax,
                isSneaking, localSeated, lookAtPitch);
        }

        // Seated flag - non-zero occupiedFurniture handle means player is
        // sitting in a FURN. Bit 10 of animFlags.
        bool seatedFlag = (SafeReadPlayerFurnHandle(a_actor) != 0);

        a_pos.animFlags =
            (static_cast<uint16_t>(idleLoco & 0x3))        |
            (static_cast<uint16_t>(sprint & 0x3)   << 2)   |
            (static_cast<uint16_t>(relax & 0x7)    << 4)   |
            (static_cast<uint16_t>(stopped ? 1 : 0) << 7)  |
            (static_cast<uint16_t>(moveStart ? 1 : 0) << 8) |
            (static_cast<uint16_t>(s_weaponDrawnSticky ? 1 : 0) << 9) |
            (static_cast<uint16_t>(seatedFlag ? 1 : 0) << 10) |
            // bit 12 - helmet flashlight on (player+0xEB0 != null)
            (static_cast<uint16_t>(Pulsar::FlashlightSync::IsLocalOn() ? 1 : 0) << 12);

        // Fill AnimVarSnapshot with all graph variables
        a_pos.vars.speed      = a_pos.speed;
        a_pos.vars.direction  = a_pos.direction;
        a_pos.vars.lookHeading = lookAtHeading;
        // Read pitch from hook's atomic (fCameraPitch only works in engine thread)
        a_pos.vars.lookPitch = ::g_playerPitch.load();
        a_pos.rotX = a_pos.vars.lookPitch;

        RE::BSFixedString turnDeltaVar("TurnDelta");
        RE::BSFixedString curSpeedVar("CurrentSpeed");
        a_actor->GetGraphVariableImplFloat(turnDeltaVar, a_pos.vars.turnDelta);
        a_actor->GetGraphVariableImplFloat(curSpeedVar, a_pos.vars.currentSpeed);

        int32_t iState = 0;
        RE::BSFixedString iStateVar("iState");
        a_actor->GetGraphVariableImplInt(iStateVar, iState);

        int32_t jumpState = 0;
        RE::BSFixedString jumpVar("iSyncJumpState");
        a_actor->GetGraphVariableImplInt(jumpVar, jumpState);

        a_pos.vars.iState = iState;
        a_pos.vars.iSyncIdleLocomotion = idleLoco;
        a_pos.vars.iSyncSprintState    = sprint;
        a_pos.vars.iSyncRelaxReadySneak = relax;
        a_pos.vars.iSyncJumpState       = jumpState;
        a_pos.vars.boolFlags = (stopped ? 1 : 0) | (moveStart ? 2 : 0);
    }

    NetworkHost::~NetworkHost()
    {
        Shutdown();
    }

    void NetworkHost::SetRelay(const std::string& a_relayAddr, const std::string& a_sessionHex)
    {
        m_relayMode = false;
        if (a_relayAddr.empty() || a_sessionHex.empty()) return;
        if (!Relay::DecodeSession(a_sessionHex, m_relaySession)) {
            PLOG_DIAG("[PULSAR] [Relay] bad session hex - falling back to direct"sv);
            return;
        }
        auto colon = a_relayAddr.rfind(':');
        if (colon == std::string::npos) {
            PLOG_DIAG("[PULSAR] [Relay] relay addr needs host:port - direct fallback"sv);
            return;
        }
        std::string host = a_relayAddr.substr(0, colon);
        m_relayAddress = ENetAddress{};
        enet_address_set_host(&m_relayAddress, host.c_str());
        m_relayAddress.port =
            static_cast<uint16_t>(std::strtoul(a_relayAddr.c_str() + colon + 1, nullptr, 10));
        m_relayMode = true;
        // Authentication context for the PEER intercept (only this relay + this
        // session may set our direct-connect target).
        g_expectRelayHost.store(m_relayAddress.host, std::memory_order_relaxed);
        g_expectRelayPort.store(m_relayAddress.port, std::memory_order_relaxed);
        std::memcpy(g_expectSession, m_relaySession, Relay::kSessionBytes);
        // Public-alpha safety valve: PULSAR_FORCE_RELAY=1 forces the proven
        // relay-always path (skip P2P hole-punch).
        char* fr = nullptr; size_t frn = 0;
        if (_dupenv_s(&fr, &frn, "PULSAR_FORCE_RELAY") == 0 && fr) { m_forceRelay = (fr[0] == '1'); free(fr); }
        PLOG_DIAG("[PULSAR] [Relay] enabled via {} (forceRelay={})"sv, a_relayAddr, m_forceRelay);
    }

    void NetworkHost::SendRelayControl(uint8_t a_type)
    {
        if (!m_relayMode || !m_host) return;
        auto pkt = Relay::MakeControl(a_type, m_relaySession);
        ENetBuffer eb{};
        eb.data       = pkt.data();
        eb.dataLength = pkt.size();
        enet_socket_send(m_host->socket, &m_relayAddress, &eb, 1);
    }

    // Hole-punch: send a few raw datagrams to the peer's endpoint on the SAME
    // socket ENet uses, so the NAT mapping opened is the one ENet will send/recv on.
    void NetworkHost::PunchTo(const ENetAddress& a_ep, int a_count)
    {
        if (!m_host) return;
        uint8_t one = 0x50;  // 'P' - content irrelevant; the act of sending opens our mapping
        ENetBuffer eb{};
        eb.data = &one; eb.dataLength = 1;
        for (int i = 0; i < a_count; ++i) enet_socket_send(m_host->socket, &a_ep, &eb, 1);
    }

    bool NetworkHost::StartHost(uint16_t a_port)
    {
        if (!m_initialized) {
            if (enet_initialize() != 0) {
                PLOG_WARN("ENet initialization failed"sv);
                return false;
            }
            m_initialized = true;
        }

        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        address.port = a_port;

        m_host = enet_host_create(&address, MAX_CLIENTS, NUM_CHANNELS, 0, 0);
        if (!m_host) {
            PLOG_WARN("Failed to create ENet host on port {}"sv, a_port);
            return false;
        }
        if (m_relayMode && !m_forceRelay) {
            m_host->intercept = &RelayIntercept;
            // Re-arm rendezvous for THIS session (so re-hosting / host→join→host
            // doesn't keep stale state that suppresses the punch).
            m_rz = Rz::kNone;
            g_rzPeerKnown.store(false, std::memory_order_release);
        }

        m_isHost = true;
        if (m_relayMode) {
            SendRelayControl(Relay::kTypeRegister);
            m_relayKeepaliveAt = GetTickCount64() + 15000;
            PLOG_DIAG("[PULSAR] [Relay] host registered session"sv);
        }
        PLOG_INFO("[PULSAR] Host started on port {}"sv, a_port);
        return true;
    }

    bool NetworkHost::Connect(const char* a_address, uint16_t a_port)
    {
        // L.7 V3.1 - if the player isn't fully spawned yet (main menu,
        // no save loaded → null inventoryList) we DEFER the connect instead
        // of refusing. Update() drains m_pendingConnectActive on the first
        // tick where IsPlayerReadyForConnect() is true. This lets:
        // SFSE plugin-load auto-connect (Main.cpp:1543/1554) succeed and
        //     start the NetworkLoop thread even when no save is loaded yet
        // User's save-load complete → next tick auto-fires the real connect
        // DirectRestore::TickPeriodicCapture pre-populates the cache so
        //     the resulting save bridge restores client identity correctly.
        if (!Pulsar::InventorySync::DirectRestore::IsPlayerReadyForConnect()) {
            {
                std::lock_guard lk(m_pendingConnectMu);
                m_pendingConnectAddress = a_address;
                m_pendingConnectPort    = a_port;
            }
            m_pendingConnectActive.store(true, std::memory_order_release);
            m_isHost = false;
            PLOG_INFO(
                "[PULSAR] Connect queued (waiting for save load - "
                "will auto-connect when player is in-game)"sv);
            // Return true so callers start their loops (g_running etc.).
            return true;
        }

        return DoConnectInternal(a_address, a_port);
    }

    bool NetworkHost::DoConnectInternal(const char* a_address, uint16_t a_port)
    {
        if (!m_initialized) {
            if (enet_initialize() != 0) {
                PLOG_WARN("ENet initialization failed"sv);
                return false;
            }
            m_initialized = true;
        }

        m_host = enet_host_create(nullptr, 1, NUM_CHANNELS, 0, 0);
        if (!m_host) {
            PLOG_WARN("Failed to create ENet client"sv);
            return false;
        }
        if (m_relayMode && !m_forceRelay) m_host->intercept = &RelayIntercept;

        ENetAddress address{};
        if (m_relayMode && !m_forceRelay) {
            // P2P: announce to the relay, then WAIT for the PEER endpoint. Update()
            // drives the punch + direct connect (and the relay fallback). Do NOT
            // connect to the relay yet.
            g_rzPeerKnown.store(false, std::memory_order_release);
            for (int i = 0; i < 3; ++i) SendRelayControl(Relay::kTypeJoin);
            m_rz = Rz::kAwaitPeer;
            m_rzDeadline = GetTickCount64() + 2000;   // kPeerWaitMs
            m_isHost = false;
            m_lastAddress = a_address; m_lastPort = a_port;
            PLOG_DIAG("[PULSAR] [Relay] client: rendezvous - awaiting peer endpoint"sv);
            return true;
        } else if (m_relayMode) {
            // FORCE_RELAY - today's verified path verbatim.
            address = m_relayAddress;
            for (int i = 0; i < 3; ++i) SendRelayControl(Relay::kTypeJoin);  // beat reordering
            PLOG_DIAG("[PULSAR] [Relay] client joined session, connecting via relay (forced)"sv);
        } else {
            enet_address_set_host(&address, a_address);
            address.port = a_port;
        }

        m_peer = enet_host_connect(m_host, &address, NUM_CHANNELS, 0);
        if (!m_peer) {
            PLOG_DIAG("[PULSAR] Failed to initiate connection to {}:{}"sv, a_address, a_port);
            return false;
        }

        m_isHost = false;
        m_lastAddress = a_address;
        m_lastPort = a_port;
        PLOG_DIAG("[PULSAR] Connecting to {}:{}..."sv, a_address, a_port);
        return true;
    }

    void NetworkHost::Update()
    {
        // Relay: keep the host's NAT pinhole open with periodic REGISTER until
        // (and after) the client connects. ENet traffic also holds it once flowing.
        if (m_relayMode && m_isHost && m_host) {
            uint64_t nowMs = GetTickCount64();
            if (nowMs >= m_relayKeepaliveAt) {
                std::lock_guard lk(m_enetMutex);
                SendRelayControl(Relay::kTypeRegister);
                m_relayKeepaliveAt = nowMs + 15000;
            }
        }

        // P2P host: once the relay reports the client's endpoint, punch toward it
        // (one-shot) so the client's direct connect can reach us.
        if (m_relayMode && !m_forceRelay && m_isHost && m_host && m_rz == Rz::kNone &&
            g_rzPeerKnown.load(std::memory_order_acquire)) {
            const uint64_t v = g_rzPeer.load(std::memory_order_acquire);
            const uint32_t ip = static_cast<uint32_t>(v >> 16);
            const uint16_t port = static_cast<uint16_t>(v & 0xFFFF);
            m_rz = Rz::kAwaitPeer;  // host: mark handled (no further rendezvous states)
            if (IsSaneEndpoint(ip, port)) {
                ENetAddress cep{}; cep.host = ip; cep.port = port;
                { std::lock_guard lk(m_enetMutex); PunchTo(cep, 5); }
                PLOG_DIAG("[PULSAR] [Relay] host punched toward client"sv);
            } else {
                PLOG_WARN("[PULSAR] [Relay] host: ignoring insane peer endpoint"sv);
            }
        }

        // P2P client: rendezvous → punch → direct connect → relay fallback.
        if (m_relayMode && !m_forceRelay && !m_isHost &&
            (m_rz == Rz::kAwaitPeer || m_rz == Rz::kDirectConnecting)) {
            const uint64_t now = GetTickCount64();
            std::lock_guard lk(m_enetMutex);
            if (m_rz == Rz::kAwaitPeer) {
                // Load the KNOWN flag (acquire) BEFORE the packed endpoint, so the
                // acquire fences the peer read against the intercept's release store
                // (matches the host-punch block). Skip the load entirely until known.
                const bool known = g_rzPeerKnown.load(std::memory_order_acquire);
                const uint64_t v = known ? g_rzPeer.load(std::memory_order_acquire) : 0;
                const uint32_t ip = static_cast<uint32_t>(v >> 16);
                const uint16_t port = static_cast<uint16_t>(v & 0xFFFF);
                if (known && IsSaneEndpoint(ip, port)) {
                    m_directAddress = ENetAddress{};
                    m_directAddress.host = ip;
                    m_directAddress.port = port;
                    PunchTo(m_directAddress, 5);                          // kPunchCount
                    m_peer = enet_host_connect(m_host, &m_directAddress, NUM_CHANNELS, 0);
                    m_rz = Rz::kDirectConnecting;
                    m_rzDeadline = now + 3000;                            // kDirectMs
                    PLOG_DIAG("[PULSAR] [Relay] punch + direct connect to peer"sv);
                } else if (now >= m_rzDeadline) {
                    m_peer = enet_host_connect(m_host, &m_relayAddress, NUM_CHANNELS, 0);
                    m_rz = Rz::kDoneRelay;
                    PLOG_DIAG("[PULSAR] [Relay] no peer endpoint - relay fallback"sv);
                }
            } else { // kDirectConnecting
                if (m_connectionEstablished.load(std::memory_order_acquire)) {
                    m_rz = Rz::kDoneDirect;
                    PLOG_DIAG("[PULSAR] [Relay] DIRECT P2P established"sv);
                } else if (now >= m_rzDeadline) {
                    if (m_peer) { enet_peer_reset(m_peer); m_peer = nullptr; }
                    m_peer = enet_host_connect(m_host, &m_relayAddress, NUM_CHANNELS, 0);
                    m_rz = Rz::kDoneRelay;
                    PLOG_DIAG("[PULSAR] [Relay] direct timed out - relay fallback"sv);
                }
            }
        }

        // L.7 V3.1 - drain queued connect once the player is in-game.
        // Runs BEFORE the m_host null-check because the whole point is that
        // the connect was deferred (m_host still null). IsPlayerReadyForConnect
        // is SEH-wrapped so reading player pointers off the net thread is safe.
        if (m_pendingConnectActive.load(std::memory_order_acquire) &&
            Pulsar::InventorySync::DirectRestore::IsPlayerReadyForConnect())
        {
            std::string addr;
            uint16_t    port;
            {
                std::lock_guard lk(m_pendingConnectMu);
                addr = m_pendingConnectAddress;
                port = m_pendingConnectPort;
            }
            m_pendingConnectActive.store(false, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] Save loaded - dequeuing pending Connect to {}:{}"sv,
                addr, port);
            DoConnectInternal(addr.c_str(), port);
        }

        if (!m_host) {
            return;
        }

        // Host position send + client pending teleport moved to Main.cpp game thread.
        // Rationale: FillAnimationData calls GetGraphVariableImpl which touches the
        // animation graph (same NiNode family as bone reads). Running that off the
        // game thread races with the engine's own animation updates. SetPosition
        // triggers physics-settle callbacks which are also game-thread-only.
        // See RemotePlayerHostTick() / ClientTeleportTick() in Main.cpp.

        // L.7 V9.3 - drain deferred reconnect on game thread. Don't call
        // Disconnect() directly - that destroys ENet host/peer synchronously
        // and races the network thread which is still using them. Instead
        // initiate a graceful peer-disconnect; ENet's event loop dispatches
        // OnDisconnect naturally, which runs the full cleanup (NPCSync
        // reset + clone despawn + auto-reconnect schedule) on the network
        // thread without races.
        if (m_pendingReconnect.exchange(false, std::memory_order_acq_rel)) {
            PLOG_INFO(
                "[PULSAR] [L.7 V9.3] game-thread draining pending reconnect - "
                "calling enet_peer_disconnect (OnDisconnect cleanup will "
                "fire async)"sv);
            NASLOG("L.7 V9.3 graceful disconnect requested");
            if (m_peer) {
                enet_peer_disconnect(m_peer, 0);
                if (m_host) enet_host_flush(m_host);
            }
        }

        // Periodic packet stats logging (safe from any thread - just atomics)
        uint64_t now = GetTickCount64();
        if (now - s_lastPktLog > 5000) {
            s_lastPktLog = now;
            PLOG_INFO("[PULSAR] PKT stats: sent={} recv={} recvPos={}"sv,
                      s_pktSent.load(), s_pktRecv.load(), s_pktRecvPos.load());
        }

        // Host: poll for fresh quicksave and send when file size has stabilized
        if (m_quicksavePending && m_isHost && m_peer) {
            uint64_t now = GetTickCount64();
            uint64_t elapsed = now - m_quicksaveStartTick;

            // (2026-06-23) - ABORT (do NOT send a stale save) after 30s without
            // a fresh quicksave. The host being IN COMBAT blocks the engine quicksave
            // (the trigger is consumed but no file is written), so the fresh save never
            // materialises. The old fallback here sent SendSaveToClient() = the newest
            // file ON DISK with no freshness check - which is an OLD save (observed
            // 2026-06-23: an in-space save from earlier testing) → the client loaded it
            // and ended up in space instead of at the host ("client ist nie beim host
            // gelandet"). Mislocating the client is worse than not bridging, so we abort
            // cleanly. The host re-bridges (auto-follow / F8) once it is OUT of combat.
            if (elapsed > 30000) {
                PLOG_WARN(
                    "[PULSAR] Quicksave timeout (30s) - no FRESH save (host likely in "
                    "combat; engine blocks quicksave). ABORTING bridge instead of "
                    "sending a stale save (would mislocate the client). Retry once out "
                    "of combat."sv);
                NASLOG("Quicksave timeout - bridge aborted (no fresh save, host in combat?)");
                m_quicksavePending = false;
            } else if (now - m_quicksaveLastPoll >= 300) {
                m_quicksaveLastPoll = now;

                std::string newest = SaveScanner::GetMostRecentSavePath();
                if (!newest.empty()) {
                    std::error_code ec;
                    auto newestTime = std::filesystem::last_write_time(newest, ec);
                    auto newestSize = std::filesystem::file_size(newest, ec);
                    bool isFresh = !ec && newestTime > m_quicksavePreTime;

                    if (isFresh) {
                        if (m_quicksaveCandidate != newest || newestSize != m_quicksaveCandidateSize) {
                            m_quicksaveCandidate = newest;
                            m_quicksaveCandidateSize = newestSize;
                            m_quicksaveSizeStableSince = now;
                        } else if (now - m_quicksaveSizeStableSince >= 500 && newestSize > 0) {
                            // Size stable + non-zero. Final completeness gate (#co-loc fix
                            // 2026-06-23): the file must FULLY decompress before we send it.
                            // The old "client rejects the triggered quicksave" was an
                            // incomplete file sent mid-write; only a complete save goes out.
                            std::vector<uint8_t> decompProbe;
                            SaveScanner::BcpsMeta metaProbe{};
                            if (SaveScanner::LoadAndDecompressAll(newest, decompProbe, metaProbe)) {
                                auto fname = std::filesystem::path(newest).filename().string();
                                PLOG_INFO("[PULSAR] Fresh quicksave ready + complete: {} ({} bytes, elapsed {}ms)"sv,
                                    fname, newestSize, elapsed);
                                m_quicksavePending = false;
                                SendSaveToClient(newest);
                            } else {
                                // Not fully written yet - keep polling until the 30s timeout.
                                m_quicksaveSizeStableSince = now;   // re-arm stability window
                            }
                        }
                    }
                }
            }
        }

        // Client auto-reconnect
        if (m_reconnectAt > 0 && GetTickCount64() >= m_reconnectAt) {
            m_reconnectAt = 0;
            if (!m_peer && !m_lastAddress.empty()) {
                PLOG_INFO("[PULSAR] Auto-reconnecting to {}:{}..."sv, m_lastAddress, m_lastPort);
                NASLOG("Auto-reconnecting...");

                // Create the fresh host BEFORE destroying the old one: if creation
                // fails we keep the old host, so Update() (which returns early on a
                // null m_host) keeps running and the retry below still fires.
                ENetHost* fresh = enet_host_create(nullptr, 1, NUM_CHANNELS, 0, 0);
                if (fresh) {
                    if (m_host) enet_host_destroy(m_host);
                    m_host = fresh;
                    ENetAddress address{};
                    enet_address_set_host(&address, m_lastAddress.c_str());
                    address.port = m_lastPort;
                    m_peer = enet_host_connect(m_host, &address, NUM_CHANNELS, 0);
                    if (m_peer) {
                        PLOG_INFO("[PULSAR] Reconnect initiated"sv);
                    } else {
                        m_reconnectAt = GetTickCount64() + 5000;  // retry in 5s
                        PLOG_WARN("[PULSAR] Reconnect failed, retry in 5s"sv);
                    }
                } else {
                    m_reconnectAt = GetTickCount64() + 5000;
                    PLOG_WARN("[PULSAR] Reconnect: could not create the network host, retry in 5s"sv);
                }
            }
        }

        // Drain ENet events under lock. OnReceive dispatches to handlers which may
        // run quickly enough to hold the lock - that's fine. Critical: never call
        // any ENet API from another thread without this lock.
        {
            std::lock_guard lock(m_enetMutex);
            ENetEvent event{};
            while (enet_host_service(m_host, &event, 0) > 0) {
                switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    PLOG_DIAG("[PULSAR] co-op: peer connected"sv);   // tester diag (sanitized)
                    OnConnect(event.peer);
                    break;
                case ENET_EVENT_TYPE_RECEIVE:
                    OnReceive(event.peer, event.packet);
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    PLOG_DIAG("[PULSAR] co-op: peer disconnected (reason {})"sv, event.data);
                    OnDisconnect(event.peer, event.data);
                    break;
                default:
                    break;
                }
            }
        }
    }

    void NetworkHost::Send(const uint8_t* a_data, size_t a_length, bool a_reliable)
    {
        std::lock_guard lock(m_enetMutex);
        EncSend(0, a_data, a_length, a_reliable);   // encrypt-or-queue then send
    }

    // encrypt the payload (AES-256-GCM) then send. Before the key handshake
    // completes, queue the packet and flush it once the shared key is derived. When
    // e2e is off (debug), sends in the clear. Relay/network only ever see ciphertext.
    void NetworkHost::EncSend(int a_channel, const uint8_t* a_data, size_t a_len, bool a_reliable)
    {
        if (!m_peer) return;
        const enet_uint32 flags = a_reliable ? ENET_PACKET_FLAG_RELIABLE : enet_uint32{0};

        // Read the e2e/ready flags and, if the key isn't ready yet, queue the plaintext
        // copy under m_cryptoQueueMtx (this runs from two threads; see header). Snapshot
        // the flags then release the lock BEFORE any enet_*/m_crypto.* call.
        bool e2e, ready;
        {
            std::lock_guard<std::mutex> qk(m_cryptoQueueMtx);
            e2e   = m_e2e;
            ready = m_cryptoReady;
            if (e2e && !ready) {
                if (m_preCryptoTx.size() < 1024)
                    m_preCryptoTx.push_back({ a_channel, std::vector<uint8_t>(a_data, a_data + a_len), a_reliable });
                return;  // flushed once the handshake completes
            }
        }

        if (!e2e) {
            ENetPacket* p = enet_packet_create(a_data, a_len, flags);
            enet_peer_send(m_peer, a_channel, p);
            return;
        }
        std::vector<uint8_t> enc;
        if (!m_crypto.Encrypt(a_data, a_len, enc)) return;
        ENetPacket* p = enet_packet_create(enc.data(), enc.size(), flags);
        enet_peer_send(m_peer, a_channel, p);
    }

    // voice: prepend PKT_VOICE and send on channel 2 unreliable via EncSend
    // (auto-encrypts). Locks m_enetMutex like Send(). No-op if not connected.
    void NetworkHost::SendVoiceFrame(const uint8_t* a_payload, size_t a_len)
    {
        if (!m_peer || a_len == 0 || a_len > 1024) return;
        std::vector<uint8_t> buf;
        buf.reserve(1 + a_len);
        buf.push_back(static_cast<uint8_t>(PKT_VOICE));
        buf.insert(buf.end(), a_payload, a_payload + a_len);
        std::lock_guard lock(m_enetMutex);
        EncSend(2, buf.data(), buf.size(), false);
    }

    // Bypasses crypto: used only for the key-exchange hello (public key, safe in clear).
    void NetworkHost::RawSendUnenc(int a_channel, const uint8_t* a_data, size_t a_len, bool a_reliable)
    {
        if (!m_peer) return;
        const enet_uint32 flags = a_reliable ? ENET_PACKET_FLAG_RELIABLE : enet_uint32{0};
        ENetPacket* p = enet_packet_create(a_data, a_len, flags);
        enet_peer_send(m_peer, a_channel, p);
    }

    void NetworkHost::SetAuthSecret(const std::string& a_secret)
    {
        // Store the secret string's bytes directly: both peers share the identical string
        // from the trusted DB, so its encoding is irrelevant - only that both mix the same
        // bytes into the KDF. Applied to m_crypto at connect (OnConnect), gated on config.
        m_authSecret.assign(a_secret.begin(), a_secret.end());
    }

    void NetworkHost::SendCryptoHello()
    {
        auto pub = m_crypto.PublicBlob();
        if (pub.empty()) return;
        std::vector<uint8_t> buf(1 + pub.size());
        buf[0] = PKT_CRYPTO_HELLO;
        std::memcpy(buf.data() + 1, pub.data(), pub.size());
        RawSendUnenc(0, buf.data(), buf.size(), true);
    }

    void NetworkHost::FlushPreCryptoTx()
    {
        // Take ownership of the queue under the lock, then send outside it so we never
        // hold m_cryptoQueueMtx across an enet_*/m_crypto.* call.
        std::vector<PendingTx> pending;
        {
            std::lock_guard<std::mutex> qk(m_cryptoQueueMtx);
            if (!m_cryptoReady || !m_peer) { m_preCryptoTx.clear(); return; }
            pending.swap(m_preCryptoTx);
        }
        for (auto& tx : pending) {
            std::vector<uint8_t> enc;
            if (!m_crypto.Encrypt(tx.data.data(), tx.data.size(), enc)) continue;
            const enet_uint32 flags = tx.reliable ? ENET_PACKET_FLAG_RELIABLE : enet_uint32{0};
            ENetPacket* p = enet_packet_create(enc.data(), enc.size(), flags);
            enet_peer_send(m_peer, tx.channel, p);
        }
    }

    void NetworkHost::SendSaveToClient(const std::string& a_path)
    {
        if (!m_peer || !m_isHost) return;

        std::string savePath = a_path.empty() ? SaveScanner::GetMostRecentSavePath() : a_path;
        if (savePath.empty()) {
            PLOG_WARN("[PULSAR] SendSave: no save file found"sv);
            return;
        }

        // Read the entire .sfs file
        std::ifstream file(savePath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            PLOG_WARN("[PULSAR] SendSave: can't open {}"sv, savePath);
            return;
        }

        size_t fileSize = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        // L.7 Path B Phase 3 - patch out the host's pilot binding
        // before transmission so the engine never restores it on the
        // client side. Failure is non-fatal: fall back to unpatched
        // bytes if any step fails.
        std::vector<uint8_t> rawBytes(fileSize);
        file.read(reinterpret_cast<char*>(rawBytes.data()),
                  static_cast<std::streamsize>(fileSize));
        file.close();

        std::vector<uint8_t> sendBytes;
        bool patchedOk = false;
        do {
            std::vector<uint8_t> decomp;
            SaveScanner::BcpsMeta meta{};
            if (!SaveScanner::DecompressBcpsBuffer(rawBytes, decomp, meta)) {
                PLOG_WARN(
                    "[PULSAR] [L.7 PathB] decompress failed - sending unpatched"sv);
                break;
            }
            const bool didPatch = SaveScanner::PatchPilotState(decomp);
            if (!didPatch) {
                PLOG_INFO(
                    "[PULSAR] [L.7 PathB] no pilot marker - sending unpatched"sv);
                break;
            }
            std::vector<uint8_t> patched;
            if (!SaveScanner::RecompressToBcps(decomp, meta, patched)) {
                PLOG_WARN(
                    "[PULSAR] [L.7 PathB] recompress failed - sending unpatched"sv);
                break;
            }
            sendBytes = std::move(patched);
            patchedOk = true;
            PLOG_INFO(
                "[PULSAR] [L.7 PathB] patched + recompressed: {} → {} bytes"sv,
                fileSize, sendBytes.size());
        } while (false);
        if (!patchedOk) {
            sendBytes = std::move(rawBytes);
        }

        // Build packet: [uint8 type][uint32 totalSize][raw file data]
        const size_t outFileSize = sendBytes.size();
        size_t packetSize = 1 + 4 + outFileSize;
        std::vector<uint8_t> buf(packetSize);
        buf[0] = PKT_SAVE_TRANSFER;
        uint32_t totalSize = static_cast<uint32_t>(outFileSize);
        std::memcpy(&buf[1], &totalSize, 4);
        std::memcpy(&buf[5], sendBytes.data(), outFileSize);

        // Send as one big reliable packet - ENet fragments automatically. Encrypted :
        // the save (= character data) is the most sensitive payload, so it goes through
        // the same E2E path; the relay only ever forwards ciphertext fragments.
        EncSend(1, buf.data(), buf.size(), true);  // channel 1 for large data

        auto filename = std::filesystem::path(savePath).filename().string();
        PLOG_INFO(
            "[PULSAR] SendSave: sent {} ({} bytes, {}) to client"sv,
            filename, outFileSize,
            patchedOk ? "PILOT-PATCHED" : "raw");
        NASLOG("SendSave: %s (%zu bytes, %s)", filename.c_str(), outFileSize,
               patchedOk ? "PATCHED" : "raw");
    }

    bool NetworkHost::ConsumePendingTeleport(float& a_x, float& a_y, float& a_z, float& a_rotZ)
    {
        if (!m_hasPendingTeleport) return false;
        a_x = m_pendingTeleport.x;
        a_y = m_pendingTeleport.y;
        a_z = m_pendingTeleport.z;
        a_rotZ = m_pendingTeleport.rotZ;
        m_hasPendingTeleport = false;
        return true;
    }

    void NetworkHost::BeginHostQuicksaveTransfer()
    {
        if (!m_peer || !m_isHost) return;

        // Snapshot the newest save BEFORE triggering quicksave so we can detect a fresh one
        std::string preSave = SaveScanner::GetMostRecentSavePath();
        std::filesystem::file_time_type preTime = std::filesystem::file_time_type::min();
        if (!preSave.empty()) {
            std::error_code ec;
            auto t = std::filesystem::last_write_time(preSave, ec);
            if (!ec) preTime = t;
        }

        if (!WorldSync::TriggerHostQuicksave()) {
            // do NOT fall back to the stale newest save (mislocates the client,
            // see the poll-timeout note above). Abort; the client re-requests / F8.
            PLOG_WARN("[PULSAR] BeginHostQuicksaveTransfer: quicksave trigger failed - "
                      "aborting bridge (no stale-save fallback, would mislocate client)"sv);
            NASLOG("BeginHostQuicksaveTransfer: trigger failed - bridge aborted");
            return;
        }

        m_quicksavePending = true;
        m_quicksaveStartTick = GetTickCount64();
        m_quicksaveLastPoll = 0;
        m_quicksavePreTime = preTime;
        m_quicksaveCandidate.clear();
        m_quicksaveCandidateSize = 0;
        m_quicksaveSizeStableSince = 0;

        PLOG_INFO("[PULSAR] Quicksave transfer: triggered, polling for new save..."sv);
        NASLOG("Quicksave transfer started");
    }

    void NetworkHost::BeginHostBridgeSaveTransfer(const char* /*a_saveName*/)
    {
        if (!m_peer || !m_isHost) return;

        // L.7 V8c - DO NOT trigger our own save. The engine auto-quicksaves
        // after every Star Map jump, and triggering a second save while the
        // engine's SaveLoadManager is still mid-flight crashes the host.
        // Instead, just poll for the newest file (= engine's post-jump
        // auto-save) and send that. The PFT detector already gives us 5s
        // settle + 3s send delay before this fires, so the auto-save should
        // be on disk by the time the polling starts.
        std::string preSave = SaveScanner::GetMostRecentSavePath();
        std::filesystem::file_time_type preTime = std::filesystem::file_time_type::min();
        if (!preSave.empty()) {
            std::error_code ec;
            auto t = std::filesystem::last_write_time(preSave, ec);
            if (!ec) preTime = t;
        }

        m_quicksavePending = true;
        m_quicksaveStartTick = GetTickCount64();
        m_quicksaveLastPoll = 0;
        // Use a slightly older preTime so the newest save right NOW (the
        // engine's just-completed auto-quicksave) is treated as "fresh".
        m_quicksavePreTime = preTime - std::chrono::seconds(5);
        m_quicksaveCandidate.clear();
        m_quicksaveCandidateSize = 0;
        m_quicksaveSizeStableSince = 0;

        PLOG_INFO(
            "[PULSAR] [L.7 V8c] Bridge-save transfer: NO trigger - using "
            "engine's post-jump auto-save (newest .sfs in saves dir)"sv);
        NASLOG("L.7 V8c Bridge-save transfer (engine auto-save)");
    }

    void NetworkHost::SetPeerTimeout(uint32_t a_ms)
    {
        if (m_peer) {
            enet_peer_timeout(m_peer, 0, a_ms, a_ms * 2);
        }
    }

    void NetworkHost::ResetTeleport()
    {
        // Phase 3i v17.1: bridge saves (forward + return) place client at host's
        // exact position via the save itself. Auto-teleport on next PKT_POSITION
        // would push them back into NULL cell state ("game menu" fallback).
        if (m_suppressNextResetTeleport.exchange(false)) {
            PLOG_INFO("[PULSAR] NetworkHost: ResetTeleport SUPPRESSED (bridge save)"sv);
            NASLOG("TELE-DIAG ResetTeleport SUPPRESSED");
            return;
        }
        bool wasTeleported = m_clientTeleported;
        m_clientTeleported = false;
        m_hasPendingTeleport = false;
        PLOG_INFO(
            "[PULSAR] [TELE-DIAG] ResetTeleport applied - m_clientTeleported {} → false "
            "(next PKT_POSITION will trigger 'Will teleport player to host area')"sv,
            wasTeleported);
        NASLOG("TELE-DIAG ResetTeleport applied (wasTeleported=%d)",
               wasTeleported ? 1 : 0);
    }

    void NetworkHost::SuppressNextResetTeleport()
    {
        m_suppressNextResetTeleport.store(true);
        PLOG_INFO(
            "[PULSAR] [TELE-DIAG] SuppressNextResetTeleport armed - next "
            "ResetTeleport call will be skipped"sv);
        NASLOG("TELE-DIAG SuppressNextResetTeleport armed");
    }

    void NetworkHost::Disconnect()
    {
        if (m_peer) {
            enet_peer_disconnect(m_peer, 0);
            if (m_host) enet_host_flush(m_host);
            m_peer = nullptr;
        }
        if (m_host) {
            enet_host_destroy(m_host);
            m_host = nullptr;
        }
        m_isHost = false;
        m_reconnectAt = 0;
        PLOG_INFO("[PULSAR] Disconnected"sv);
    }

    void NetworkHost::Shutdown()
    {
        Disconnect();

        if (m_initialized) {
            enet_deinitialize();
            m_initialized = false;
        }

        PLOG_INFO("[PULSAR] Network shut down"sv);
    }

    void NetworkHost::OnConnect(ENetPeer* a_peer)
    {
        m_peer = a_peer;

        // Configure peer timeout: detect dead connections.
        // params: limit, min_timeout_ms, max_timeout_ms
        // 2026-06-11: RELAXED from (5, 2000, 5000) - that dropped the peer
        // after a 2-5s hiccup (WLAN burst loss / big reliable transfer), and
        // every spurious drop despawned the clone + 3s auto-reconnect +
        // respawn = the reported "clone disappears and reappears" flicker.
        // 8-16s with 16 retries still detects a truly dead link, but transient
        // stalls now survive without a disconnect.
        enet_peer_timeout(a_peer, 16, 8000, 16000);

        // Ping interval for keepalive (default 500ms, reduce for faster detection)
        enet_peer_ping_interval(a_peer, 500);

        // L.7 V9.4 - peer is now ENET_PEER_STATE_CONNECTED; safe for sends.
        m_connectionEstablished.store(true, std::memory_order_release);

        // tell the Companion a voice session is up (start capture).
        Pulsar::Voice::VoiceBridge::OnPeerUp();

        // kick off the E2E key exchange right away. Both peers send their ephemeral
        // public key (unencrypted, safe); on receiving the peer's hello each derives the
        // same shared AES key (see OnReceive). App packets are queued until then.
        const bool wantE2e = Pulsar::GetConfig().e2eEncryption;
        const bool cryptoInitOk = wantE2e && m_crypto.Init();  // Init() before taking the queue lock
        {
            std::lock_guard<std::mutex> qk(m_cryptoQueueMtx);
            m_e2e = wantE2e;
            m_cryptoReady = false;
            m_preCryptoTx.clear();
        }
        if (wantE2e) {
            if (cryptoInitOk) {
                // Anti-MITM: mix the DB-delivered per-lobby secret into the key derivation
                // so a malicious relay cannot MITM the ECDH exchange. Gated on config and
                // on a secret actually being present; otherwise unauthenticated (legacy)
                // ECDH. Set per-connect so a stale secret never carries over.
                if (Pulsar::GetConfig().e2eRelayAuth && !m_authSecret.empty()) {
                    m_crypto.SetAuthSecret(m_authSecret.data(), m_authSecret.size());
                    PLOG_DIAG("[PULSAR] co-op: relay-auth secret applied (anti-MITM)"sv);
                } else {
                    m_crypto.SetAuthSecret(nullptr, 0);
                }
                SendCryptoHello();
                PLOG_DIAG("[PULSAR] co-op: secure channel handshake sent"sv);
            } else {
                // Fail closed: never silently downgrade to plaintext. CNG is proven at
                // startup by the self-test, so this path is effectively unreachable; if it
                // ever does fire, drop the peer rather than leak gameplay traffic in clear.
                PLOG_DIAG("[PULSAR] crypto init failed; refusing plaintext, disconnecting"sv);
                if (m_peer) enet_peer_disconnect(m_peer, 0);
                return;
            }
        }

        // P2P: if the connected peer is NOT the relay endpoint, it's a direct
        // link → stop the host's REGISTER keepalive so the relay session idles
        // out (zero game traffic over the VPS). If it IS the relay, we fell back.
        if (m_relayMode && !m_forceRelay && m_isHost) {
            const bool viaRelay = (a_peer->address.host == m_relayAddress.host &&
                                   a_peer->address.port == m_relayAddress.port);
            if (!viaRelay) {
                m_relayKeepaliveAt = UINT64_MAX;
                PLOG_DIAG("[PULSAR] [Relay] host: DIRECT peer - keepalive stopped"sv);
            } else {
                PLOG_DIAG("[PULSAR] [Relay] host: peer via relay (fallback)"sv);
            }
        }

        PLOG_DIAG("[PULSAR] Client connected! Timeout=8-16s, Ping=500ms"sv);
        NASLOG("Client connected, timeout configured");
    }

    void NetworkHost::OnReceive(ENetPeer* a_peer, ENetPacket* a_packet)
    {
        // E2E: handle the key-exchange hello, then decrypt every other packet BEFORE
        // any code below reads data[0]. After decrypt we repoint a_packet at the plaintext
        // (the original packet is destroyed by the caller, not here).
        // Only while the key is not agreed yet: encrypted packets start with a random
        // nonce, and ~1 in 256 of them begin with the hello byte (KeyExchange.h).
        bool sessionReady;
        { std::lock_guard<std::mutex> qk(m_cryptoQueueMtx); sessionReady = m_cryptoReady; }
        if (a_packet && IsKeyExchangeHello(m_e2e && sessionReady, a_packet->data, a_packet->dataLength)) {
            if (m_e2e && a_packet->dataLength > 1 &&
                m_crypto.DeriveFromPeer(a_packet->data + 1, a_packet->dataLength - 1)) {
                { std::lock_guard<std::mutex> qk(m_cryptoQueueMtx); m_cryptoReady = true; }
                FlushPreCryptoTx();
                PLOG_DIAG("[PULSAR] co-op: secure channel established"sv);
            }
            return;
        }
        std::vector<uint8_t> plain;
        ENetPacket localPkt{};
        if (m_e2e) {
            if (!m_cryptoReady) return;                  // key not agreed yet -> drop
            if (!a_packet || !m_crypto.Decrypt(a_packet->data, a_packet->dataLength, plain))
                return;                                  // forged / corrupt -> drop
            localPkt.data       = plain.data();
            localPkt.dataLength = static_cast<enet_uint32>(plain.size());
            a_packet = &localPkt;
        }

        // PKT_VOICE - proximity voice frame (own framing, unreliable ch2). High
        // frequency; handled + returned before the diagnostic logging and the
        // save-load barrier so it never floods logs or gets barrier-dropped.
        if (a_packet && a_packet->dataLength >= 1 + sizeof(VoiceFrameHeader)
            && a_packet->data[0] == PKT_VOICE) {
            Pulsar::Voice::VoiceBridge::OnVoicePacket(a_packet->data + 1, a_packet->dataLength - 1);
            return;
        }

        // V14.3 - log every received pkt type for hatch-crash diagnosis.
        // NASLOG flushes immediately so we still see the last pkt before
        // a freeze. Throttled to once per type per ~500ms to avoid flood.
        if (a_packet && a_packet->dataLength >= 1) {
            static std::atomic<std::uint64_t> s_lastLogPerType[256] = {};
            const uint8_t pktTypeForLog = a_packet->data[0];
            const std::uint64_t now = ::GetTickCount64();
            const std::uint64_t lastT =
                s_lastLogPerType[pktTypeForLog].load(std::memory_order_relaxed);
            if (now - lastT > 500) {
                s_lastLogPerType[pktTypeForLog].store(
                    now, std::memory_order_relaxed);
                NASLOG("NET-RECV pkt=0x%02X len=%zu role=%s",
                       pktTypeForLog,
                       static_cast<size_t>(a_packet->dataLength),
                       m_isHost ? "host" : "client");
            }
        }

        // Special handling for PKT_SAVE_TRANSFER (uses own format, not PacketHeader)
        // Only the CLIENT receives save transfers; the host is the sender
        if (!m_isHost && a_packet->dataLength >= 5 && a_packet->data[0] == PKT_SAVE_TRANSFER) {
            WorldSync::OnSaveReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // L.7 V13g / V15.1 - save-load barrier. Drop most packets while:
        //   (a) WorldSync is in SAVE_RECEIVED/PENDING_TRIGGER/LOADING
        //       (client is loading host-bridge save), OR
        //   (b) loadMenu is open OR was just observed open (client did
        //       its OWN fast-travel/POI/door-load - V13g didn't catch
        //       this because WorldSync stays LOADED for client-initiated
        //       loads). Verified 2026-05-04: client crashed after own
        //       fast-travel because 279 buffered NPC packets all fired
        //       ApplyLocomotion in one tick when loadMenu CLOSED.
        // Whitelist: control/handshake/load-complete signals only.
        if (!m_isHost && a_packet->dataLength >= 1) {
            const uint8_t pktType = a_packet->data[0];
            const auto wss = WorldSync::GetState();
            const bool worldSyncBarrier =
                (wss == WorldSync::State::SAVE_RECEIVED ||
                 wss == WorldSync::State::PENDING_TRIGGER ||
                 wss == WorldSync::State::PENDING_QUIT_FADE ||
                 wss == WorldSync::State::LOADING);
            // Cell3DAttach::IsQuiet() naming is inverted - returns TRUE when
            // engine is in chaos (loadMenu open OR within settle window after
            // close). That's exactly when we want to barrier. V15.1 bug: I
            // had `!IsQuiet()` which blocked all NORMAL play traffic and
            // broke host→client position sync. Verified user 2026-05-04.
            const bool loadMenuBarrier =
                Pulsar::Cell3DAttach::IsQuiet();
            const bool barrier = worldSyncBarrier || loadMenuBarrier;
            if (barrier) {
                const bool whitelisted =
                    (pktType == PKT_SAVE_TRANSFER) ||
                    (pktType == PKT_HANDSHAKE) ||
                    (pktType == PKT_CLIENT_LOAD_COMPLETE) ||
                    (pktType == PKT_BRIDGE_REQUEST) ||
                    (pktType == PKT_HOST_REQUEST_RECONNECT);
                if (!whitelisted) {
                    static std::atomic<uint64_t> s_dropCount{ 0 };
                    auto n = ++s_dropCount;
                    if ((n & 0x3F) == 0) {  // log every 64th drop
                        PLOG_INFO(
                            "[PULSAR] [L.7 V13g] save-load barrier dropped "
                            "pkt=0x{:02X} (#{} since plugin load)"sv,
                            pktType, n);
                    }
                    return;
                }
            }
        }

        // PKT_NPC_STATE - batch of NPC positions/health/dead-state (host → client).
        // Also uses own framing (not PacketHeader). Only clients receive.
        if (!m_isHost && a_packet->dataLength >= 1 + sizeof(NPCStateBatchHeader)
            && a_packet->data[0] == PKT_NPC_STATE) {
            Pulsar::NPCSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_NPC_DAMAGE - client forwarded combat damage for a host-matched
        // NPC (own framing, like PKT_NPC_STATE). Host-only: only the host owns
        // the authoritative enemy. Queues; applied on the game thread.
        if (m_isHost && a_packet->dataLength >= 1 + sizeof(NpcDamageData)
            && a_packet->data[0] == PKT_NPC_DAMAGE) {
            Pulsar::NPCSync::OnDamageReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_PLAYER_DOWNED - peer's player entered/left the downed state.
        // Bidirectional (each player owns its own mortality), so NOT host-gated.
        if (a_packet->dataLength >= 1 + sizeof(PlayerDownedData)
            && a_packet->data[0] == PKT_PLAYER_DOWNED) {
            Pulsar::Coop::Mortality::OnDownedReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_PLAYER_REVIVE - partner asks the downed player (us) to get up
        // (medpack revive). Bidirectional, NOT host-gated. OnReviveReceived
        // queues only; the revive runs on the game thread.
        if (a_packet->dataLength >= 1 + sizeof(PlayerReviveData)
            && a_packet->data[0] == PKT_PLAYER_REVIVE) {
            Pulsar::Coop::Mortality::OnReviveReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_PLAYER_COMBAT - peer's player combat state changed. A downed partner
        // gates its combat-end auto-revive on this (its own enemies disengage the
        // instant it goes down). Bidirectional, NOT host-gated.
        if (a_packet->dataLength >= 1 + sizeof(PlayerCombatData)
            && a_packet->data[0] == PKT_PLAYER_COMBAT) {
            Pulsar::Coop::Mortality::OnCombatStatusReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_KILL_XP - peer earned kill-XP; mirror it onto our own player so
        // both players gain XP for a shared-area kill. Bidirectional (each side
        // owns its own kills), NOT host-gated. Queues; awarded on game thread.
        if (a_packet->dataLength >= 1 + sizeof(KillXpData)
            && a_packet->data[0] == PKT_KILL_XP) {
            Pulsar::Coop::KillXp::OnKillXpReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_EQUIP_V2 - peer's worn equipment. re-enabled 2026-06-19 for
        // ARMOR only (weapon equip-apply stays gated off in RemotePlayerManager via
        // EquipSync::kSyncWeapons - it's what destabilised the clone anim graph).
        if (a_packet->dataLength >= 1 + sizeof(EquipV2Header)
            && a_packet->data[0] == PKT_EQUIP_V2) {
            Pulsar::RemotePlayer::EquipSync::OnEquipV2Received(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_CIVILIAN_SNAPSHOT - slim periodic civilian position resync (host → client).
        if (!m_isHost && a_packet->dataLength >= 1 + sizeof(CivilianSnapshotHeader)
            && a_packet->data[0] == PKT_CIVILIAN_SNAPSHOT) {
            Pulsar::NPCSync::OnCivilianSnapshotReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_CORPSE_LOOT - host-authoritative corpse inventory (host → client).
        if (!m_isHost && a_packet->dataLength >= 1 + sizeof(CorpseLootHeader)
            && a_packet->data[0] == PKT_CORPSE_LOOT) {
            Pulsar::NPCSync::OnCorpseLootReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_QUEST_STAGE - peer's TESQuest just advanced a stage; mirror it
        // by calling native worker FUN_140D0E1F0 with echo-suppression flag.
        // Bidirectional: either side can be the originator. L.8 Phase 1.
        if (a_packet->dataLength >= 1 + sizeof(QuestStageData)
            && (a_packet->data[0] == PKT_QUEST_STAGE
                || a_packet->data[0] == PKT_QUEST_STAGE_REPLAY)) {
            Pulsar::QuestSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_QUEST_START / PKT_QUEST_STOP - Phase 2 placeholders, dispatched
        // so the dedicated handler can drain them with a "not yet handled"
        // log and we don't dump them on the default-case fallback.
        if (a_packet->dataLength >= 1 + sizeof(QuestStartStopData)
            && (a_packet->data[0] == PKT_QUEST_START
                || a_packet->data[0] == PKT_QUEST_STOP)) {
            Pulsar::QuestSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_DIALOG_START - peer entered dialogue with NPC. L.8 Phase 3 V1.
        if (a_packet->dataLength >= 1 + sizeof(DialogStartData)
            && a_packet->data[0] == PKT_DIALOG_START) {
            Pulsar::DialogSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }
        // PKT_DIALOG_END - peer dismissed dialogue.
        if (a_packet->dataLength >= 1 + sizeof(DialogEndData)
            && a_packet->data[0] == PKT_DIALOG_END) {
            Pulsar::DialogSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }
        // PKT_DIALOG_VOTE - peer picked a TopicInfo. L.8 Phase 3 V4.
        if (a_packet->dataLength >= 1 + sizeof(DialogVoteData)
            && a_packet->data[0] == PKT_DIALOG_VOTE) {
            Pulsar::DialogSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_VEHICLE_POS - single FURN-REFR position from host (Moon Jumper etc.).
        // Own framing like PKT_NPC_STATE. Client-only.
        if (!m_isHost && a_packet->dataLength >= 1 + sizeof(VehiclePosData)
            && a_packet->data[0] == PKT_VEHICLE_POS) {
            Pulsar::VehicleSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_LOOT_PICKUP - a pickup happened on the other side; replay
        // locally so the REFR despawns here too. Bidirectional (either side
        // can be the sender) - the hook's replay-guard prevents a rebroadcast
        // loop.
        if (a_packet->dataLength >= 1 + sizeof(LootPickupData)
            && a_packet->data[0] == PKT_LOOT_PICKUP) {
            Pulsar::WorldSync::Loot::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_CONTAINER_TAKE - the peer just took an item from a container;
        // apply the same delta to our mirror container so both sides stay
        // converged. Bidirectional.
        if (a_packet->dataLength >= 1 + sizeof(ContainerTakeData)
            && a_packet->data[0] == PKT_CONTAINER_TAKE) {
            Pulsar::WorldSync::Loot::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_LOOT_DROP - the peer dropped an item from inventory; spawn a
        // matching REFR at our clone's position via drop-core replay.
        // Bidirectional.
        if (a_packet->dataLength >= 1 + sizeof(LootDropData)
            && a_packet->data[0] == PKT_LOOT_DROP) {
            Pulsar::WorldSync::Loot::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_KEY_MIRROR - the peer looted a key/keycard; add it to our player
        // too so neither gets stuck at a loading-screen door. Bidirectional.
        if (a_packet->dataLength >= 1 + sizeof(KeyMirrorData)
            && a_packet->data[0] == PKT_KEY_MIRROR) {
            Pulsar::WorldSync::Loot::OnKeyMirrorReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_DOOR_STATE - the peer activated a door; replay Activate
        // locally so the door animates open/closed in sync. Bidirectional.
        if (a_packet->dataLength >= 1 + sizeof(DoorStateData)
            && a_packet->data[0] == PKT_DOOR_STATE) {
            Pulsar::WorldSync::Door::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_NPC_SPAWN - host says a persistent quest NPC became active in the
        // shared cell; enable + place the same ref locally .
        if (a_packet->dataLength >= 1 + sizeof(NpcSpawnData)
            && a_packet->data[0] == PKT_NPC_SPAWN) {
            Pulsar::SpawnSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_LOCK_STATE - the peer's REFR_LOCK->flags transitioned (typ.
        // lockpick success); mirror the flag on our local REFR.
        // Bidirectional.
        if (a_packet->dataLength >= 1 + sizeof(LockStateData)
            && a_packet->data[0] == PKT_LOCK_STATE) {
            Pulsar::WorldSync::Lock::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_CONTAINER_OPEN - the peer activated an unlocked kCONT;
        // mirror the open-state via SetOpen on our local REFR. Bidirectional.
        if (a_packet->dataLength >= 1 + sizeof(ContainerOpenData)
            && a_packet->data[0] == PKT_CONTAINER_OPEN) {
            Pulsar::WorldSync::Lock::OnContainerOpenReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_GRAB_STATE - the peer is E-Hold-grabbing a REFR; mirror its
        // position via SetPosition vt[0x89] until release. Bidirectional.
        if (a_packet->dataLength >= 1 + sizeof(GrabStateData)
            && a_packet->data[0] == PKT_GRAB_STATE) {
            Pulsar::WorldSync::Grab::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_ACTI_STATE - the peer activated a kACTI REFR (switch, button,
        // power-conduit); replay Activate locally so the engine triggers
        // the activator's script + animation. Bidirectional.
        if (a_packet->dataLength >= 1 + sizeof(ActiStateData)
            && a_packet->data[0] == PKT_ACTI_STATE) {
            Pulsar::WorldSync::Acti::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_FORM_BLOB - host streamed a serialized form (typically the
        // player REFR after cell-transition). Client deserializes onto the
        // matching local target (clone) so the engine treats it like a
        // save-loaded actor (re-attaches biped, applies extra-data, etc.).
        if (a_packet->dataLength >= 1 + sizeof(FormBlobHeader)
            && a_packet->data[0] == PKT_FORM_BLOB) {
            Pulsar::Forms::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_TRANSIT - Phase K.1. Peer transitioned cells. K.1 logs only.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(TransitData)
            && a_packet->data[0] == PKT_TRANSIT) {
            Pulsar::TransitSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_SPACE_TRANSITION - Phase K.7. Peer ship lifecycle event.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(SpaceTransitionData)
            && a_packet->data[0] == PKT_SPACE_TRANSITION) {
            Pulsar::SpaceSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // L.6 V1 (PKT_DOCK_STATE 0x88) + V2 (PKT_HATCH_TRANSIT 0x89) removed
        // 2026-05-02 - both were MoveTo-shortcut approaches. V3 plan in
        // memory/project_l6_dock_sync.md (engine-state memory-diff).

        // PKT_SHIP_POS - Phase L.3 V1. Pilot's ship pos/rot update.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(ShipPosData)
            && a_packet->data[0] == PKT_SHIP_POS) {
            Pulsar::SpaceSync::OnShipPosReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_FLIGHT_STATE - Phase L.3 V2 V1. Pilot StickDataProvider state.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(FlightStateData)
            && a_packet->data[0] == PKT_FLIGHT_STATE) {
            Pulsar::FlightSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_FLIGHT_INPUT - Phase L.3 V2 V2. Pilot raw input axes.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(FlightInputData)
            && a_packet->data[0] == PKT_FLIGHT_INPUT) {
            Pulsar::FlightSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_PMO_PAYLOAD - Phase L.3 V2.2. Full PMO event payload bytes.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(PMOPayloadData)
            && a_packet->data[0] == PKT_PMO_PAYLOAD) {
            Pulsar::FlightSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_SHIP_TRANSFORM - Phase L.3 V3. NiNode world transform.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(ShipTransformData)
            && a_packet->data[0] == PKT_SHIP_TRANSFORM) {
            Pulsar::FlightSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_SHIP_FIRE - Phase L.4 V1. Ship weapon fire event.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(ShipFireData)
            && a_packet->data[0] == PKT_SHIP_FIRE) {
            Pulsar::FlightSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_NPC_SHIP_BATCH - Phase M.1 V2 iter2. NPC ship transforms.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(NPCShipBatch)
            && a_packet->data[0] == PKT_NPC_SHIP_BATCH) {
            Pulsar::FlightSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_SHIP_AV - ship-combat sync. Shared-ship hull (slice 2).
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(ShipAvData)
            && a_packet->data[0] == PKT_SHIP_AV) {
            Pulsar::ShipCombatSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_TIME_WEATHER_REQ - client asks host for time+weather (header-only).
        if (a_packet->dataLength >= 1 && a_packet->data[0] == PKT_TIME_WEATHER_REQ) {
            Pulsar::TimeWeatherSync::OnRequestReceived();
            return;
        }

        // PKT_TIME_WEATHER - host's one-shot time+weather snapshot (client applies).
        if (a_packet->dataLength >= 1 + sizeof(TimeWeatherData)
            && a_packet->data[0] == PKT_TIME_WEATHER) {
            Pulsar::TimeWeatherSync::OnSnapshotReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_CRUISE_STATE - Phase L.5. Cruise mode toggle.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(CruiseStateData)
            && a_packet->data[0] == PKT_CRUISE_STATE) {
            Pulsar::FlightSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_CRUISE_ORIENT - Phase L.5 Phase 4 Step 11. Continuous orient sync.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(CruiseOrientData)
            && a_packet->data[0] == PKT_CRUISE_ORIENT) {
            Pulsar::FlightSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_DOCK_FINISHED - Phase L.6 V3 Phase 2. Host → client dock-state.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(DockFinishedData)
            && a_packet->data[0] == PKT_DOCK_FINISHED) {
            Pulsar::DockSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_DOCK_EXTRATELEPORT - Phase L.6 V3 Phase 3b. ExtraTeleport bytes.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(DockExtraTeleportData)
            && a_packet->data[0] == PKT_DOCK_EXTRATELEPORT) {
            Pulsar::DockSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_DOCK_UNDOCKED - Phase L.6 V3 Phase 3e. Symmetric undock cleanup.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(DockUndockedData)
            && a_packet->data[0] == PKT_DOCK_UNDOCKED) {
            Pulsar::DockSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_DOCK_STATION_XT - Phase L.6 V3 Phase 3i. Pilot's station-XT bytes.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(DockStationXTData)
            && a_packet->data[0] == PKT_DOCK_STATION_XT) {
            Pulsar::DockSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_DOCK_FORWARD_BRIDGE_EXPECTED - Phase L.6 V3 Phase 3i v17.
        // Host signals "save-bridge incoming, suppress your cockpit-hatch
        // activate". Payload: just a uint64 timestamp (8 bytes after header).
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(uint64_t)
            && a_packet->data[0] == PKT_DOCK_FORWARD_BRIDGE_EXPECTED) {
            Pulsar::DockSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_FAST_TRAVEL_SYNC - Phase L.7 V7. Host tells client which
        // cell+pos+WS its ship arrived in after a Star Map fast-travel.
        if (a_packet->dataLength >= HEADER_SIZE + sizeof(FastTravelSyncData)
            && a_packet->data[0] == PKT_FAST_TRAVEL_SYNC) {
            Pulsar::SpaceSync::FastTravel::OnFastTravelPacket(
                a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_CLIENT_LOAD_COMPLETE - Phase L.7 V8. Client → host: a save
        // load just finished, host can fire the next save-bridge safely.
        if (a_packet->dataLength >= 1 && a_packet->data[0] == PKT_CLIENT_LOAD_COMPLETE) {
            Pulsar::SpaceSync::FastTravel::OnClientLoadCompletePacket(
                a_packet->data, a_packet->dataLength);
            // the client just finished a bridge/F8 load and is now freshly
            // co-located in the host's cell. F8/bridge reloads ONLY the client, so
            // the host's clone-of-client can be left stale/invisible: the normal
            // spawn-on-PKT_POSITION path is guarded by !IsSpawnPending(), so an
            // already-existing (broken) clone is NEVER refreshed → host stops
            // seeing the client. Force a clean respawn here (despawn + fresh spawn
            // at the client's now-co-located position + biped re-attach).
            if (m_isHost) {
                RemotePlayer::GetRemotePlayerManager().RequestForceRespawn();
                PLOG_INFO(
                    "[PULSAR] [] client LOAD_COMPLETE → host force-respawn "
                    "clone-of-client (refresh stale/invisible clone)"sv);
            }
            return;
        }

        // PKT_BRIDGE_REQUEST - L.7. Client → host: the client's AutoFollow
        // had to skip across a proc / cross-worldspace boundary and asks the
        // host to bridge it (auto co-location). Queues for the game thread.
        if (a_packet->dataLength >= 1 && a_packet->data[0] == PKT_BRIDGE_REQUEST) {
            Pulsar::SpaceSync::FastTravel::OnBridgeRequestPacket(
                a_packet->data, a_packet->dataLength);
            return;
        }

        // PKT_HOST_REQUEST_RECONNECT - L.7 V9. Host says "drop and rejoin"
        // so the connect-time save flow (full engine reset) re-syncs the
        // client. Used after the engine's 2nd-mid-game-load limit kicks in.
        // CALLED ON NETWORK-RECEIVE THREAD: must NOT call Disconnect() here
        // (it races with the game thread's clone/cell teardown). Just set
        // a flag; the game-thread tick polls it and calls Disconnect from
        // a safe context.
        if (a_packet->dataLength >= 1 && a_packet->data[0] == PKT_HOST_REQUEST_RECONNECT && !m_isHost) {
            PLOG_INFO(
                "[PULSAR] [L.7 V9] client RECV RECONNECT request - "
                "scheduling disconnect on game thread"sv);
            NASLOG("L.7 V9 client RECV reconnect request (deferred)");
            m_pendingReconnect.store(true, std::memory_order_release);
            return;
        }

        // PKT_CELL_CONTENT_BATCH - Phase K.6 V1. Custom framing
        // ([type][hdr][descriptors]), NOT PacketHeader-based.
        if (a_packet->dataLength >= 1 + sizeof(CellContentBatchHeader)
            && a_packet->data[0] == PKT_CELL_CONTENT_BATCH) {
            Pulsar::CellContentSync::OnPacketReceived(a_packet->data, a_packet->dataLength);
            return;
        }

        PacketReader reader(a_packet->data, a_packet->dataLength);
        if (!reader.IsValid()) {
            PLOG_WARN("[PULSAR] Invalid packet ({} bytes)"sv, a_packet->dataLength);
            return;
        }

        s_pktRecv++;
        switch (reader.Type()) {
        case PKT_HANDSHAKE: {
            HandshakeData hs{};
            if (reader.Read(hs)) {
                hs.playerName[sizeof(hs.playerName) - 1] = '\0';   // untrusted field
                const Compat::Version localV{
                    static_cast<std::uint8_t>(Plugin::Version.major()),
                    static_cast<std::uint8_t>(Plugin::Version.minor()),
                    static_cast<std::uint8_t>(Plugin::Version.patch()) };
                const Compat::Version remoteV{ hs.versionMajor, hs.versionMinor, hs.versionPatch };
                if (m_isHost && !Compat::IsCompatible(localV, remoteV)) {
                    // Packet layouts differ between releases: playing on would desync
                    // or crash. Refuse, and tell the client which version we run.
                    PLOG_DIAG("[PULSAR] co-op: refused '{}' - they run PULSAR {}, this host runs {}. "
                              "Both players need the same PULSAR version."sv,
                              hs.playerName, Compat::DescribeRemote(remoteV), Compat::ToString(localV));
                    if (m_peer) enet_peer_disconnect(m_peer, Compat::EncodeVersionReject(localV));
                    break;
                }
                PLOG_INFO("[PULSAR] Handshake: {} v{}.{}.{} DLC:0x{:02X}"sv,
                    hs.playerName, hs.versionMajor, hs.versionMinor, hs.versionPatch, hs.dlcFlags);
                NASLOG("Handshake: %s v%d.%d.%d", hs.playerName, hs.versionMajor, hs.versionMinor, hs.versionPatch);

                // Host: trigger a FRESH quicksave of the current position and send THAT,
                // so the client co-locates with where the host actually is (not a stale
                // on-disk save). RE 2026-06-23: a quicksave's decompressed inner save is
                // byte-format-IDENTICAL to a full save (SFS_SAVEGAME, same fields), so the
                // old "triggered quicksave is rejected" was NOT a format problem - it was
                // an incomplete file sent mid-write (or the abandoned saveType=2 mutation
                // corrupting it). The quicksave-poll now gates the send on the file fully
                // decompressing, so only a COMPLETE save goes out. If no fresh save appears
                // (host in combat → engine blocks saving, or mid-flight), the poll's 30s
                // timeout falls back to the newest on-disk save.
                //
                // L.7 D1 - debounce: a lossy link re-handshakes after an ENet
                // timeout hiccup. Re-sending the bridge save to a client that is
                // already in-world forces a destructive save-reload mid-session
                // (clone freed mid-load → CTD). Only auto-bridge if we haven't
                // bridged in the last 90s; a genuine rejoin can press F8.
                if (m_isHost) {
                    const uint64_t nowMs = GetTickCount64();
                    if (m_lastAutoBridgeMs != 0 &&
                        nowMs - m_lastAutoBridgeMs < 90000) {
                        PLOG_INFO(
                            "[PULSAR] [L.7 D1] handshake re-bridge SKIPPED "
                            "({}ms since last auto-bridge < 90s) - client likely "
                            "reconnected mid-session; press F8 to force"sv,
                            nowMs - m_lastAutoBridgeMs);
                        NASLOG("D1 re-bridge skipped (reconnect debounce)");
                        // No fresh save for this client: send it the quest stages
                        // it missed while disconnected (0.5.0).
                        Pulsar::QuestSync::ReplayMissedStagesToPeer();
                    } else {
                        m_lastAutoBridgeMs = nowMs;
                        Pulsar::QuestSync::ForgetMissedStages();   // the save carries them
                        BeginHostQuicksaveTransfer();   // fresh save of current pos (was: stale SendSaveToClient)
                    }
                }
            }
            break;
        }
        case PKT_POSITION: {
            PositionData pos{};
            if (reader.Read(pos)) {
                if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) break;

                auto& remoteMgr = RemotePlayer::GetRemotePlayerManager();

                // Client: teleport player to host ONCE on first connect
                if (!m_isHost && !m_clientTeleported) {
                    m_pendingTeleport = { pos.x, pos.y, pos.z, pos.rotX, pos.rotY, pos.rotZ };
                    m_hasPendingTeleport = true;
                    m_clientTeleported = true;
                    PLOG_INFO(
                        "[PULSAR] [TELE-DIAG] Will teleport player to host area "
                        "(target pos=({:.1f},{:.1f},{:.1f}) cell=0x{:08X}) - "
                        "triggered by PKT_POSITION RECV after m_clientTeleported was false"sv,
                        pos.x, pos.y, pos.z, pos.cellFormID);
                    NASLOG("TELE-DIAG Will teleport pos=(%.1f,%.1f,%.1f) cell=0x%08X",
                           pos.x, pos.y, pos.z, pos.cellFormID);
                }

                // First position packet: request spawn of remote player avatar
                // Guard: only spawn when PULSAR.esp is loaded (globs available).
                // During WorldSync (save loading), the client is at the main menu - globs aren't
                // available yet, DoDespawn/DoSpawn are no-ops, causing an infinite storm.
                // WorldSync::ProcessPending triggers ForceRespawn itself at LOADED state.
                if (!remoteMgr.IsSpawnPending() && remoteMgr.IsGlobsResolved()) {
                    remoteMgr.RequestForceRespawn();
                }

                // Send position to remote player manager (writes to GLOBs → Papyrus TranslateTo)
                s_pktRecvPos++;
                // Phase I - propagate sender's parentCell so DoSetPosition can
                // suppress cross-cell applies (proc-cell mismatch after rein).
                remoteMgr.SetRemoteCellID(pos.cellFormID);
                remoteMgr.RequestSetPosition(pos.x, pos.y, pos.z, pos.rotX, pos.rotY, pos.rotZ,
                                             pos.speed, pos.direction, pos.lookHeading, pos.animFlags, pos.vars);
            }
            break;
        }
        case PKT_EQUIP: {
            // Disabled 2026-04-25 (legacy single-weapon equip). F.2 rebuilds.
            EquipData equip{};
            (void)reader.Read(equip);
            break;
        }
        case PKT_EQUIPMENT: {
            // Disabled 2026-04-25 - outdated equipment-sync path, rebuilt in
            // Phase F.2. Receiver is a no-op; packet still drained so the
            // reader advances. Sender also disabled in Main.cpp.
            EquipmentData equip{};
            (void)reader.Read(equip);
            break;
        }
        case PKT_BONES: {
            BoneData bones{};
            if (reader.Read(bones)) {
                auto& remoteMgr = RemotePlayer::GetRemotePlayerManager();
                remoteMgr.RequestSetBones(bones);
            }
            break;
        }
        case PKT_FIRE: {
            FireData fire{};
            if (reader.Read(fire)) {
                auto& remoteMgr = RemotePlayer::GetRemotePlayerManager();
                remoteMgr.RequestFire(fire.actionFormID);
            }
            break;
        }
        case PKT_INTERACT: {
            InteractData interact{};
            if (reader.Read(interact)) {
                PLOG_INFO("[PULSAR] Interact request: 0x{:08X}"sv, interact.targetFormID);
                Companion::GetInteractionManager().RequestInteract(interact.targetFormID);
            }
            break;
        }
        case PKT_DROP_ITEM: {
            DropItemData drop{};
            if (reader.Read(drop)) {
                PLOG_INFO("[PULSAR] Drop from other player: 0x{:08X} x{}"sv, drop.itemFormID, drop.count);
                Companion::GetInteractionManager().RequestSpawnDrop(drop.itemFormID, drop.count);
            }
            break;
        }
        case PKT_INVENTORY: {
            // Client requests inventory sync
            PLOG_INFO("[PULSAR] Inventory sync requested"sv);
            Companion::GetInteractionManager().RequestInventorySync();
            break;
        }
        case PKT_CLIENT_PROFILE: {
            // Equipment/profile-apply disabled 2026-04-25. Rebuilt in F.2 once
            // loot pickup/drop lands and we have a real inventory-ownership
            // model. Packet still consumed so reader state is correct.
            ClientProfileHeader header{};
            if (!reader.Read(header)) break;
            PLOG_INFO("[PULSAR] Client profile rx (ignored): '{}' Lv{} items={}"sv,
                header.playerName, header.playerLevel, header.inventoryCount);
            break;
        }
        default:
            PLOG_INFO("[PULSAR] Packet type 0x{:02X} ({} bytes)"sv,
                static_cast<uint8_t>(reader.Type()), reader.PayloadSize());
            break;
        }
    }

    void NetworkHost::OnDisconnect(ENetPeer* a_peer, uint32_t a_reason)
    {
        const char* reasonStr = "unknown";
        switch (a_reason) {
            case 0: reasonStr = "graceful"; break;
            case 1: reasonStr = "timeout"; break;
            case 2: reasonStr = "reset"; break;
        }
        PLOG_INFO("[PULSAR] Peer disconnected - reason={} ({}) rtt={}ms loss={:.1f}%"sv,
                  a_reason, reasonStr,
                  a_peer ? a_peer->roundTripTime : 0,
                  a_peer ? (a_peer->packetLoss / 65536.0f * 100.0f) : 0.0f);
        NASLOG("Peer disconnected reason=%d (%s)", a_reason, reasonStr);
        if (m_peer == a_peer) {
            m_peer = nullptr;
            Pulsar::Voice::VoiceBridge::OnPeerDown();  // stop voice capture
        }
        // L.7 V9.4 - reset established flag so handshake-on-reconnect waits
        // for the new OnConnect callback rather than firing while CONNECTING.
        m_connectionEstablished.store(false, std::memory_order_release);

        // tear down the E2E session; a reconnect derives a fresh ephemeral key.
        m_crypto.Reset();
        {
            std::lock_guard<std::mutex> qk(m_cryptoQueueMtx);
            m_cryptoReady = false;
            m_preCryptoTx.clear();
        }

        // Abort any in-flight quicksave transfer
        m_quicksavePending = false;
        m_quicksaveCandidate.clear();

        // Clear NPC-sync tracking so no stale state carries over to reconnect
        Pulsar::NPCSync::Reset();
        Pulsar::QuestSync::NotePeerDisconnected();
        Pulsar::QuestSync::Reset();
        Pulsar::DialogSync::Reset();

        // M.3 Player Inventory Separation - capture client's current state
        // to PULSAR_Profiles/client_state.psprof so the next MP session
        // starts from this state instead of host's loaded inventory. Host
        // is no-op (their save is canonical). Queues onto game thread.
        Pulsar::InventorySync::OnNetworkDisconnect();

        // Lobby auto-save: back up the player's character (loot/credits gained this
        // session) so it travels to the next session. Flag only - the actual capture
        // runs on the game thread (TickAutoSave), safe-point gated + debounced.
        Pulsar::Lobby::RequestAutoSave();

        // Despawn clone so it's clean for reconnect
        auto& remoteMgr = RemotePlayer::GetRemotePlayerManager();
        remoteMgr.RequestDespawn();

        if (m_isHost) {
            Companion::GetCompanionManager().RequestEnableAI();
        }

        // Refused for a version mismatch: say what to install and do NOT retry,
        // a reconnect would only be refused again every 3 s.
        Compat::Version hostV;
        const bool versionRejected = !m_isHost && Compat::DecodeVersionReject(a_reason, hostV);
        if (versionRejected) {
            PLOG_DIAG("[PULSAR] co-op: the host refused this connection - it runs PULSAR {}, "
                      "you run {}.{}.{}. Install the same version as the host. Not reconnecting."sv,
                      Compat::ToString(hostV), Plugin::Version.major(), Plugin::Version.minor(),
                      Plugin::Version.patch());
        }

        // Host stays listening - client can reconnect without restart
        if (!m_isHost && !m_lastAddress.empty() && !versionRejected) {
            m_reconnectAt = GetTickCount64() + 3000;  // try reconnect in 3s
            PLOG_INFO("[PULSAR] Will auto-reconnect in 3s..."sv);
        }
    }
}
