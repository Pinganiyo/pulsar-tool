#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include "Animation/BoneSync.h"
#include "Network/PacketTypes.h"

namespace RE { class TESGlobal; class Actor; class TESObjectREFR; }
namespace Pulsar::Network { struct BoneData; }

namespace Pulsar::RemotePlayer
{
    // downed pose - perform a BGSAction (AACT FormID) on any actor via the
    // engine PerformAction dispatcher (Phase-E primitive). Game thread only.
    bool PerformActionOnActor(RE::Actor* a_actor, std::uint32_t a_actionFormID);

    // downed pose - while true, the clone anim sync asserts iSyncBleedout=1
    // (instead of zeroing it) so the downed partner's clone holds the pose.
    void SetCloneBleedoutPose(bool a_on);

    // downed pose - set an int graph variable on any actor's active graph
    // (SEH-guarded). Used to hold iSyncBleedout on the local player.
    bool SetActorGraphInt(RE::Actor* a_actor, const char* a_name, std::int32_t a_value);

    // downed - while true, the local player's combat actions (fire/attack/
    // reload/draw/melee/throw) are swallowed in the PerformAction hook.
    void SetPlayerCombatActionsBlocked(bool a_on);

    // Combat V2-1e - tick of the local player's last attack action (fire/melee/
    // throw), from the PerformAction hook. 0 = never. NPCSync's damage-forward
    // gates on this for attacker attribution.
    std::uint64_t GetLastLocalPlayerAttackMs();

    // F.2-v2 - one resolved worn-equipment entry (receiver-local FormIDs).
    struct EquipV2Entry
    {
        std::uint32_t formID = 0;
        bool          isWeapon = false;
        std::uint16_t modSentinel = 0xFFFF;
        std::vector<std::uint32_t> mods;
    };

    struct RemotePositionData
    {
        float x, y, z;
        float rotX, rotY, rotZ;
        float speed       = 0.0f;
        float direction    = 0.0f;
        float lookHeading  = 0.0f;
        uint16_t animFlags = 0;
        Network::AnimVarSnapshot vars{};
    };

    class RemotePlayerManager
    {
    public:
        // Called from network thread
        void RequestSpawn();
        void RequestDespawn();
        // Despawn + delayed respawn (clears stale Papyrus state). spawnDelayMs = the
        // despawn->spawn settle gap. proc-cell-walk recovery passes a short delay
        // (fast reappear); save-load / stale-state callers keep the safe 2000ms default.
        void RequestForceRespawn(std::uint32_t spawnDelayMs = 2000);
        // clone-flicker fix. Outcome of the orphan check / reparent.
        enum class CloneReparentResult {
            kVisible,      // 3D still parented in a loaded scene-graph → leave it (survives across cells)
            kReparented,   // 3D was orphaned → MoveTo re-attached it, no despawn
            kDead          // no loaded 3D → caller must RequestForceRespawn()
        };
        // Only act when the clone is genuinely orphaned (3D present but detached from
        // the render scene-graph). Visible clones are left alone (no respawn flicker);
        // orphaned ones are MoveTo-reparented (no despawn). Game thread only.
        CloneReparentResult ReparentCloneIfOrphaned();
        void RequestSetPosition(float x, float y, float z, float rotX, float rotY, float rotZ,
                                float speed = 0.0f, float direction = 0.0f,
                                float lookHeading = 0.0f, uint16_t animFlags = 0,
                                const Network::AnimVarSnapshot& vars = {});
        void RequestSetBones(const Network::BoneData& data);
        void RequestEquipWeapon(uint32_t formID);
        void RequestEquipArmor(const uint32_t* formIDs, uint8_t count);
        // F.2-v2: full worn set (weapon + armor) incl. mods, from EquipSync RX.
        void RequestEquipV2(std::vector<EquipV2Entry>&& a_entries);
        void RequestFire(uint32_t actionFormID);

        // Called from game thread (~1000x/sec)
        void ProcessPending();

        // Phase I - track the remote player's parentCell so DoSetPosition
        // can suppress applies that would push the clone into a different
        // cell than the local player (cross-cell clones cause engine
        // pingpong + 3D detach loops; verified 2026-04-27 logs).
        void SetRemoteCellID(uint32_t a_cellID);   // also stamps GetRemoteCellAtMs
        // GetTickCount64() of the last SetRemoteCellID (0 = never).
        uint64_t GetRemoteCellAtMs() const { return m_remoteCellAtMs.load(std::memory_order_acquire); }
        uint32_t GetRemoteCellID() const { return m_remoteCellID.load(std::memory_order_acquire); }

