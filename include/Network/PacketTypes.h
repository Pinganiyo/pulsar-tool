#pragma once

#include <cstdint>
#include <cstring>

namespace Pulsar::Network
{
    enum PacketType : uint8_t {
        PKT_HANDSHAKE        = 0x01,
        PKT_POSITION         = 0x02,
        PKT_ANIMATION        = 0x03,
        PKT_INPUT            = 0x04,
        PKT_INTERACT         = 0x05,
        PKT_DROP_ITEM        = 0x06,
        PKT_INVENTORY        = 0x07,
        PKT_ITEM_TAKEN       = 0x08,
        PKT_ITEM_DENIED      = 0x09,
        PKT_CRAFT_ITEM       = 0x0A,
        PKT_CRAFT_RESULT     = 0x0B,
        PKT_DIALOG_START     = 0x10,
        PKT_DIALOG_VOTE      = 0x11,
        PKT_DIALOG_END       = 0x12,
        PKT_XP_GAIN          = 0x20,
        PKT_PLAYER_STATUS    = 0x30,
        PKT_LOADSCREEN_START = 0x31,
        PKT_LOADSCREEN_READY = 0x32,
        PKT_SHIP_BLUEPRINT   = 0x40,
        PKT_SHIP_BUILDER_ADD = 0x41,
        PKT_SHIP_BUILDER_REM = 0x42,
        PKT_SHIP_BUILDER_MOV = 0x43,
        PKT_SHIP_BUILDER_END = 0x44,
        PKT_EQUIP            = 0x0C,
        PKT_EQUIPMENT        = 0x0D,  // full equipment sync (weapon + armor, one packet)
        PKT_BONES            = 0x50,
        PKT_FIRE             = 0x51,
        PKT_CLIENT_PROFILE   = 0x60,  // Client → Host: full profile (inventory + equipment)
        PKT_SAVE_TRANSFER    = 0x70,  // Host → Client: raw .sfs save file
        PKT_NPC_STATE        = 0x71,  // Host → Client: batch of NPC position/health/dead state (CellSync A)
        PKT_VEHICLE_POS      = 0x72,  // Host → Client: FURN-REFR position (Moon Jumper etc.) when host occupies it
        PKT_LOOT_PICKUP      = 0x73,  // Host → Client: loot item just got picked up on host - replay on client
        PKT_CONTAINER_TAKE   = 0x74,  // Either side: one item was just taken from a container - remove same item from peer's mirror container
        PKT_LOOT_DROP        = 0x75,  // Either side: player dropped item(s) from inventory - peer spawns matching REFR at clone position
        PKT_DOOR_STATE       = 0x76,  // Either side: player activated a door - peer replays Activate so the door animates open/closed in sync
        PKT_LOCK_STATE       = 0x77,  // Either side: lockable REFR's locked-flag flipped - peer mirrors REFR_LOCK->flags (typ. lockpick success)
        PKT_CONTAINER_OPEN   = 0x78,  // Either side: kCONT REFR was opened (Activate success on unlocked) - peer SetOpens for sustained-open mirror
        PKT_GRAB_STATE       = 0x79,  // Either side: player E-Hold grabbed a REFR - start, position-update-while-held, and release. Peer mirrors REFR position via SetPosition vt[0x89] while grabbed=1; on grabbed=0 stops mirroring (engine physics takes over).
        PKT_ACTI_STATE       = 0x7A,  // Either side: player activated a kACTI REFR (switch, button, lever, power-conduit) - peer replays Activate via clone-activator so the activator's script + animation run locally.
        PKT_FORM_BLOB        = 0x7B,  // Host → Client: serialized TESForm blob (engine-format via vt[0x1A] SaveGame). Used for cell-transition biped re-init - client deserializes onto matching clone via vt[0x1B] LoadGame so engine treats clone like a save-loaded form.
        PKT_TRANSIT          = 0x7C,  // Either side: peer transitioned cells (door, fast-travel, ship-landing, gravjump). Receiver of K.1 logs only; K.2 auto-follow on later phase.
        PKT_SPACE_TRANSITION = 0x7D,  // Either side: ship state event - TakeOff, Landing, GravJump, Dock. Detected via SFSE Spaceship event sinks (RVA 0x2160390 etc.). Phase K.7.
        PKT_CELL_CONTENT_BATCH = 0x7E, // Host → Client: cell-content snapshot - list of "interesting" REFRs (CONT/DOOR/ACTI/FURN/REFR/ACHR) on host's cell. Phase K.6 V1 - log-only on receiver. V2 will add spawn pipeline.
        PKT_SHIP_POS         = 0x7F,  // Either side (pilot): spaceship REFR position + Euler rotation. Receiver writes to local same-FormID ship REFR. Phase L.3.
        PKT_FLIGHT_STATE     = 0x80,  // Pilot → peer: ShipHudDataModel StickDataProvider state (throttle, yaw/pitch input, boost, reverse, cruise). Receiver mirrors values into local StickDataProvider so cockpit gauges + control state match host. Phase L.3 V2.
        PKT_FLIGHT_INPUT     = 0x81,  // Active pilot → passive: raw flight-input axes (yaw/pitch/throttle) read from MovementHandlerAgentPlayerFlightControls::vt[7]. Receiver overrides its own handler's +0x40 vec3 buffer so its engine processes host's input as if local - both engines compute identical ship state, visual flight matches. Phase L.3 V2.
        PKT_PMO_PAYLOAD      = 0x82,  // Active pilot → passive: full Spaceship::PlayerMovementOutputEvent payload (64 bytes), captured by hooking BSTEventSource::Notify (FUN_1422CB8F0) on the PMO singleton. Receiver overwrites its own about-to-be-fired payload with these bytes so all sinks (HUD, FlightController, ship physics) process host's exact state. Phase L.3 V2.2.
        PKT_SHIP_TRANSFORM   = 0x83,  // Active pilot → passive: ship NiNode world transform - 9-float rotation matrix + 3-float translation = 48 bytes. Bypasses event/input layers; host reads ship.loadedData->data3D->world (rotate@+0x80, translate@+0xB0) per-frame and broadcasts; client writes the same to its local ship NiNode pre-render so the renderer sees host's exact transform regardless of local physics state. Phase L.3 V3.
        PKT_SHIP_FIRE        = 0x84,  // Active pilot → passive: weapon fire event - host detects via BSTEventSource<WeaponFiredEvent> (REL::ID 412442) match in NotifyDispatch hook, broadcasts {shipFormID, weaponPtr, baseProjPtr, ts}. V1 minimal: client logs receipt (visual stub). V1.1+ adds VFX/sound. V2 (with NPCSync-Space) adds real projectile spawn. Phase L.4.
        PKT_NPC_SHIP_BATCH   = 0x85,  // Host → Client: batch of non-player ship havok transforms. Phase M.1 V2. 1Hz heartbeat. Each entry = {handleHash from host param_2 ptr, havokState[16]}. Client position-matches each remote entry to a local body (translation in havokState[12..14]) and applies via SafeWriteHavokState.
        PKT_CRUISE_STATE     = 0x86,  // Host → Client: cruise mode toggle. Detected on host by polling cruise-active global byte @ RVA 0x5EAA5AE (the byte that IsCruiseModeActive reads). Client applies via call to cruise-toggle function @ RVA 0x13FDAC0 with bool. Phase L.5.
        PKT_CRUISE_ORIENT    = 0x87,  // Host → Client: cruise orient triple (DAT_145F31DA0/DA8/DB0, 3 doubles), broadcast continuously @5Hz during active cruise. Phase L.5 Phase 4 Step 11. Memory diff (Step 10) showed these doubles evolve on host but stagnate on client between arrivalInit/EXIT events - likely the missing planet-rendering / NPC-ship signal.
        // 0x88 (PKT_DOCK_STATE) + 0x89 (PKT_HATCH_TRANSIT) reserved - both
        // attempted in L.6 V1/V2 (2026-05-02), both removed (see note above).
        PKT_DOCK_FINISHED    = 0x8A,  // Host → Client: ship finished/started docking. L.6 V3 Phase 2. Sent on Spaceship::DockEvent fire when payload byte [0x10]=1 ("started") flips from 0 - engine has committed to the dock, cinematic about to begin. Carries shipFormID + dock-port REFR FormID + ship pos/quat. Phase 2 = receiver logs only. Phase 3 = receiver replays the engine dispatcher (RVA 0x14213A130 / REL::ID 120238) so client's engine runs ComponentDB registration + cell-transit naturally.
        PKT_DOCK_EXTRATELEPORT = 0x8B,  // Host → Client: cockpit-hatch ExtraTeleport pair. L.6 V3 Phase 3b. After dock cutscene, engine on host calls AddOrGetExtraTeleport (RVA 0xBA0020) on the cockpit hatch REFR (kDOOR base 0x00009CF4) - the dispatcher RVA 0xB47920's gate then takes the "ExtraTeleport branch" and the activate fires. On client this never happens because dock state machine isn't driven. Host polls cockpit cell post-(1,0) DockEvent; when local hatch's ExtraTeleport.inner becomes non-null, captures the 28-byte inner struct + sends. Client receives, finds local kDOOR base 0x9CF4 in cockpit cell, calls AddOrGetExtraTeleport, writes captured data, sets refr.flags16 |= 0x40 - hatch becomes activatable on client without cross-cell MoveTo.
        PKT_DOCK_UNDOCKED      = 0x8C,  // Either-side: ship has undocked (TakeOffEvent fired on local pilot). Tells other peers to clean up their applied dock-state on the cockpit hatch (CLEAR our bits, call FUN_140B37390(hatch,0) = disable-for-dock, optionally clear ExtraTeleport inner-ptr). Symmetric counterpart of PKT_DOCK_EXTRATELEPORT. L.6 V3 Phase 3e.
        PKT_DOCK_STATION_XT    = 0x8D,  // Pilot-side → others: station-side hatch ExtraTeleport bytes captured by pilot's engine (which ran SetUpDockExtraTeleport during cutscene). Pilot broadcasts when their player crosses cockpit→station and finds the local station-side hatch with populated XT. Receivers store + apply via Phase 3f to enable return-to-ship without hardcoded values. Generalizes to all ships (vanilla Frontier, Razorleaf, custom-builds) since values are ship-specific cockpit-side coords. L.6 V3 Phase 3i.
        PKT_DOCK_FORWARD_BRIDGE_EXPECTED = 0x8E,  // Host → Client: heads-up that host just exited a procedural ship into station and is firing a save-bridge. Client should suppress its own cockpit-side hatch activate until the save arrives - otherwise client teleports to its OWN procedural ship instance instead of host's, sees a different ship at the dock, etc. L.6 V3 Phase 3i v17.
        PKT_FAST_TRAVEL_SYNC = 0x8F,  // Host → Client: ship has finished a Star Map fast-travel jump. Carries shipFormID + dest cell FormID + dest worldspace FormID + dest position. Client uses engine MoveTo (RVA 0x14191EFB0) on its local same-FormID ship to teleport into the destination cell, triggering the engine cell-streamer so events/NPCs/planets in the destination system load on client too. L.7 V7. Triggered by sinking PlayerFastTravel::Event (RVA 0x597A360, _EX singleton style) on host, with a 3s settle delay to ensure ship.parentCell has been updated to the destination.
        PKT_CLIENT_LOAD_COMPLETE = 0x90,  // Client → Host: client has finished loading a host-bridged save (initial connect or any subsequent fast-travel save-bridge). Sent from WorldSync once g_state == LOADED. Lets the host gate further save-bridge fires on "client is in a stable in-world state" instead of guessing via timers - fixes the L.7 v6 bug where save-bridge fired during the initial save-load window. Payload: uint8 reason (0 = initial-connect, 1 = post-bridge), uint64 timestamp.
        PKT_HOST_REQUEST_RECONNECT = 0x91,  // Host → Client: please drop the connection and auto-reconnect. Used by L.7 V9 when the engine 2nd-mid-game-save-load limit blocks a save-bridge fire - client reconnects, hits the connect-time save flow which is the engine's full-reset path and always works. Payload: uint8 reason (1 = L.7 cross-system resync). Client calls Disconnect() on receipt; existing m_reconnectAt logic schedules auto-reconnect in 3s.
        PKT_CIVILIAN_SNAPSHOT = 0x92,  // Host → Client: low-Hz position snapshot of civilian (LOCAL-tier) NPCs. Engine simulates AI deterministically per-machine but FPS / package-tick / RNG diverges over time so positions drift. 2 Hz batch keeps shared static-ref civilians visually consistent at ~2 KB/s. NPCSync V5.
        PKT_QUEST_STAGE      = 0x93,  // Either side: TESQuestStageEvent fired locally on a TESQuest. Sender broadcasts {questFormID, stageID}; receiver looks up local TESQuest by FormID and calls native worker FUN_140D0E1F0(quest, stage) with g_applyingRemoteStage guard so the worker's internal Notify doesn't echo back. L.8 Phase 1.
        PKT_QUEST_START      = 0x94,  // Either side: TESQuestStartStopEvent fired with started=1. Sender broadcasts {questFormID, initialStage}; receiver applies via Quest.Start path. L.8 Phase 2 (placeholder slot - Phase 1 only sends stage updates; this slot reserved so future phase doesn't shift IDs).
        PKT_QUEST_STOP       = 0x95,  // Either side: TESQuestStartStopEvent fired with started=0. Sender broadcasts {questFormID, completed}; receiver stops/completes locally. Reserved Phase 2 slot.
        PKT_NPC_DAMAGE       = 0x96,  // Client → Host: client-dealt combat damage to a host-matched NPC. Payload NpcDamageData{hostRefFormID, damage}. Host subtracts `damage` from that NPC's Health (engine processes death at <=0); the reduced Health + isDead echo back via the existing PKT_NPC_STATE path. Combat Sync V2 Step 1.
        PKT_PLAYER_DOWNED    = 0x97,  // Either side → peer: local player entered (downed=1) or left (downed=0) the downed state. Authority = sender's own machine (each player owns its mortality). Receiver tracks it as g_remoteDowned; when BOTH local+remote are downed each machine stops suppressing its own reload → controlled both-downed game-over (load last save). Coop Downed & Revive  Feature A.
        PKT_PLAYER_REVIVE    = 0x98,  // Partner → downed peer: medpack-revive request. Authority stays with the downed machine (it runs ReviveLocalPlayer on receipt at 50% HP). Payload PlayerReviveData{playerSlot = the DOWNED player}. Coop Downed & Revive  Feature A, Inc4.
        PKT_PLAYER_COMBAT    = 0x99,  // Either side → peer: sender's player combat state changed (debounced on/off). Lets a DOWNED partner gate its combat-end auto-revive (Way B) on the FIGHTER'S combat instead of its own - the downed player's local enemies disengage the instant it goes down, so its own hostiles-scan reads "clear" mid-fight and it pops back up (confirmed 2026-06-09). Payload PlayerCombatData{inCombat,slot}. Sent reliably on change (latest-wins). Coop Downed & Revive  Feature A.
        PKT_EQUIP_V2         = 0x9A,  // Either side → peer: full worn-equipment snapshot of the sender's LOCAL player (current weapon + all equipped ARMO pieces), each with plugin-encoded FormID + mod array + sentinel (LootDropMod trailer per item, same encoding as PKT_LOOT_DROP). Receiver resolves cross-plugin and (un)equips the partner CLONE incl. mod apply before equip. Sent on change + 10s heartbeat. F.2-v2 (2026-06-11) - replaces the 2026-04-25-disabled PKT_EQUIP/PKT_EQUIPMENT.
        PKT_CORPSE_LOOT      = 0x9B,  // Host → Client: authoritative corpse inventory of a freshly-dead NPC (each machine rolls loot with its OWN RNG → contents diverge; the F.1.e take-sync then warns+skips on missing items). Sent ONCE ~1s after the death edge (engine appends death items at kill time). Client wipes its matched corpse and applies the host list incl. mod arrays + sentinels (epics/legendaries render identically). Wire: [type][CorpseLootHeader][(CorpseLootItem + modCount×LootDropMod) × itemCount].
        PKT_BRIDGE_REQUEST   = 0x9C,  // Client → Host: "I can't AutoFollow you" - sent when the client's AutoFollow has to SKIP a teleport because it would cross a procedural-instance / worldspace boundary (host fast-travelled to a planet/settlement the client can't reach via SetPosition). Header-only (no payload). Host replays the F8 manual-bridge path (TriggerHostAutoSave + arm autosave-checker manual-force) on the GAME thread (drained in TickManualBridgeHotkey), debounced ~20s, so the client load-syncs into the host's location automatically. F8 stays as the manual fallback. L.7.
        PKT_KEY_MIRROR       = 0x9D,  // Either side → peer: a KEY/keycard (FormType kKEYM) appeared in the local player's inventory (looted, regardless of who). Mirror it into the peer's inventory so neither player gets stuck at a loading-screen door that needs the key. Payload KeyMirrorData{base, localID, plugin} (cross-plugin resolved). Receiver adds the key if missing; idempotent (tracked in s_knownKeys both sides → no echo). Keys are safe to duplicate (door only checks "has key X"); quest turn-in items are NOT mirrored.
        PKT_SHIP_AV          = 0x9E,  // Host → Client: shared-host-ship combat ActorValues (hull cur/max). Read via ship+0x70 ActorValueOwner (vt[1]=GetActorValue cur, vt[2]=GetPermanent ≈ max), hull-AV form from FUN_14049acc0()+0x108. Client mirrors onto its local same-FormID ship via ModActorValue(kDamage, hullInfo, target-localCur), clamped >0 so the mirror never destroys the client ship. Payload ShipAvData. Slice 2 = hull only; shield/subsystems later.
        PKT_TIME_WEATHER     = 0x9F,  // Host → Client: one-shot game time-of-day + current weather, sent in reply to PKT_TIME_WEATHER_REQ. Client sets the Calendar globals (gameHour @ cal+0x28, gameDaysPassed @ cal+0x30, value @ +0x48; Calendar ptr @ RVA 0x5FDCDF8) and force-activates the weather form (engine setter FUN_1418c63f0 @ RVA 0x18C3BE0, ctrl @ RVA 0x61DF748; current weather read from Sky @ RVA 0x61DF6F0 +0x118) so both see the same time + weather. Payload TimeWeatherData.
        PKT_TIME_WEATHER_REQ = 0xA0,  // Client → Host: "send me your current time + weather" (header-only, 1 byte). Client fires it once on join and retries every few seconds until a snapshot is applied.
        PKT_CRYPTO_HELLO     = 0xA1,  // Both sides on connect: ephemeral ECDH P-256 public key for E2E key agreement. Sent UNENCRYPTED (public keys are safe); after both exchange, all further packets are AES-256-GCM encrypted (Crypto.h). Wire: [0xA1][BCRYPT_ECCPUBLIC_BLOB].
        PKT_KILL_XP          = 0xA2,  // Either side → peer: a kill just awarded XP to the LOCAL player (captured by hooking the engine XP-award primitive FUN_141962310, source==7=KILL). Carries the host-canonical NPC FormID + the final awarded XP. Receiver awards the SAME XP to its own player (source=0xc, no multiplier) UNLESS it already earned natural kill-XP for that host-ref (g_myNaturalKills dedup) - so a scripted-mirror kill (which awards no natural XP) still grants both players the kill's XP. Co-op Kill-XP-Sharing.
        PKT_NPC_SPAWN        = 0xA3,  // Host → Client: a persistent quest NPC just became active (enabled/placed) in the host player's cell - e.g. Block-Event-quest dropship pirates the client's own (non-deterministic) BE alias-fill never produces, and which NPCSync can't bring in (it only updates EXISTING NPCs + caps at 20). Detected via ActorCellChangeEvent ENTER (cellA==0). Client LookupByID the same persistent FormID and Enable+place it (REL::ID 63386, idempotent, safe off-VM). NPC-spawn-replication.
        PKT_QUEST_STAGE_REPLAY = 0xA5,  // Host -> Client: a quest stage the host set while the client was disconnected, replayed on a reconnect that skipped the save bridge (QuestSync/StageJournal.h). Same wire as PKT_QUEST_STAGE; the client skips stages it set itself during the gap. 0.5.0.
        PKT_VOICE            = 0xA4,  // Either side -> peer: one Opus voice frame on unreliable ENet channel 2. Wire: [1B type][2B seq][1B flags][opus]. Framed/handled by VoiceBridge, never reaches the PacketReader switch. Proximity Voice .
    };

    // All packets start with this header
#pragma pack(push, 1)
    struct PacketHeader
    {
        PacketType type;
        uint16_t   size; // payload size (excluding header)
    };
#pragma pack(pop)

    static constexpr size_t HEADER_SIZE = sizeof(PacketHeader); // must be 3

    // Complete animation graph variable snapshot (synced with each position update)
    struct AnimVarSnapshot
    {
        // Floats
        float speed       = 0.0f;  // "Speed"
        float currentSpeed = 0.0f; // "CurrentSpeed" (from CharacterController)
        float direction   = 0.0f;  // "Direction"
        float turnDelta   = 0.0f;  // "TurnDelta" (instant)
        float lookHeading = 0.0f;  // "LookAtHeadingVar"
        float lookPitch   = 0.0f;  // "LookAtPitchVar"
        float turnDeltaSmoothed = 0.0f;  // "TurnDeltaSmoothed" - anim lean-blend input (T121)

        // Integers
        int32_t iState               = 0;  // "iState"
        int32_t iSyncIdleLocomotion  = 0;  // "iSyncIdleLocomotion"
        int32_t iSyncSprintState     = 0;  // "iSyncSprintState"
        int32_t iSyncRelaxReadySneak = 0;  // "iSyncRelaxReadySneak"
        int32_t iSyncJumpState       = 0;  // "iSyncJumpState"
        int32_t iSyncGunActions      = 0;  // "iSyncGunActions" - fire/reload state
        int32_t iSyncGunDown         = 0;  // "iSyncGunDown" - weapon-down marker
        int32_t iSyncStrafeState     = 0;  // "iSyncStrafeState" - strafe L/R (T121)
        int32_t iSyncTurnState       = 0;  // "iSyncTurnState" - turn/lean state (T121)

        // Booleans (packed)
        uint8_t boolFlags = 0;
        // bit 0: IsCompletelyStopped
        // bit 1: bPlayerMoveStartActive
        // bit 2: IsTurning (T121)
        // bit 3: FullBodyTurn (T121)
    };

#pragma pack(push, 1)
    // PKT_POSITION payload - position + animation variables piggybacked
    struct PositionData
    {
        // Position
        float x;
        float y;
        float z;
        float rotX;
        float rotY;
        float rotZ;

        // Animation variables (piggybacked for sync)
        float speed;          // "Speed" behavior graph variable
        float direction;      // "Direction" behavior graph variable
        float lookHeading;    // "LookHeadingOffset" - head up/down look
        uint16_t animFlags;   // Packed state flags (legacy, used for weapon debounce):
                              //   bits 0-1: iSyncIdleLocomotion (0=idle, 1=locomotion)
                              //   bits 2-3: iSyncSprintState (0=no sprint, 1=sprinting)
                              //   bits 4-6: iSyncRelaxReadySneak (0=relaxed,1=ready,2=sneak,3=alert)
                              //   bit 7:    IsCompletelyStopped
                              //   bit 8:    bPlayerMoveStartActive
                              //   bit 9:    weapon drawn (sticky)
                              //   bit 10:   seated
                              //   bit 11:   hostSighted (2026-04-25) - host is aiming with RMB
                              //   bit 12:   (free)
                              //   bit 13:   (free)
        AnimVarSnapshot vars; // Full animation variable snapshot