        bool IsSpawned() const { return m_spawned; }
        // True if spawn is already in-flight (pending, delayed, or done) - prevents re-triggering
        bool IsSpawnPending() const { return m_spawned || m_delayedSpawnAt > 0 || m_pendingForceRespawn.load() || m_pendingSpawn.load(); }
        bool IsGlobsResolved() const { return m_globsResolved; }
        // Reset so that ProcessPending will re-run ResolveGlobs (e.g. after a save load).
        void ResetGlobsResolved() { m_globsResolved = false; }
        // L.7 V13g - invalidate cached RE::Actor*/RE::TESObjectREFR* pointers
        // immediately on save receipt. The engine destroys those objects
        // during the save-load and recreates them. Without this nuke,
        // ProcessPending / CellDiag / VehicleSync read freed memory →
        // pure-virtual-call crash (V13f.1 test). Pointers will be
        // re-resolved by FindSpawnedActor + FindLookMarker post-load.
        void ClearStaleClonePtrs() {
            m_remoteActor = nullptr;
            m_lookMarker = nullptr;
            m_spawned = false;
        }
        ///both-down crash - a LOCAL game-over reload (both players downed
        // -> forced death -> save-load) destroys+recreates all actors, but unlike a
        // network save receipt nothing invalidated the clone. ProcessPending's weapon-
        // draw tick then deref'd the freed clone (dump: ProcessPending+0xceb
        // call [rax+0x9B0]=DrawWeaponMagicHands, rax=garbage). Call this on the
        // loadMenu RISING edge: null the cached ptrs, CLEAR the spawn GLOB FormID so
        // FindSpawnedActor can't re-acquire the stale one, and re-queue the worn
        // equipment so the freshly re-spawned clone gets it back. Defined in the .cpp
        // (touches TESGlobal - incomplete type here).
        void InvalidateCloneOnSaveLoad();
        RE::Actor* GetRemoteActor() const { return m_remoteActor; }
        float GetNetSpeed() const { return m_targetPosData.speed; }
        uint16_t GetAnimFlags() const { return m_targetPosData.animFlags; }
        // Proximity voice  - latest received peer position (net values),
        // same coordinate space as the local player's data.location.
        float GetNetX() const { return m_targetPosData.x; }
        float GetNetY() const { return m_targetPosData.y; }
        float GetNetZ() const { return m_targetPosData.z; }
        bool HasWeaponEquipped() const { return m_currentWeaponFormID != 0; }

        // Bone sync (public for graph update hook access)
        BoneSync::BoneCache m_boneCache;
        BoneSync::BoneFrame m_targetBoneFrame{};
        bool                m_hasBones = false;

    private:
        void ResolveGlobs();
        void DoSpawn();
        void DoDespawn();
        void DoSetPosition();
        void DoEquipWeapon();
        void DoEquipArmor();
        void FindSpawnedActor();
        void FindLookMarker();

        // Bone sync (thread-safe pending buffer)
        std::atomic<bool>   m_pendingBones{ false };
        BoneSync::BoneFrame m_pendingBoneFrame{};
        std::mutex          m_boneMutex;

        // GLOB form cache (resolved once via LookupByEditorID)
        RE::TESGlobal* m_globPosX   = nullptr;
        RE::TESGlobal* m_globPosY   = nullptr;
        RE::TESGlobal* m_globPosZ   = nullptr;
        RE::TESGlobal* m_globRotZ   = nullptr;
        RE::TESGlobal* m_globSpeed  = nullptr;
        RE::TESGlobal* m_globDirty  = nullptr;
        RE::TESGlobal* m_globSpawn  = nullptr;
        RE::TESGlobal* m_globFormHi = nullptr;
        RE::TESGlobal* m_globFormLo = nullptr;
        RE::TESGlobal* m_globLookHi = nullptr;
        RE::TESGlobal* m_globLookLo = nullptr;
        bool m_globsResolved = false;

        // Phase I - remote player's parentCell formID (live).
        std::atomic<uint32_t> m_remoteCellID{ 0 };
        std::atomic<uint64_t> m_remoteCellAtMs{ 0 };

        // Remote actor (found after Papyrus spawns it and writes FormID)
        RE::Actor* m_remoteActor = nullptr;
        RE::TESObjectREFR* m_lookMarker = nullptr;
        bool       m_spawned     = false;