        // Phase I (2026-04-27): sender's parentCell formID. Lets the receiver
        // detect cross-cell mismatch (each side has a different procedural-
        // cell instance after walking through the same door). Without this,
        // applying a remote position whose cell differs from the local player's
        // cell causes the engine to repeatedly cell-flip the clone REFR
        // (verified from 20:25/20:26 logs: clones pingponging between
        // FF019A78 ↔ FF01BFAA on host, FF0199EA ↔ FF01BE97 on client).
        // 0 = sender has no parentCell yet (initial / pre-load state).
        uint32_t cellFormID;
    };
#pragma pack(pop)

    // PKT_HANDSHAKE payload
    struct HandshakeData
    {
        uint8_t  versionMajor;
        uint8_t  versionMinor;
        uint8_t  versionPatch;
        uint8_t  dlcFlags; // Bit 0 = Shattered Space, Bit 1 = Terran Armada
        char     playerName[32];
    };

#pragma pack(push, 1)
    // PKT_INTERACT payload - client wants to activate a world object
    struct InteractData
    {
        uint32_t targetFormID;  // FormID of the object to interact with
    };

    // PKT_INVENTORY payload - single inventory entry (sent as array)
    struct InventoryEntry
    {
        uint32_t formID;    // Base object FormID
        int32_t  count;     // Stack count
        uint8_t  equipped;  // 1 = equipped, 0 = not
    };

    // PKT_ITEM_TAKEN payload - confirmation that item was picked up
    struct ItemTakenData
    {
        uint32_t targetFormID;  // The ref that was activated
        uint32_t itemFormID;    // Base object that was acquired
        int32_t  count;         // How many
    };

    // PKT_ITEM_DENIED payload - interaction failed
    struct ItemDeniedData
    {
        uint32_t targetFormID;  // The ref that failed
        uint8_t  reason;        // 0 = already taken, 1 = too far, 2 = invalid
    };

    // PKT_DROP_ITEM payload - client wants to drop an item
    struct DropItemData
    {
        uint32_t itemFormID;  // Base object FormID to drop
        int32_t  count;       // How many to drop
    };

    // PKT_EQUIP payload - weapon equip sync
    struct EquipData
    {
        uint32_t weaponFormID;  // 0 = no weapon (unequipped)
    };

    // PKT_BONES payload - bone transforms for animation sync
    // 19 bones × quaternion (x,y,z,w) + hips position
    static constexpr int BONE_COUNT = 19;
    struct BoneData
    {
        float rotations[BONE_COUNT * 4];  // quaternions: x,y,z,w per bone
        float hipsPos[3];                 // hips position offset
    };
    // = 19*16 + 12 = 316 bytes

    // PKT_FIRE payload - fire action from player
    struct FireData
    {
        uint32_t actionFormID;  // e.g. 0x00004A5C = ActionFireAuto
    };

    // PKT_EQUIPMENT payload - full equipment sync in ONE packet
    // Sent on connect + whenever equipment changes (polled every 2s)
    static constexpr int MAX_EQUIP_SLOTS = 16;
    struct EquipmentData
    {
        uint32_t weaponFormID;                    // currently equipped weapon (0 = none)
        uint8_t  armorCount;                      // number of armor items following
        uint32_t armorFormIDs[MAX_EQUIP_SLOTS];   // equipped armor FormIDs
    };
    // Total: 4 + 1 + 64 = 69 bytes max

    // PKT_CLIENT_PROFILE payload - variable length
    // Header followed by inventoryCount * ProfileItemNet entries
    struct ClientProfileHeader
    {
        char     playerName[32];
        uint32_t playerLevel;
        uint32_t weaponFormID;
        uint8_t  armorCount;
        uint32_t armorFormIDs[MAX_EQUIP_SLOTS];
        uint16_t inventoryCount;
    };
    // = 32 + 4 + 4 + 1 + 64 + 2 = 107 bytes

    struct ProfileItemNet
    {
        uint32_t formID;
        int32_t  count;
    };
    // = 8 bytes each

    // PKT_NPC_STATE payload - batch of NPC states sent from host to client.
    // Wire format: [uint8 type][NPCStateBatchHeader][NPCStateData × count]
    // Uses its own framing (no PacketHeader), matches PKT_SAVE_TRANSFER style.
    struct NPCStateBatchHeader
    {
        uint32_t cellFormID;   // Cell these NPCs belong to (for client cell consistency check)
        uint16_t count;        // Number of NPCStateData entries following
    };
    // = 6 bytes

    // Max plugin filename we transmit. Bethesda filenames are typically well under
    // 32 bytes ("ShatteredSpace.esm" = 18). 64 gives headroom for mods w/o wasting bandwidth.
    static constexpr int NPC_SOURCE_PLUGIN_MAX = 64;

    struct NPCStateData
    {
        uint32_t refFormID;     // Runtime reference FormID (often 0xFFxxxxxx dynamic, won't resolve on client)
        uint32_t baseFormID;    // TESNPC direct base - may be dynamic (0xFF) for POI-spawned mobs
        uint32_t sourceBaseFormID; // Static source template FormID (T28: TESNPC+0x130). 0 when baseFormID is already static. HOST-LOCAL formID (host load-order).
        uint32_t sourceLocalID;    // Plugin-local portion of sourceBaseFormID (flag-aware mask). Client re-encodes using ITS load order via sourcePlugin.
        char     sourcePlugin[NPC_SOURCE_PLUGIN_MAX]; // Filename of the plugin that owns sourceBaseFormID (e.g. "Starfield.esm", "ShatteredSpace.esm"). Null-terminated. Empty => base is dynamic / unknown.
        float    posX, posY, posZ;
        float    rotZ;          // Z-axis rotation only (Starfield convention)
        float    speed;         // Graph "Speed" - mirrors player-clone sync
        float    direction;     // Graph "Direction" - body-relative heading
        float    health;        // Current health (ActorValue kHealth)
        float    healthMax;     // Permanent max health (for bar scaling)
        uint8_t  flags;         // bit 0 = isDead, bit 1 = weaponDrawn
        uint8_t  moveFlags;     // bit 0 = isMoving, bit 1 = isSprinting
                                // (bit 2+ reserved; weaponDrawn stays in `flags`)
    };
    // = 16 (IDs) + 64 (plugin) + 12 (pos) + 4 (rotZ) + 8 (speed+dir) + 8 (health×2) + 2 (flags+moveFlags) = 114 bytes
#pragma pack(pop)

    static_assert(sizeof(NPCStateBatchHeader) == 6, "NPCStateBatchHeader size");
    static_assert(sizeof(NPCStateData) == 114, "NPCStateData size");

#pragma pack(push, 1)
    // PKT_NPC_DAMAGE payload - single client→host combat-damage event.
    // Wire format: [uint8 type][NpcDamageData]. Own framing (no PacketHeader),
    // same style as PKT_NPC_STATE. Sent reliably - deltas are cumulative, a
    // dropped one = lost damage.
    struct NpcDamageData
    {
        uint32_t hostRefFormID;  // The HOST's enemy FormID (key into host g_tracked / LookupByID). Comes from the client's g_npcRemap host-side key.
        float    damage;         // Positive final damage to subtract from the host enemy's Health this event. Ignored when kill=1.
        uint8_t  kill;           // 1 = the client's LOCAL copy of this enemy died - kill the host's matched enemy outright (SafeKillActor), bypassing the damage-vs-regen race. 0 = ordinary damage event. V2-Step-1d.
    };
    // = 9 bytes
#pragma pack(pop)

    static_assert(sizeof(NpcDamageData) == 9, "NpcDamageData size");

#pragma pack(push, 1)
    // PKT_PLAYER_DOWNED payload - local player's downed state changed.
    // Wire format: [uint8 type][PlayerDownedData]. Own framing (no PacketHeader),
    // same style as PKT_NPC_DAMAGE. Sent reliably (state change must not be lost).
    // Bidirectional: each machine owns its own player's mortality and broadcasts
    // transitions. Receiver stores it as the peer's downed state; when both the
    // local and remote players are downed, each side lets its own vanilla reload
    // fire → controlled both-downed game-over. Coop Downed & Revive .
    struct PlayerDownedData
    {
        uint8_t downed;     // 1 = entered downed, 0 = revived / back up
        uint8_t playerSlot; // 0 = host, 1 = client (informational / diagnostics)
        uint8_t pad[2];
    };
    // = 4 bytes
#pragma pack(pop)

    static_assert(sizeof(PlayerDownedData) == 4, "PlayerDownedData size");

    // PKT_PLAYER_REVIVE payload - a partner asks the downed player to get up.
#pragma pack(push, 1)
    struct PlayerReviveData {
        uint8_t playerSlot;   // 0 = host, 1 = client - the DOWNED player being revived
        uint8_t pad[3];
    };
#pragma pack(pop)
    static_assert(sizeof(PlayerReviveData) == 4, "PlayerReviveData size");

    // PKT_PLAYER_COMBAT payload - sender's player combat state (debounced).
#pragma pack(push, 1)
    struct PlayerCombatData {
        uint8_t inCombat;     // 1 = sender is in an active fight, 0 = clear
        uint8_t playerSlot;   // 0 = host, 1 = client (diagnostics)
        uint8_t pad[2];
    };
#pragma pack(pop)
    static_assert(sizeof(PlayerCombatData) == 4, "PlayerCombatData size");

    // PKT_EQUIP_V2 - F.2-v2 live worn-equipment sync.
    // Wire format: [uint8 type][EquipV2Header][ (EquipV2Item + modCount × LootDropMod) × itemCount ]
    // LootDropMod is defined further below (shared with PKT_LOOT_DROP).
#pragma pack(push, 1)
    struct EquipV2Header
    {
        uint8_t itemCount;    // ≤ kEquipV2MaxItems
        uint8_t playerSlot;   // 0 = host, 1 = client (diagnostics)
        uint8_t pad[2];
    };
    struct EquipV2Item
    {
        uint32_t base;          // sender-local base FormID
        uint32_t localID;       // plugin-local portion (flag-aware mask)
        char     plugin[NPC_SOURCE_PLUGIN_MAX];
        uint8_t  isWeapon;      // 1 = current weapon (kWEAP), 0 = armor (kARMO)
        uint8_t  modCount;      // LootDropMod entries trailing THIS item
        uint16_t modSentinel;   // kObjectInstance +0x28 (0xFFFF = default)
    };
#pragma pack(pop)
    static constexpr uint8_t kEquipV2MaxItems       = 20;
    static constexpr uint8_t kEquipV2MaxModsPerItem = 24;
    static_assert(sizeof(EquipV2Header) == 4, "EquipV2Header size");
    static_assert(sizeof(EquipV2Item) == 76, "EquipV2Item size");

    // PKT_CORPSE_LOOT - host-authoritative corpse inventory (see enum docs).
#pragma pack(push, 1)
    struct CorpseLootHeader
    {
        uint32_t hostRefFormID;   // host runtime REFR id - client maps via its NPC matcher
        uint8_t  itemCount;       // ≤ kCorpseLootMaxItems
        uint8_t  pad[3];
    };
    struct CorpseLootItem
    {
        uint32_t base;            // sender-local base FormID
        uint32_t localID;         // plugin-local portion
        char     plugin[NPC_SOURCE_PLUGIN_MAX];
        int32_t  count;
        uint8_t  modCount;        // LootDropMod entries trailing THIS item
        uint8_t  pad;
        uint16_t modSentinel;
    };
#pragma pack(pop)
    static constexpr uint8_t kCorpseLootMaxItems = 24;
    static_assert(sizeof(CorpseLootHeader) == 8, "CorpseLootHeader size");
    static_assert(sizeof(CorpseLootItem) == 80, "CorpseLootItem size");

#pragma pack(push, 1)
    // PKT_KEY_MIRROR payload  - a looted KEY/keycard (FormType kKEYM) to
    // mirror into the peer's inventory. Cross-plugin resolved like LootDropMod.
    struct KeyMirrorData
    {
        uint32_t base;            // sender-local base FormID
        uint32_t localID;         // plugin-local portion (flag-aware mask)
        char     plugin[NPC_SOURCE_PLUGIN_MAX];
    };
#pragma pack(pop)

#pragma pack(push, 1)
    // PKT_NPC_SPAWN payload  - host tells the client to ENABLE + place a
    // persistent quest NPC that became active on the host (BE-quest dropship
    // pirates etc.). Wire: [uint8 type][NpcSpawnData] (own framing, like
    // DoorStateData). refFormID is the host runtime REFR FormID; for PERSISTENT
    // refs (the only ones we replicate in v1) it is IDENTICAL on the client, so
    // LookupByID resolves the same disabled ref to enable. baseFormID/localID/
    // plugin let the client log/verify the base cross-plugin.
    struct NpcSpawnData
    {
        uint32_t refFormID;       // host runtime REFR FormID (persistent => same on client)
        uint32_t baseFormID;      // TESNPC base FormID (host-local)
        uint32_t baseLocalID;     // plugin-local portion of base (flag-aware mask)
        char     basePlugin[NPC_SOURCE_PLUGIN_MAX]; // plugin owning the base
        uint32_t cellFormID;      // cell the NPC entered on the host (co-location gate)
        float    posX, posY, posZ;
        float    rotZ;            // Z-axis rotation (Starfield convention)
    };
#pragma pack(pop)

#pragma pack(push, 1)
    // PKT_VOICE payload header - precedes the raw Opus blob on the ENet wire.
    struct VoiceFrameHeader
    {
        uint16_t seq;    // sender monotonic sequence, for jitter-buffer reorder/drop
        uint8_t  flags;  // bit 0 = talking (VAD/PTT open); reserved otherwise
    };
#pragma pack(pop)
    static_assert(sizeof(VoiceFrameHeader) == 3, "VoiceFrameHeader size");

#pragma pack(push, 1)
    // PKT_KILL_XP payload  - a kill awarded XP to the sender's LOCAL player.
    // Wire format: [uint8 type][KillXpData]. Own framing (no PacketHeader), same
    // style as PKT_NPC_DAMAGE. Bidirectional (each machine owns its own kills).
    //
    // hostRefFormID is the HOST-canonical NPC FormID so both sides key the
    // double-count guard identically: the host sends the victim's own FormID; the
    // client remaps its local victim through the combat-sync NPC remap first
    // (NPCSync::LocalRefToHostRef). 0 = could not canonicalize → sender skips.
    // xp = the exact final awarded XP (post-multiplier) the killer's engine gave,
    // so the receiver grants the same amount raw (source=0xc, no re-multiply).
    struct KillXpData
    {
        uint32_t hostRefFormID;   // host-canonical victim FormID (dedup key)
        float    xp;              // final awarded XP to mirror onto the peer
    };
#pragma pack(pop)
    static_assert(sizeof(KillXpData) == 8, "KillXpData size");

#pragma pack(push, 1)
    // PKT_CIVILIAN_SNAPSHOT payload - slim periodic position resync for
    // civilian (LOCAL-tier) NPCs. Engine runs AI deterministically per
    // machine but small timing differences accumulate, so we periodically
    // correct drift on shared static-ref civilians. No health, no anim,
    // no plugin-resolution: civilians keep simulating locally, we just
    // snap their position to host's value at low Hz.
    // Wire format: [uint8 type][CivilianSnapshotHeader][CivilianSnapshotData × count]
    struct CivilianSnapshotHeader
    {
        uint32_t cellFormID;
        uint16_t count;
    };
    // = 6 bytes

    struct CivilianSnapshotData
    {
        uint32_t refFormID;       // Static-ref civilians only - direct LookupByID on client
        float    posX, posY, posZ;
        float    rotZ;
    };
    // = 20 bytes
#pragma pack(pop)

    static_assert(sizeof(CivilianSnapshotHeader) == 6, "CivilianSnapshotHeader size");
    static_assert(sizeof(CivilianSnapshotData) == 20, "CivilianSnapshotData size");

#pragma pack(push, 1)
    // PKT_QUEST_STAGE payload - TESQuestStageEvent fired locally on a TESQuest.
    // Wire format: [uint8 type][QuestStageData]. Either side can send.
    // Receiver looks up quest by formID via TESForm::LookupByID, sets
    // g_applyingRemoteStage=true, calls native worker FUN_140D0E1F0(quest, stage)
    // (RVA 0xD0DAC0 - the engine's true stage-set primitive that runs all
    // subordinate stage scripts + recalc + Notify), then clears the flag.
    // The worker fires TESQuestStageEvent locally as part of its work; the flag
    // tells the broadcast sink to skip rebroadcast → no infinite loop.
    // L.8 Phase 1.
    struct QuestStageData
    {
        uint32_t questFormID;
        uint16_t stageID;
    };

    // PKT_QUEST_START / PKT_QUEST_STOP payload - TESQuestStartStopEvent fired
    // locally. Reserved L.8 Phase 2 slots (Phase 1 = stage only).
    struct QuestStartStopData
    {
        uint32_t questFormID;
        uint16_t stageOrFlag;  // start: initial stage; stop: 1=completed, 0=stopped/failed
    };
#pragma pack(pop)

    static_assert(sizeof(QuestStageData) == 6, "QuestStageData size");
    static_assert(sizeof(QuestStartStopData) == 6, "QuestStartStopData size");

#pragma pack(push, 1)
    // PKT_DIALOG_START payload - peer initiated a dialogue session.
    // Wire format: [uint8 type][DialogStartData]. Either side can send.
    // V1 Phase 3 = log-only on receiver. V2 will add UI toast, V3 lockout,
    // V4 BG3-style voting. Outcomes already sync via Phase 1 stage events.
    struct DialogStartData
    {
        uint8_t  playerSlot;   // 0 = host, 1 = client. Lets receiver know who's talking.
        uint32_t npcFormID;    // Actor pointer's FormID (raw +0x28). 0 if speaker not yet captured.
    };

    // PKT_DIALOG_END payload - peer closed dialogue (menu dismissed).
    struct DialogEndData
    {
        uint8_t  playerSlot;
    };

    // PKT_DIALOG_VOTE - Phase 3 V4 / say-mirror. When a peer fires a
    // TESTopicInfoEvent (an NPC speaks a line - scene/forced/ambient OR a menu
    // pick) we broadcast {playerSlot, topicInfoFormID, speakerFormID} so the
    // receiver can make the SAME NPC say the SAME line locally (voice +
    // subtitle + lipsync, no menu) via TESTopicInfo::Fire(info, 0/1,
    // speakerHandle, 0). speakerFormID is REQUIRED: Fire's 3rd arg is the
    // speaker's REFR handle, which the receiver builds from this formID.
    // (Field was sourceQuestFormID in the menu-only V4 MVP - always 0 - now
    // repurposed; wire size unchanged.)
    struct DialogVoteData
    {
        uint8_t  playerSlot;       // 0 = host, 1 = client
        uint32_t topicInfoFormID;  // The dialogue line (TESTopicInfo) that fired
        uint32_t speakerFormID;    // The speaking NPC (Actor REFR formID); 0 if unknown
    };
#pragma pack(pop)

    static_assert(sizeof(DialogStartData) == 5, "DialogStartData size");
    static_assert(sizeof(DialogEndData)   == 1, "DialogEndData size");
    static_assert(sizeof(DialogVoteData)  == 9, "DialogVoteData size");

#pragma pack(push, 1)
    // PKT_VEHICLE_POS payload - single FURN-REFR the host currently occupies.
    // Wire format: [uint8 type][VehiclePosData]. Own framing like PKT_NPC_STATE.
    // Client re-encodes baseLocalID via its own plugin load-order using
    // basePlugin (T34 pattern) - then scans loaded cells for a FURN-REFR with
    // the resolved base within a few meters of the host's position.
    // Note on rotation: Host sends all three Euler angles so vehicles pitch/roll
    // with terrain. Applied to FURN+0x98/0x9C/0xA0 (data.angle.x/y/z) via raw
    // write on client after SetPosition.
    struct VehiclePosData
    {
        uint32_t refFormID;              // Host runtime FURN-REFR FormID (0xFFxxxxxx, not resolvable locally)
        uint32_t baseFormID;              // TESFurniture base FormID (HOST-LOCAL - unreliable cross-machine for ESL/FE plugins)
        uint32_t baseLocalID;             // Plugin-local portion of baseFormID (flag-aware mask - same across load orders)
        char     basePlugin[NPC_SOURCE_PLUGIN_MAX]; // Plugin filename owning the FURN base (e.g. "ShatteredSpace.esm")
        uint32_t cellFormID;              // Host's parentCell - informational
        float    posX, posY, posZ;
        float    rotX, rotY, rotZ;        // Full Euler rotation so terrain roll/pitch sync
        // Havok-follow: host's vehicle data3D root world.rotate (NiMatrix3,
        // row-major [m00,m01,m02, m10,m11,m12, m20,m21,m22]). Written directly
        // into the client body's hkTransform rotation basis - exact orientation
        // (incl. pitch/roll), no Euler-convention guessing. All-zero = not sent.
        float    rot3x3[9];
    };
#pragma pack(pop)