        // Interpolation state (C++ side, runs at framerate)
        float m_lerpX    = 0.0f;
        float m_lerpY    = 0.0f;
        float m_lerpZ    = 0.0f;
        float m_lerpRotZ = 0.0f;
        bool  m_lerpInit = false;

        // Last seen jump state - used to fire Havok-velocity impulse only on
        // state transitions (continuous write caused fall damage on clone).
        uint32_t m_lastJumpState = 0;

        // Spawn timeout tracking
        uint64_t m_spawnRequestTime = 0;
        bool     m_spawnTimeoutWarned = false;

        // Weapon equip sync
        std::atomic<bool>     m_pendingEquip{ false };
        std::atomic<uint32_t> m_pendingWeaponFormID{ 0 };
        uint32_t              m_currentWeaponFormID = 0;
        bool                  m_weaponDrawn = false;
        bool                  m_weaponDrawTarget = false;  // debounced target
        uint64_t              m_weaponDrawChangeTime = 0;  // when target changed
        // (2026-06-20) weapon-swap draw fix: 2-stage deferred re-draw. On a
        // swap the new weapon equips HOLSTERED; the normal per-tick draw is
        // suppressed while a redraw is pending (an early draw before the equip
        // settles gets swallowed and leaves the weapon stuck on the back). Stage 1
        // (m_pendingRedraw) forces a holster ~800ms after the swap; stage 2
        // (m_pendingRedrawDrawAt) draws ~250ms after that - a real holster->draw
        // transition that pulls the new weapon into the hand. actorState2 is NOT a
        // reliable draw-state indicator on a clone, so we drive this off timers/intent.
        uint64_t              m_lastDrawCall = 0;
        uint64_t              m_pendingRedraw = 0;       // stage 1: tick to holster after swap
        uint64_t              m_pendingRedrawDrawAt = 0; // stage 2: tick to draw after holster

        // Graph kickstart: fire auto draw/holster cycle after spawn to trigger
        // ActionInitializeGraphToBaseState (non-instant) on clone - without
        // this the first jump glitches because the graph stays in a partially
        // initialized state that only the Instant version gets.
        uint64_t              m_graphKickstartDrawAt    = 0;
        uint64_t              m_graphKickstartHolsterAt = 0;
        bool                  m_graphKickstartDone      = false;

        // Armor equip sync
        std::atomic<bool>     m_pendingArmor{ false };
        uint32_t              m_pendingArmorIDs[16]{};
        uint8_t               m_pendingArmorCount = 0;
        // F.2-v2: mods per pending item - index-aligned with m_pendingArmorIDs;
        // weapon mods separate. Written by RequestEquipV2 (ENet thread), read
        // by DoEquipWeapon/DoEquipArmor (game thread) under m_equipModsMutex.
        std::mutex            m_equipModsMutex;
        std::vector<uint32_t> m_weaponMods;
        uint16_t              m_weaponModSentinel = 0xFFFF;
        std::vector<uint32_t> m_armorMods[16];
        uint16_t              m_armorModSentinels[16]{};
        uint32_t              m_currentArmorIDs[16]{};
        uint8_t               m_currentArmorCount = 0;

        // Fire sync - V2-1f: ordered queue (ENet thread pushes, game thread
        // drains ALL per tick). The old single-slot atomics lost actions when
        // several PKT_FIRE arrived in one ENet pump (auto weapons).
        std::mutex            m_fireQueueMutex;
        std::vector<uint32_t> m_fireQueue;
        std::atomic<uint32_t> m_lastFireAction{ 0 };  // most recent, for sighted-state tracking

        // F.2-v2: tick the clone was last (re)spawned. Equip apply waits a
        // settle window after this so it never runs on a graph mid-rebuild.
        std::atomic<uint64_t> m_cloneSpawnedAtMs{ 0 };

        // Thread-safe request queue
        std::atomic<bool> m_pendingSpawn{ false };
        std::atomic<bool> m_pendingDespawn{ false };
        std::atomic<bool> m_pendingForceRespawn{ false };
        std::atomic<uint32_t> m_forceRespawnDelayMs{ 2000 };  // despawn->spawn gap for the pending force-respawn
        std::atomic<uint64_t> m_delayedSpawnAt{ 0 };  // tickcount to call DoSpawn after force-despawn
        std::atomic<bool> m_pendingPosition{ false };
        bool              m_hasPosition = false;

        std::mutex          m_posMutex;
        RemotePositionData  m_pendingPosData{};
        RemotePositionData  m_targetPosData{};
    };

    RemotePlayerManager& GetRemotePlayerManager();
}