    // 4 + 4 + 4 + NPC_SOURCE_PLUGIN_MAX(64) + 4 + 12 + 12 + 36 = 140
    static_assert(sizeof(VehiclePosData) == 140, "VehiclePosData size");

#pragma pack(push, 1)
    // PKT_SHIP_POS payload - spaceship REFR position + Euler rotation, broadcast
    // by whoever's piloting (could be host OR client). Both engines have the
    // SAME static ship FormID (0x0003F7E8 vanilla, save-shared) so receiver
    // looks up via FormID directly - no plugin re-encode needed.
    //
    // Receiver writes to local ship REFR's data.location (raw +0x8C) and
    // data.angle (raw +0x98) - engine moves cockpit cell with ship.
    //
    // Phase L.3 V1.
    struct ShipPosData
    {
        uint32_t shipFormID;       // ship REFR FormID (typically 0x0003F7E8 for player ship)
        uint32_t spaceCellFormID;  // ship's parentCell (informational, validation)
        float    posX, posY, posZ; // position within space cell
        float    rotX, rotY, rotZ; // Euler rotation (radians)
    };
#pragma pack(pop)
    static_assert(sizeof(ShipPosData) == 32, "ShipPosData size");

#pragma pack(push, 1)
    // PKT_FLIGHT_STATE payload - Phase L.3 V2.
    //
    // Pilot ship REFR is a static anchor at space-cell origin (REFR.data.location/.angle
    // never change during flight - confirmed by L.3 V1/V2 V0 probes). The actual flight
    // state lives in `ShipHudDataModel::StickDataProvider` - an array of 24 TUIValue<T>
    // properties at offset +0x1BB0 of ShipHudDataModel. The engine fires
    // `Spaceship::PlayerMovementOutputEvent` per pilot-frame; ShipHudDataModel's sink
    // updates these properties; the HUD reads from them.
    //
    // We tap the same data path: read on host, broadcast, write on client. Each
    // TUIValue<T> entry is 32 bytes with value at +0x18 (u32 for float, u8 for bool).
    // Field offsets within StickDataProvider (from FUN_141544730 RE):
    //   +0x000 boost / +0x020 throttle / +0x040 adjustedThrottle / +0x060 speed
    //   +0x080 fThrusterModeInputX (yaw)  / +0x0A0 fThrusterModeInputY (pitch)
    //   +0x0C0 fReticleX            / +0x0E0 fReticleY
    //   +0x100 dirCrosshairX        / +0x120 dirCrosshairY
    //   +0x140 aimAssistX           / +0x160 aimAssistY
    //   +0x180 boostActive (bool)   / +0x1A0 throttleMode (int)
    //   +0x1C0 bReverse (bool)      / +0x1E0 bShowAimAssist (bool)
    //   +0x200 bTargetFriendly (bool)/+0x220 fAdjustedRoll
    //   +0x240 fTurnRateSweetSpot   / +0x260 fTurnRateSweetSpotRange
    //   +0x280 bStickSwap (bool)    / +0x2A0 bShowCruiseButton (bool)
    //   +0x2C0 uiCruiseLevel (int)  / +0x2E0 fMaxCruiseSpeed
    //
    // V1 packs only the gameplay-relevant subset that the diagnostic dump
    // confirmed is actually written by the engine during piloting. Entries
    // [4]/[5] (fThrusterModeInputX/Y) and [6]/[7] (fReticleTransformInputX/Y)
    // stay 0 in the StickDataProvider during normal flight - they only fire
    // in lateral-strafe / reticle-pre-stage paths. The values that actually
    // carry the steering signal are entries [8]/[9] (shipDirectionCrosshair).
    //
    // V1 fields (from diag dump 2026-04-29):
    //   [0]  boost              float  - boost meter / fuel level 0..1
    //   [1]  throttle           float  - main throttle 0..1
    //   [2]  adjustedThrottle   float  - post-engine throttle (cruise blend)
    //   [3]  speed              float  - current velocity magnitude
    //   [8]  dirCrosshairX      float  - yaw indicator (crosshair X)
    //   [9]  dirCrosshairY      float  - pitch indicator (crosshair Y)
    //   [12] boostActive        bool   - boost button held this frame
    //   [14] bReverse           bool   - reverse-thrust mode
    //   [22] uiCruiseLevel      int→u8 - Reisemodus stage 0..N
    //   [23] maxCruiseSpeed     float  - configured cruise cap (info)
    struct FlightStateData
    {
        float    throttle;             // entry [1]
        float    adjustedThrottle;     // entry [2]
        float    speed;                // entry [3]
        float    boost;                // entry [0] - float, fuel meter
        float    dirCrosshairX;        // entry [8] - yaw output
        float    dirCrosshairY;        // entry [9] - pitch output
        float    maxCruiseSpeed;       // entry [23]
        uint8_t  boostActive;          // entry [12]
        uint8_t  bReverse;             // entry [14]
        uint8_t  uiCruiseLevel;        // entry [22]
        uint8_t  _pad;                 // align to 4
    };
#pragma pack(pop)
    // 7*4 + 4*1 = 28 + 4 = 32
    static_assert(sizeof(FlightStateData) == 32, "FlightStateData size");

#pragma pack(push, 1)
    // PKT_FLIGHT_INPUT payload - Phase L.3 V2.
    //
    // Engine flight-input pipeline:
    //   PlayerControls (input source)
    //     → MovementHandlerAgentPlayerFlightControls::vt[7] reads 3 floats
    //       from input source (yaw/pitch/throttle) and writes them to
    //       its own +0x40 vec3 buffer (FUN_14273d5d0)
    //     → MovementAgentFlightController consumes +0x40 → ship state
    //     → ship physics → havok body → visual transform
    //
    // We post-hook vt[7] (FUN_14068f440):
    //   Active pilot side: read +0x40 after engine writes it, broadcast.
    //   Passive side:      overwrite +0x40 with received values, so engine
    //                      processes host's input as if local. Same +0x40
    //                      → same flight-controller state → same visual.
    //
    // For Alpha: HOST is always the active pilot; CLIENT is always passive.
    // Future: pilot-claim negotiation when both seated (V2.1+).
    struct FlightInputData
    {
        float    axis0;        // local_18 - first axis read (likely yaw)
        float    axis1;        // local_14 - second axis (likely pitch)
        float    axis2;        // local_10 - third axis (likely throttle)
        uint8_t  pilotByte;    // handler+0xA4 - 1-byte input flag (mode/dirty?)
        uint8_t  hasInput;     // 1 if engine wrote a fresh input this frame
        uint8_t  _pad[2];
    };
#pragma pack(pop)
    static_assert(sizeof(FlightInputData) == 16, "FlightInputData size");

#pragma pack(push, 1)
    // PKT_PMO_PAYLOAD payload - Phase L.3 V2.2.
    //
    // Raw bytes of Spaceship::PlayerMovementOutputEvent. We hook the
    // BSTEventSource::Notify helper (FUN_1422CB8F0) on the PMO singleton,
    // so we get access to the exact payload the engine builds per frame.
    // Captured on host, broadcast, applied on client by overwriting the
    // dispatcher's payload pointer target before the original Notify runs.
    //
    // 64 bytes spans the full payload (decomp shows ~0x54 = 84 bytes worst
    // case; we use 64 for V2.2 - refine if needed).
    struct PMOPayloadData
    {
        uint8_t bytes[96];
    };
#pragma pack(pop)
    static_assert(sizeof(PMOPayloadData) == 96, "PMOPayloadData size");

#pragma pack(push, 1)
    // PKT_SHIP_TRANSFORM payload - Phase L.3 V3.
    //
    // The final render-layer ship transform. Host reads its NiAVObject world
    // transform each frame at ship.loadedData->data3D (NiAVObject):
    //   rotate    @ +0x80 (3x3 matrix, row-major, 9 floats)
    //   translate @ +0xB0 (3 floats)
    // Client writes the same bytes to its local ship's NiNode pre-render so
    // the engine renders host's exact world position/orientation independent
    // of client-side physics. Bethesda design: child cells (ship interior)
    // rigid-follow this NiNode automatically.
    struct ShipTransformData
    {
        float    rotate[9];        // row-major 3x3 (NiAVObject.world.rotate, diagnostic only)
        float    translate[3];     // world position (diagnostic only)
        float    havokState[16];   // V3 iter8 - 64 bytes from havokBody[+0x4D0]+0x50..+0x8F
                                   // (the actual rotation/transform that vt[+0x258] returns and
                                   // FUN_1423bae50 writes - RE'd from bhkCharacterStateSpaceship vt[8])
        uint32_t shipFormID;       // L.3 V5 - host's ship REFR FormID. Lets the
                                   // receiver locate its local same-FormID ship
                                   // via TESForm::LookupByID when player->GetSpaceship
                                   // returns null (typical after WorldSync save load).
    };
#pragma pack(pop)
    static_assert(sizeof(ShipTransformData) == 116, "ShipTransformData size");

#pragma pack(push, 1)
    // PKT_FAST_TRAVEL_SYNC payload - Phase L.7 V7.
    //
    // Sent host → client a few seconds after PlayerFastTravel::Event fires on
    // host, so the receiving side can teleport its local same-FormID ship to
    // the destination cell. The cell-streamer side-effect of engine MoveTo
    // (RVA 0x14191EFB0) is what makes star-system events/planets/NPCs render
    // correctly on the client.
    struct FastTravelSyncData
    {
        uint32_t shipFormID;       // sender's piloted ship REFR FormID
        uint32_t destCellFormID;   // ship's parentCell.formID after jump (space cell)
        uint32_t destWSFormID;     // dest cell's cellWorldspace.formID
        float    destPos[3];       // ship REFR position in destination
        uint64_t timestampMs;      // host tick at send time, for staleness checks
    };
#pragma pack(pop)
    static_assert(sizeof(FastTravelSyncData) == 32, "FastTravelSyncData size");

#pragma pack(push, 1)
    // PKT_SHIP_FIRE payload - Phase L.4 V1 (visual fire-event sync).
    //
    // Detected via BSTEventSource<WeaponFiredEvent> vtable (REL::ID 412442)
    // match in HookedNotifyDispatch. Captured payload qw[1] = weapon mount
    // pointer (4 distinct values per ship's 4 weapons), qw[8] = per-shot
    // pointer. We dereference qw[1] to get TESObjectWEAP.formID @ +0x28.
    //
    // V1 = minimal wire validation (client logs receipt only). V1.1 adds
    // visual VFX + sound. V2 (with NPCSync-Space) reverses Projectile struct
    // for actual projectile spawn. ShipFireData kept stable across V1..V2.
    struct ShipFireData
    {
        uint32_t shooterShipFormId;  // host's ship REFR (typically 0x0003F7E8)
        uint32_t weaponFormId;       // TESObjectWEAP form, dereffed from qw[1]
        uint32_t baseProjFormId;     // BGSProjectile form, from projectile REFR base
        uint32_t pad0;
        uint64_t timestampMs;        // for jitter compensation
        // slice 3 - real shot geometry, read by the host from the live
        // projectile (ProjBeginUpdate). Lets the client spawn the SAME aimed
        // bolt (host already aimed at the target) instead of guessing a dir.
        float    origin[3];          // projectile spawn pos (REFR data.location +0x8C)
        float    dir[3];             // movementDirection (+0x158), normalized
        float    power;              // +0x214
        float    range;              // +0x21C
        float    speedMult;          // +0x218
        float    vel[3];             // velocity (+0x164) - actual vector dir*speed
    };
#pragma pack(pop)
    static_assert(sizeof(ShipFireData) == 72, "ShipFireData size");

#pragma pack(push, 1)
    // PKT_SHIP_AV payload - ship-combat sync, slice 2 (hull only).
    // Host broadcasts the shared ship's hull-health AV; client mirrors it onto
    // its local same-FormID ship so both games agree on the ship's condition.
    struct ShipAvData
    {
        uint32_t shipFormId;  // host ship REFR (typically 0x0003F7E8 Frontier)
        float    hullCur;     // GetActorValue(hullAV)         - current hull
        float    hullMax;     // GetPermanentActorValue(hullAV) - effective max
    };
#pragma pack(pop)
    static_assert(sizeof(ShipAvData) == 12, "ShipAvData size");

#pragma pack(push, 1)
    // PKT_TIME_WEATHER payload  - host's game time + current weather, sent once
    // on client join. Client applies time (Calendar globals) + force-activates the
    // weather form so both machines see the same time-of-day and weather effects.
    struct TimeWeatherData
    {
        float    gameDaysPassed;   // Calendar master clock (encodes day + fractional hour)
        float    gameHour;         // current hour (set alongside daysPassed)
        uint32_t weatherBase;      // host-local current weather FormID (fallback)
        uint32_t weatherLocalID;   // plugin-local portion (flag-aware mask)
        char     weatherPlugin[NPC_SOURCE_PLUGIN_MAX]; // owning plugin filename
        uint8_t  hasWeather;       // 1 = a current weather form was read; 0 = skip weather apply
        uint8_t  pad[3];
    };
#pragma pack(pop)
    static_assert(sizeof(TimeWeatherData) == 4 + 4 + 4 + 4 + NPC_SOURCE_PLUGIN_MAX + 4, "TimeWeatherData size");

#pragma pack(push, 1)
    // PKT_NPC_SHIP_BATCH - Phase M.1 V2 NPC ship transform sync.
    //
    // Host captures non-player bhkSpaceshipState bodies via vt[8] (one of
    // the 9-16 unique ones seen in encounters), caches their 64-byte havok
    // state, and broadcasts a batch periodically. Client position-matches
    // remote entries to local bodies and applies the state.
    //
    // handleHash is just the low 32 bits of host's param_2 pointer - opaque
    // to the client, used only to track which entry in the batch is which
    // ship across consecutive packets (for delta detection / interpolation).
    struct NPCShipBatchEntry
    {
        uint32_t handleHash;       // low 32b of host's param_2 ptr - per-host stable
        uint32_t pad0;
        float    havokState[16];   // same 64B layout as ShipTransformData.havokState
    };

    constexpr uint8_t MAX_NPC_SHIPS_PER_BATCH = 12;  // 12 * 72 = 864 bytes payload

    struct NPCShipBatch
    {
        uint8_t           count;
        uint8_t           pad[7];
        NPCShipBatchEntry entries[MAX_NPC_SHIPS_PER_BATCH];
    };
#pragma pack(pop)
    static_assert(sizeof(NPCShipBatchEntry) == 72, "NPCShipBatchEntry size");
    static_assert(sizeof(NPCShipBatch) == 8 + 12 * 72, "NPCShipBatch size");

#pragma pack(push, 1)
    // PKT_CRUISE_STATE - Phase L.5 cruise mode toggle.
    // Host detects via 60Hz poll of byte @ RVA 0x5EAA5AE (the same byte that
    // IsCruiseModeActive reads - confirmed via static binary analysis).
    // Client applies by calling the cruise-toggle function @ RVA 0x13FDAC0 with
    // the bool - that function handles all engine state setup (HUD, speed cap,
    // cell streaming) including the eventual cell reload on cruise EXIT.
    struct CruiseStateData
    {
        uint8_t  enable;          // 1 = enter cruise, 0 = exit cruise
        uint8_t  pad[7];
        uint32_t shipFormId;      // pilot's ship FormID for diagnostic / cross-check
        uint32_t pad2;
        uint64_t timestampMs;     // for ordering / jitter visibility
        // Phase 3v3 - cruise world position (3 doubles at DAT_145F31DA0/DA8/DB0).
        // These are the engine's double-precision world coords for the ship
        // (set at cruise ENTER from FUN_14090ECF0(actor), used by per-tick math
        // + arrival cleanup). Client overrides them BEFORE applying the EXIT
        // call so its post-cruise final position matches host's. On ENTER
        // packets we send 0s (client computes its own at force-ENTER); on
        // EXIT packets we send host's actual values. shipPos[3] = 0 (4th qword
        // is always zero per RE).
        double   shipPosX;        // DAT_145F31DA0 - world X (game units, ~10^10 scale)
        double   shipPosY;        // DAT_145F31DA8 - world Y
        double   shipPosZ;        // DAT_145F31DB0 - world Z
        // Phase 4 Step 4 - cruise destination formID at DAT_145EB8284 (low 32b).
        // Set by FUN_1413FDFD0 (POI resolver) inside ENTER on host. Client's
        // force=true ENTER skips that resolver, so client's field stays 0 and
        // the per-tick arrival check (FUN_1413FE900) idles. Sent at ENTER and
        // written by client BEFORE calling FUN_1413FDC90(true) so the per-tick
        // math has a target. EXIT carries 0 (host's field already cleared).
        uint32_t destFormId;      // DAT_145EB8284 (low 32b) - TESForm formID of POI target
        // Phase 4 Step 6 - "arrival initiated" trigger. Host polls DAT_145EB2856.
        // On 0->1 transition (per-tick math fired FUN_1413FA140 = "Cruise Mode
        // Arrival Initiated"), sets this byte. Client calls FUN_1413FA140
        // directly - sets all 3 dur-progression gate bits (DAT_145EB2856,
        // DAT_145EB29EE, DAT_14590E27F), schedules the cell-stream async task
        // (FUN_140B6EB20), fires UI animation. Drives client's `dur` countdown
        // + POI/cell content load to match host's.
        uint8_t  arrivalInit;     // 1 = "fire arrival initiation now" (in addition to enable transition)
        uint8_t  uiSelector;      // Phase 4 Step 7 - DAT_145EB2857 (0=POI arrival path, 1=end-cruise path)
        uint8_t  pad3[2];
        // Phase 4 Step 7 - DAT_145EB8274 (current state machine value). FUN_1413FA140
        // copies this into DAT_145EB84D4 as arrival comparator. Client's stale 0
        // value made FUN_1413FA6C0(0) call return without picking a POI → no UI
        // string ("Jemison" name) shown. Synced on arrivalInit packets.
        uint32_t arrivalState;    // DAT_145EB8274 value at host's arrival init
        uint32_t pad4;
    };
#pragma pack(pop)
    static_assert(sizeof(CruiseStateData) == 64, "CruiseStateData size");

#pragma pack(push, 1)
    // PKT_CRUISE_ORIENT - Phase L.5 Phase 4 Step 11. Continuous (5Hz) sync of
    // the 3 cruise-orient doubles at DAT_145F31DA0/DA8/DB0. Memory-diff (Step
    // 10) showed these evolve on host throughout cruise but stagnate on client
    // between arrivalInit/EXIT events. Likely drives planet/celestial-body
    // rendering. Sent unreliable - missing one update is fine, the next 200ms
    // tick will catch the client up.
    struct CruiseOrientData
    {
        double   posX;          // DAT_145F31DA0 - world X (game units, ~10^10 scale)
        double   posY;          // DAT_145F31DA8 - world Y
        double   posZ;          // DAT_145F31DB0 - world Z
        uint64_t timestampMs;
    };
#pragma pack(pop)
    static_assert(sizeof(CruiseOrientData) == 32, "CruiseOrientData size");

#pragma pack(push, 1)
    // PKT_LOOT_PICKUP payload - a single loot item was just activated on host.
    // Client matches a local REFR by (resolved-base, position) - same pattern as
    // PKT_VEHICLE_POS - and replays Activate locally so the item vanishes from
    // the world. Inventory ownership is intentionally F.2 scope; this packet
    // only makes the world-state converge.
    struct LootPickupData
    {
        uint32_t refFormID;     // Host runtime REFR FormID (0xFFxxxxxx - informational only)
        uint32_t baseFormID;    // Base object FormID on host (host-local)
        uint32_t baseLocalID;   // Plugin-local portion of base (flag-aware mask)
        char     basePlugin[NPC_SOURCE_PLUGIN_MAX];  // Owning plugin filename
        uint8_t  baseFormType;  // RE::FormType value - matcher sanity check + filter
        uint8_t  _pad[3];
        float    posX, posY, posZ;
    };
#pragma pack(pop)
    // 4 + 4 + 4 + 64 + 1 + 3 + 12 = 92
    static_assert(sizeof(LootPickupData) == 92, "LootPickupData size");

#pragma pack(push, 1)
    // PKT_CONTAINER_TAKE payload - a single take-from-container on the
    // sending side. Observed in F.1.c tracing: each E-press in Starfield's
    // hover-container-UI fires a distinct Activate on the container REFR,
    // and the inventory transfer completes within that same frame. The
    // hook snapshots the container's inventoryList pre-orig, calls orig,
    // then diffs post-orig to identify exactly which base-form and how
    // many units left the container.
    //
    // Matcher on the receiving side: locate the local container by
    // (containerBase-resolved, position) within a small radius, then find
    // an entry with matching itemBase-resolved and decrement count by
    // |count|. Handles proc-gen divergence gracefully - if a peer's
    // container doesn't contain the item (different random roll), the
    // cleanup logs a warn and skips.
    struct ContainerTakeData
    {
        // Container identity - for the local-REFR matcher (same pattern as
        // PKT_LOOT_PICKUP / VehiclePosData).
        uint32_t containerRefID;        // Sender runtime REFR FormID (informational)
        uint32_t containerBase;         // Host-local container base FormID
        uint32_t containerLocalID;      // Plugin-local portion (flag-aware mask)
        char     containerPlugin[NPC_SOURCE_PLUGIN_MAX]; // Plugin owning container base
        float    posX, posY, posZ;

        // Item identity + amount transferred.
        uint32_t itemBase;              // Host-local item base FormID
        uint32_t itemLocalID;           // Plugin-local portion
        char     itemPlugin[NPC_SOURCE_PLUGIN_MAX];
        int32_t  count;                 // Signed (always positive here; negative would mean deposit, unhandled)

        // F.1.e: REFR's own formType (kREFR=0x4A for containers, kACHR=0x4B
        // for dead NPCs). Receiver branches on this - NPC bases are dynamic
        // (FF-prefix) and can't be matched by FormID across machines, so the
        // ACHR path uses position-only matching.
        uint8_t  containerRefType;
        // M.2 V5 (2026-05-08): forward the engine's ITEM_TRANSFER_REASON byte
        // from the host's AddObjInner hook so the receiver replays with the
        // matching reason. Helps the engine route ship-cargo / barter
        // transfers through their specialised paths instead of the generic
        // kScriptAddItem fallback. Defaults to 4 (kStoreInContainer) for
        // BGSInventoryList-path broadcasts that don't have the original.
        uint8_t  transferReason;
        // M.2 V7 (2026-05-08): variable-length mod array trailer. modCount
        // entries of LootDropMod follow this struct in the packet payload
        // (same encoding as PKT_LOOT_DROP). Used so weapons/armor with
        // attached mods replay with the correct mod loadout on the
        // receiver instead of being spawned as bare base items.
        uint8_t  modCount;
        // M.2 V16 (2026-05-08): kObjectInstance +0x28 sentinel - engine
        // uses this for item tier rendering (0xFFFF=default, else primary
        // mod slot index for legendary). Without syncing, client's proc-
        // gen roll leaves a non-0xFFFF sentinel and weapon renders as
        // wrong tier even after V15 modCount truncation.
        uint16_t modSentinel;
    };
#pragma pack(pop)
    // 4 + 4 + 4 + 64 + 12 + 4 + 4 + 64 + 4 + 1 + 1 + 1 + 2 = 169
    static_assert(sizeof(ContainerTakeData) == 169, "ContainerTakeData size");

#pragma pack(push, 1)
    // PKT_LOOT_DROP payload - a player-inventory drop detected via the
    // TESObjectREFR::DropObject-core hook (F.1.d). The sender's filter is
    // self->formID == 0x14 (the player FormID), which perfectly separates
    // menu drops from cell-stream / proc-gen REFR placement and from
    // container-inventory movements (those fire with self=containerRef).
    //
    // The receiver spawns the REFR by replaying the drop on the remote
    // clone actor: add the item to the clone's inventory, then call the
    // same DropObject-core with self=clone. Engine physics drops the REFR
    // at the clone's feet - which matches "at the peer player's feet" on
    // our side. No position is sent; the clone is already kept in sync by
    // PKT_POSITION, so the spawn point is correct by construction.
    struct LootDropData
    {
        uint32_t itemBase;      // Sender-local item base FormID
        uint32_t itemLocalID;   // Plugin-local portion (flag-aware mask)
        char     itemPlugin[NPC_SOURCE_PLUGIN_MAX];
        uint8_t  itemFormType;  // RE::FormType - sanity filter on receiver
        uint8_t  _pad[3];
        int32_t  count;         // Stack size (>=1)

        // F.2-Mods (2026-04-25): mod array follows as variable-length
        // trailer of `modCount * sizeof(LootDropMod)` bytes after this
        // struct. modCount is capped at 32 (kMaxModsPerDrop).
        uint8_t  modCount;
        uint8_t  _pad2[1];
        // M.2 V16 (2026-05-08): kObjectInstance +0x28 sentinel - engine
        // uses this for item tier rendering. Without syncing, gray world
        // drops still render as legendary on receiver.
        uint16_t modSentinel;
    };

    // F.2-Mods: per-mod descriptor - sent as a trailing array after
    // LootDropData. Each entry carries the plugin-local FormID portion
    // + plugin name, so receiver re-encodes via PluginLookup just like
    // the item base.
    struct LootDropMod
    {
        uint32_t base;     // Sender-local mod base FormID
        uint32_t localID;  // Plugin-local portion
        char     plugin[NPC_SOURCE_PLUGIN_MAX];
    };
#pragma pack(pop)
    // 4 + 4 + 64 + 1 + 3 + 4 + 1 + 3 = 84
    static_assert(sizeof(LootDropData) == 84, "LootDropData size");
    // 4 + 4 + 64 = 72
    static_assert(sizeof(LootDropMod) == 72, "LootDropMod size");

#pragma pack(push, 1)
    // PKT_DOOR_STATE payload - F.3 (2026-04-25). Bidirectional. The
    // sender's Activate-hook fires when a player presses E on a TESObjectDOOR
    // REFR; we broadcast post-orig so the door animation has already started
    // locally. The receiver finds the matching door REFR in cell-scan via
    // (resolved-base, position) and replays orig-Activate with the remote
    // clone as activator - engine handles the open/closed toggle natively
    // and triggers the same animation on the peer's side.
    //
    // No state byte is transmitted in V1. The Activate path in the engine
    // is naturally toggling: closed→opening→open→closing→closed. As long as
    // both sides start in sync (loaded from the same host save), each
    // activation event keeps them in lock-step. `state` is reserved for V2
    // (late-join initial-state sync, lock-state, etc.) and currently unused
    // by the receiver.
    struct DoorStateData
    {
        uint32_t refFormID;     // Sender runtime REFR FormID (informational)
        uint32_t baseFormID;    // Door base FormID (sender-local)
        uint32_t baseLocalID;   // Plugin-local portion (flag-aware mask)
        char     basePlugin[NPC_SOURCE_PLUGIN_MAX]; // Plugin owning door base
        float    posX, posY, posZ;
        uint8_t  state;         // V2-reserved: 0=closed, 1=opening, 2=open, 3=closing
        uint8_t  pad[3];
    };
#pragma pack(pop)
    // 4 + 4 + 4 + 64 + 12 + 1 + 3 = 92
    static_assert(sizeof(DoorStateData) == 92, "DoorStateData size");

#pragma pack(push, 1)
    // PKT_LOCK_STATE payload - F.3.b (2026-04-25). Bidirectional. The
    // sender's Activate-hook detects when REFR_LOCK->flags transitions
    // (typically locked YES→no after a successful lockpick). Engine
    // auto-fires Activate again once the lockpick mini-game completes,
    // so the post-orig probe in the Activate hook catches the unlocked
    // state in the same call path.
    //
    // Receiver finds the local REFR via (resolved-base, position) and
    // writes REFR_LOCK->flags directly + resets numTries. NO Activate
    // replay - for kCONT that would pop the loot UI on the peer; for
    // kDOOR the existing F.3 Door-Sync handles open/close animation
    // separately. Pure state-flag mirror.
    struct LockStateData
    {
        uint32_t refFormID;     // Sender runtime REFR FormID (informational)
        uint32_t baseFormID;    // Sender-local base FormID
        uint32_t baseLocalID;   // Plugin-local portion (flag-aware mask)
        char     basePlugin[NPC_SOURCE_PLUGIN_MAX]; // Plugin owning base
        float    posX, posY, posZ;
        uint8_t  baseFormType;  // RE::FormType - kCONT(0x1F) or kDOOR(0x25); receiver matcher filter
        uint8_t  locked;        // 0 = unlock, 1 = relock
        uint8_t  pad[2];
    };
#pragma pack(pop)
    // 4 + 4 + 4 + 64 + 12 + 1 + 1 + 2 = 92
    static_assert(sizeof(LockStateData) == 92, "LockStateData size");

#pragma pack(push, 1)
    // PKT_CONTAINER_OPEN payload - F.3.c (2026-04-25). Bidirectional.
    // Sender's Activate-hook fires this when a player E-press on a kCONT
    // REFR succeeds (orig-Activate returned true) AND the container is
    // not currently locked. Receiver finds the local match via
    // (resolved-base, position) and calls Papyrus SetOpen(true) - the
    // engine plays the lid-open animation natively (SetOpen internally
    // calls Activate) and the open-state persists without the auto-close
    // a clone-Activate-replay would otherwise trigger.
    //
    // Locked containers are intentionally excluded - F.3.b LockSync owns
    // the post-pick auto-open path and would double-broadcast otherwise.
    struct ContainerOpenData
    {
        uint32_t refFormID;     // Sender runtime REFR FormID (informational)
        uint32_t baseFormID;    // Sender-local base FormID
        uint32_t baseLocalID;   // Plugin-local portion (flag-aware mask)
        char     basePlugin[NPC_SOURCE_PLUGIN_MAX]; // Plugin owning base
        float    posX, posY, posZ;
        uint8_t  pad[4];
    };
#pragma pack(pop)
    // 4 + 4 + 4 + 64 + 12 + 4 = 92
    static_assert(sizeof(ContainerOpenData) == 92, "ContainerOpenData size");

#pragma pack(push, 1)
    // PKT_GRAB_STATE payload - F.6 V2b (2026-04-25). Bidirectional.
    // Sender's TESGrabReleaseEvent sink fires this when E-Hold starts
    // (grabbed=1) and ends (grabbed=0). While grabbed=1 the sender's
    // game-thread tick re-broadcasts at ~30 Hz with the held REFR's
    // current position so the peer mirrors the float-in-front-of-player
    // motion. On grabbed=0 the peer stops mirroring; engine physics
    // resume on both sides.
    //
    // Receiver: same base+position-radius matcher as Door/Lock - the
    // initial grabbed=1 packet identifies the REFR; subsequent packets
    // for the same item arrive with updated posX/Y/Z and we apply via
    // SafeRefSetPosition (vt[0x89]) without re-running the matcher.
    struct GrabStateData
    {
        uint32_t refFormID;     // Sender runtime REFR FormID (0xFFxxxxxx informational)
        uint32_t baseFormID;    // Sender-local base FormID
        uint32_t baseLocalID;   // Plugin-local portion (flag-aware mask)
        char     basePlugin[NPC_SOURCE_PLUGIN_MAX]; // Plugin owning base
        float    posX, posY, posZ;
        uint8_t  baseFormType;  // RE::FormType - sanity filter on receiver
        uint8_t  grabbed;       // 1 = grab start or position update, 0 = release
        uint8_t  pad[2];
    };
#pragma pack(pop)
    // 4 + 4 + 4 + 64 + 12 + 1 + 1 + 2 = 92
    static_assert(sizeof(GrabStateData) == 92, "GrabStateData size");

#pragma pack(push, 1)
    // PKT_ACTI_STATE payload - F.7 V1 (2026-04-26). Bidirectional. Sender's
    // Activate-hook fires this when a player E-press succeeds on a kACTI
    // (Activator) REFR - switches, buttons, levers, power-conduits, etc.
    // Receiver finds the local match via (resolved-base, position) and
    // replays Activate with the remote clone as activator - the activator's
    // Papyrus script runs locally and triggers its visual + state.
    //
    // Caveat: activators that consume inventory items (e.g. power conduit
    // wanting an Electromagnet/Power-Cell) will see their script bail on
    // the receiver because client lacks the item. Switches / buttons /
    // door-buttons that don't read inventory should sync cleanly.
    struct ActiStateData
    {
        uint32_t refFormID;     // Sender runtime REFR FormID (informational)
        uint32_t baseFormID;    // Activator base FormID (sender-local)
        uint32_t baseLocalID;   // Plugin-local portion (flag-aware mask)
        char     basePlugin[NPC_SOURCE_PLUGIN_MAX]; // Plugin owning base
        float    posX, posY, posZ;
        // F.7 V2 (2026-04-26): consumed-item info - for activators whose
        // Papyrus script removes an inventory item on activation (e.g.
        // power-conduit consumes a power-cell). Sender pre/post snapshots
        // player inventory across the Activate call; if an item was
        // removed, includes its base + plugin so receiver can ghost-add
        // it before the replay → script consumes ghost naturally.
        // consumedFormID == 0 means no item was consumed (switch, button).
        uint32_t consumedFormID;
        uint32_t consumedLocalID;
        int32_t  consumedCount;
        char     consumedPlugin[NPC_SOURCE_PLUGIN_MAX];
    };
#pragma pack(pop)
    // 4 + 4 + 4 + 64 + 12 + 4 + 4 + 4 + 64 = 164
    static_assert(sizeof(ActiStateData) == 164, "ActiStateData size");

#pragma pack(push, 1)
    // PKT_FORM_BLOB header - followed by `blobSize` raw bytes of the engine
    // serialized form. Host serializes via Forms::SerializeForm; client
    // deserializes onto matching local target via Forms::DeserializeForm.
    // Used for cell-transition biped re-init (see memory/reference_cell_transition_re.md).
    struct FormBlobHeader
    {
        uint32_t sourceFormID;  // Sender-local form FormID (informational)
        uint32_t targetFormID;  // Receiver-local target FormID. 0 = "self"
                                // (player REFR), non-zero = specific clone.
        uint32_t blobSize;      // bytes of payload after this header
        uint8_t  formType;      // RE::FormType - receiver-side sanity check
        uint8_t  pad[3];
    };
#pragma pack(pop)
    static_assert(sizeof(FormBlobHeader) == 16, "FormBlobHeader size");
    // Max raw blob size - empirically player REFR Actor blob is ~15 KB at
    // game start, can grow with inventory/quest data. Cap at 64 KB so we
    // fit in a single ENet reliable message.
    static constexpr size_t FORM_BLOB_MAX_SIZE = 64 * 1024;

#pragma pack(push, 1)
    // PKT_TRANSIT - Phase K.1 (2026-04-27). Bidirectional. Sender has just
    // crossed a cell boundary. Receiver of K.1 logs only; K.2 will trigger
    // auto-follow / fast-travel / ship-landing on the receiver side.
    //
    // Note on FormIDs: in Bethesda engines, runtime / proc-spawned cells get
    // FF-prefix FormIDs that are RE-numbered per machine (host vs client see
    // different IDs for the same logical cell). For statics (Lodge, Cydonia,
    // named ship interiors, planet markers), FormIDs are plugin-stable and
    // identical across machines. The receiver uses isInterior + transitType
    // to decide how to interpret the destCellFormID.
    struct TransitData
    {
        // Best-guess classification at sender:
        //   0 = kInteriorEntry (just walked through a door INTO interior)
        //   1 = kExteriorReturn (just exited interior back to exterior)
        //   2 = kFastTravel    (StarMap or marker fast-travel)
        //   3 = kShipLanding   (ship landed on planet - worldspace change)
        //   4 = kGravJump      (interstellar jump - worldspace change)
        //   5 = kUnknown       (cell change we couldn't classify)
        uint8_t  transitType;
        uint8_t  isInterior;          // dest cell IsInterior() - 1=true
        uint8_t  pad[2];

        uint32_t sourceCellFormID;    // sender's previous parentCell, 0 if unknown
        uint32_t destCellFormID;      // sender's new parentCell
        uint32_t worldspaceFormID;    // sender's new worldspace, 0 if interior

        float    destX, destY, destZ; // sender's player.data.location after transit
        float    destRotZ;            // facing

        // Phase K.2 V2 (2026-04-27): if the transit was triggered by the
        // sender activating a door (= 99% of interior entry / fast-travel
        // by ref), this is the door's FormID. Receiver-side auto-follow
        // can then Papyrus-Activate the same door - engine handles the
        // proper cell-load + teleport, which SetPosition alone cannot do
        // for interior cells. 0 = not a door-driven transit (free-roam
        // exterior fast-travel via map etc.).
        std::uint32_t sourceDoorFormID;
    };
#pragma pack(pop)
    static_assert(sizeof(TransitData) == 36, "TransitData size");

#pragma pack(push, 1)
    // PKT_SPACE_TRANSITION - Phase K.7 (2026-04-28). Ship lifecycle event from
    // SFSE-documented Spaceship event sources (GameEvents.h). Bidirectional.
    // Fires from BSTEventSink<Spaceship::TakeOffEvent> et al. on the host's
    // game thread when the engine raises the event natively.
    //
    // Receiver semantics in V1: log only. V2 will auto-trigger client takeoff
    // by calling Game.GetPlayerShip() + Player.MoveTo(ship) - engine handles
    // worldspace switch (planet → space) which our cellWorldspace-based V6
    // path can't detect (cockpit cell.cellWorldspace = nullptr always).
    struct SpaceTransitionData
    {
        uint8_t  kind;            // 0=kTakeOff, 1=kLanding, 2=kGravJump, 3=kDock
        uint8_t  pad[3];
        uint32_t playerCellFormID; // sender's player.parentCell formID at fire time
        float    posX, posY, posZ; // sender's player position at fire time (informational)
    };
#pragma pack(pop)
    static_assert(sizeof(SpaceTransitionData) == 20, "SpaceTransitionData size");

#pragma pack(push, 1)
    // PKT_CELL_CONTENT_BATCH - Phase K.6 V1 (2026-04-28). Host → Client.
    //
    // Wire format: [uint8 type][CellContentBatchHeader][CellRefDescriptor × N]
    // Custom framing (no PacketHeader), matches PKT_NPC_STATE / PKT_SAVE_TRANSFER.
    //
    // Built by CellContentSync after host's cell-change settled (1.5s post
    // loadMenu CLOSED). Filters cell.references to only "interesting" base
    // FormTypes (CONT, DOOR, ACTI, FURN, REFR, ACHR) - skips static deko
    // (kSTAT, kPDCL, kMSTT etc.). V1 receiver logs only; V2 spawns refs via
    // F.1.d's PlaceAtMe primitive after T34 plugin resolve.
    //
    // Cap: 512 descriptors per batch. Host's cell-enum that exceeds this
    // emits a warning and truncates - V2 will handle multi-batch splits.
    struct CellContentBatchHeader
    {
        uint32_t cellFormID;        // Host's parentCell formID (informational)
        uint32_t worldspaceFormID;  // 0 if interior cell
        uint8_t  isInterior;        // 1 if interior
        uint8_t  isProc;            // 1 if FF-prefix (proc-gen) cell
        uint8_t  pad[2];
        uint32_t refCount;          // Number of CellRefDescriptor entries following
    };
    static_assert(sizeof(CellContentBatchHeader) == 16, "CellContentBatchHeader size");

    struct CellRefDescriptor
    {
        uint32_t refFormID;         // Host runtime REFR FormID (informational, often FF-prefix)
        uint32_t baseFormID;        // Base object FormID on host (host-local)
        uint32_t baseLocalID;       // Plugin-local portion (flag-aware mask) - for cross-machine resolve
        char     basePlugin[NPC_SOURCE_PLUGIN_MAX]; // Plugin owning base form (T34)
        uint8_t  baseFormType;      // RE::FormType enum value (CONT/DOOR/ACTI/FURN/REFR/ACHR)
        uint8_t  pad[3];
        float    posX, posY, posZ;
    };
    static_assert(sizeof(CellRefDescriptor) == 92, "CellRefDescriptor size");
#pragma pack(pop)

#pragma pack(push, 1)
    // PKT_DOCK_FINISHED payload - L.6 V3 Phase 2 (2026-05-02). Sent by host when
    // its Spaceship::DockEvent fires with started flag flipping (typically
    // started=1, finished=0 = cutscene-begin moment per earlier RE work RE).
    //
    // Carries: host's player ship FormID + the dock-port REFR FormID (the
    // station-side anchor, vanilla 0x001DE-prefix), ship's world position +
    // orientation at dock-commit, plus a stage byte indicating which DockEvent
    // variant fired so receiver can sequence its replay (0=request,
    // 1=cutscene-begin, 2=fully-docked-if-engine-fires-it).
    //
    // Phase 2 receiver = log only. Phase 3 receiver will resolve FormIDs via
    // T34 + write ship pos/orient on local clone + invoke engine dispatcher
    // (RVA 0x14213A130 / REL::ID 120238) so engine runs ComponentDB
    // registration + cell-transit naturally - no MoveTo shortcut.
    struct DockFinishedData
    {
        uint32_t shipFormID;          // Player ship FormID (e.g. 0x0003F7E8 Frontier)
        uint32_t dockRefrFormID;      // Station dock-port REFR FormID (e.g. 0x001DE8BE)
        float    posX, posY, posZ;    // Ship world position
        float    quatX, quatY, quatZ, quatW;  // Ship orientation quaternion
        uint8_t  stage;               // 0=request(0,0), 1=begin(1,0), 2=finished(1,1)
        uint8_t  pad[3];              // align to 4
    };
    static_assert(sizeof(DockFinishedData) == 4 + 4 + 12 + 16 + 1 + 3, "DockFinishedData size");
    static_assert(sizeof(DockFinishedData) == 40, "DockFinishedData expected 40 bytes");
#pragma pack(pop)

#pragma pack(push, 1)
    // PKT_DOCK_EXTRATELEPORT payload - L.6 V3 Phase 3b (2026-05-02). Mirrors
    // the cockpit-hatch ExtraTeleport inner struct from host to client so
    // client's hatch passes the activate gate at RVA 0xB47920 and triggers
    // the existing TransitSync cell-load flow on E-press.
    //
    // Engine ExtraTeleport inner layout (REFR_TELEPORT, 0x28 bytes):
    //   +0x00  uint64  destDoorRef    (NiPointer; on client we send dest FormID
    //                                  and resolve locally - may be null)
    //   +0x08  uint32  flags
    //   +0x0C  float[3] destPos
    //   +0x18  float[3] destRot
    //   +0x24  uint8   destFlag
    //
    // The hatch identification: cockpit kDOOR with base FormID 0x00009CF4.
    // Per earlier RE work RE: hatch is form-type 0x25 = kDOOR (NOT kFURN=0x2F).
    struct DockExtraTeleportData
    {
        uint32_t hatchBaseFormID;     // = 0x00009CF4 (sanity)
        uint32_t cockpitCellFormID;   // host's cockpit cell FormID (0x05100136)
        uint32_t destDoorFormID;      // station-side hatch REFR FormID (vanilla 0x001DE...)
        uint32_t flags;               // ExtraTeleport.inner.flags
        float    destPosX, destPosY, destPosZ;
        float    destRotX, destRotY, destRotZ;
        uint8_t  destFlag;
        uint8_t  pad[3];
    };
    static_assert(sizeof(DockExtraTeleportData) == 4*4 + 12*2 + 1 + 3, "DockExtraTeleportData layout");
    static_assert(sizeof(DockExtraTeleportData) == 44, "DockExtraTeleportData expected 44 bytes");
#pragma pack(pop)

#pragma pack(push, 1)
    // PKT_DOCK_UNDOCKED payload - L.6 V3 Phase 3e (2026-05-02). Sent by the
    // local pilot when their TakeOffEvent fires (which happens both for
    // ground-takeoff AND for station-undock). Receiver checks if it has
    // applied dock-state and cleans up. Idempotent - if receiver hadn't
    // applied, it no-ops.
    struct DockUndockedData
    {
        uint32_t shipFormID;        // Pilot's ship FormID (e.g. 0x0003F7E8)
        uint32_t hatchBaseFormID;   // = 0x00009CF4 (sanity)
    };
    static_assert(sizeof(DockUndockedData) == 8, "DockUndockedData size");
#pragma pack(pop)

#pragma pack(push, 1)
    // PKT_DOCK_STATION_XT payload - L.6 V3 Phase 3i (2026-05-02). Pilot
    // captures the engine-populated ExtraTeleport inner from its local
    // station-side hatch (right after walking into station cell) and
    // broadcasts. Receivers use these values when applying their own
    // station-side bypass - replaces hardcoded Frontier-specific values
    // with ship-specific ones, generalizing to all ships including custom
    // shipyard builds.
    struct DockStationXTData
    {
        uint32_t stationHatchFormID;  // pilot's local station-side hatch ref FormID (informational)
        uint32_t hatchBaseFormID;     // station-side hatch base FormID (e.g. 0x00169FAB Stroud-Eklund, varies per station type)
        uint32_t flags;               // ExtraTeleport.inner.flags (engine-populated, ship-specific)
        float    destPosX, destPosY, destPosZ;
        float    destRotX, destRotY, destRotZ;
        uint8_t  destFlag;
        uint8_t  pad[3];
    };
    static_assert(sizeof(DockStationXTData) == 4*3 + 12*2 + 1 + 3, "DockStationXTData layout");
    static_assert(sizeof(DockStationXTData) == 40, "DockStationXTData expected 40 bytes");
#pragma pack(pop)

    // Max refs per batch to keep ENet packet under typical reliable size limit.
    // 512 × 92 = 47104 bytes + 17 header = ~46KB. Fits comfortably under
    // ENet's 64KB reliable cap. Heavier cells need V2 multi-batch split.
    static constexpr int MAX_CELL_CONTENT_REFS_PER_BATCH = 512;

    // Simple writer: builds a packet into a flat buffer
    class PacketWriter
    {
    public:
        PacketWriter(PacketType a_type, uint8_t* a_buffer, size_t a_capacity)
            : m_buffer(a_buffer), m_capacity(a_capacity)
        {
            m_header = reinterpret_cast<PacketHeader*>(m_buffer);
            m_header->type = a_type;
            m_header->size = 0;
            m_pos = HEADER_SIZE;
        }

        template <typename T>
        bool Write(const T& a_value)
        {
            if (m_pos + sizeof(T) > m_capacity) {
                return false;
            }
            std::memcpy(m_buffer + m_pos, &a_value, sizeof(T));
            m_pos += sizeof(T);
            m_header->size = static_cast<uint16_t>(m_pos - HEADER_SIZE);
            return true;
        }

        bool WriteRaw(const void* a_data, size_t a_size)
        {
            if (m_pos + a_size > m_capacity) return false;
            std::memcpy(m_buffer + m_pos, a_data, a_size);
            m_pos += a_size;
            m_header->size = static_cast<uint16_t>(m_pos - HEADER_SIZE);
            return true;
        }

        size_t Size() const { return m_pos; }
        const uint8_t* Data() const { return m_buffer; }

    private:
        uint8_t*      m_buffer;
        size_t        m_capacity;
        size_t        m_pos;
        PacketHeader* m_header;
    };

    // Simple reader: reads fields from a received packet
    class PacketReader
    {
    public:
        PacketReader(const uint8_t* a_data, size_t a_length)
            : m_data(a_data), m_length(a_length), m_pos(HEADER_SIZE)
        {
            if (a_length >= HEADER_SIZE) {
                std::memcpy(&m_header, a_data, HEADER_SIZE);
            }
        }

        PacketType Type() const { return m_header.type; }
        uint16_t PayloadSize() const { return m_header.size; }

        bool IsValid() const
        {
            return m_length >= HEADER_SIZE &&
                   m_length >= HEADER_SIZE + m_header.size;
        }

        template <typename T>
        bool Read(T& a_out)
        {
            if (m_pos + sizeof(T) > m_length) {
                return false;
            }
            std::memcpy(&a_out, m_data + m_pos, sizeof(T));
            m_pos += sizeof(T);
            return true;
        }

        bool ReadRaw(void* a_out, size_t a_size)
        {
            if (m_pos + a_size > m_length) return false;
            std::memcpy(a_out, m_data + m_pos, a_size);
            m_pos += a_size;
            return true;
        }

    private:
        const uint8_t* m_data;
        size_t         m_length;
        size_t         m_pos;
        PacketHeader   m_header{};
    };
}
