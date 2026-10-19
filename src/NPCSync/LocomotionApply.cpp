#include "PCH.h"
#include "Animation/DirectionMap.h"
#include "NPCSync/LocomotionApply.h"
#include "NPCSync/NPCSync.h"   // P1: MarkNpcAiBlocked (AI-block routing)

#include <RE/A/Actor.h>
#include <RE/B/BSAnimationGraph.h>
#include <RE/B/BSFixedString.h>
#include <RE/B/BSTSmartPointer.h>
#include <RE/N/NiAVObject.h>
#include <RE/P/PlayerCharacter.h>   // GRAPH-DIAG reference

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <unordered_map>

using namespace std::string_view_literals;

namespace Pulsar::NPCSync {

    // #ragdoll (2026-06-25) - kill-pending window. The engine kill (SafeKillActor) is
    // ASYNC: actor->IsDead() stays false for several frames after the call. In that gap
    // ApplyLocomotion/TickLerp would keep driving + re-pinning the not-yet-dead NPC, so
    // when death finally lands the body is pinned upright → no ragdoll ("dead but
    // standing", the first-kill-stuck). When a kill is fired we mark the actor
    // kill-pending; both drivers then treat it like dead (stop driving, release
    // DirectControl) so the body stays free and the engine death ragdolls it.
    static std::unordered_map<uint32_t, uint64_t> g_npcKillPending;  // formID -> expiry tick
    static std::mutex                             g_npcKillPendingMutex;
    void MarkNpcKillPending(uint32_t formID, uint64_t durationMs)
    {
        if (formID == 0) return;
        std::lock_guard lk(g_npcKillPendingMutex);
        g_npcKillPending[formID] = GetTickCount64() + durationMs;
    }
    static bool IsNpcKillPending(uint32_t formID)
    {
        if (formID == 0) return false;
        std::lock_guard lk(g_npcKillPendingMutex);
        auto it = g_npcKillPending.find(formID);
        if (it == g_npcKillPending.end()) return false;
        if (GetTickCount64() >= it->second) { g_npcKillPending.erase(it); return false; }
        return true;
    }

    // Starfield units are METERS (verified from SYNC-DIAG log: raw delta 24.6
    // displays as "24.6m"). Earlier assumption "unit ≈ 1 cm" (and its 200.0f /
    // 30.0f thresholds) meant the snap never fired.
    //
    // 0.5 m drift triggers a hard SetPosition (mesh / Havok teleport). Below
    // that, per-tick TickLerp closes the gap smoothly.
    static constexpr float kDriftThreshold   = 0.5f;
    static constexpr float kDriftThresholdSq = kDriftThreshold * kDriftThreshold;

    // Stored target per-actor for per-tick interpolation + dead reckoning.
    // ApplyLocomotion writes { pos, rotZ, velocity, timestamp } per packet.
    // TickLerp extrapolates pos forward by (velocity × elapsed) to cancel the
    // network-age lag (~33 ms at 30 Hz send rate; at 3.5 m/s that's ~12 cm of
    // visible nachzieh-lag per packet, cumulative with jitter into a ~0.5 m
    // steady-state drift). With prediction, the displayed position is WHERE
    // the actor IS NOW on the host, not where it was when the packet left.
    struct LerpTarget {
        RE::NiPoint3 pos;          // current packet position
        RE::NiPoint3 velocity;     // world-space m/s - derived from Δpos / Δt
        RE::NiPoint3 prevPos;      // previous packet position (for Δ)
        uint64_t     prevMs;       // timestamp of prevPos
        float        rotZ;         // yaw at packet time
        uint64_t     updatedMs;    // current packet's timestamp
        // Graph-var inputs - re-applied every tick so local AI can't win.
        uint8_t      moveFlags;
        bool         weaponDrawn;
        bool         hasPrev;      // false on first packet - no velocity yet
        // Host's authoritative graph "Speed" var (Starfield units, matches the
        // gait-ref scale). Smooth + host-side correct - the value the host NPC's
        // own engine used to pick its gait. Fed to IMovementDirectControl SetSpeed
        // instead of the spiky Δpos velocity (which made walkers jog). 0 for
        // low-AI (MiddleLow/Low) NPCs → we fall back to Δpos for those.
        float        graphSpeed;
    };
    static std::mutex                             g_targetsMutex;
    static std::unordered_map<uint32_t, LerpTarget> g_targets;

    // Cap extrapolation horizon. Linear prediction is great for 50-200 ms but
    // diverges catastrophically if packets stop (actor ends up sprinting off
    // the map). Above this, freeze the predicted target at the cap value.
    static constexpr float kMaxPredictSec = 0.25f;  // 250 ms

    // Engage velocity write above ~0.1 m/s - slow walks (graph-var Speed
    // values as low as 0.5) still need the Havok push.
    static constexpr float kVelocityEpsilon = 0.1f;

    // Per-actor SetPosition throttle. Phase D v14 ran at 500 ms / 1 m to
    // mitigate the 0x30B350E bone-array NULL-deref race on the render thread
    // (Havok broadcast wakes a worker thread that races with bone iteration).
    // 200 ms is tighter (5 snaps/s/actor) but only fires when drift really
    // demands it - combined with the 0.5 m gate the actual snap rate stays
    // well below the Phase D v13 flood that crashed.
    static constexpr uint64_t kSnapThrottleMs = 200;

    static uint64_t NowMs()
    {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   clock::now().time_since_epoch()).count();
    }

    // --- Havok CharController linear-velocity write -------------------------
    // Ported verbatim from RemotePlayerManager::SafeHavokBodySetLinearVelocityImpl
    // (T8 RE'd). Path:
    //   actor + 0x228              = AIProcess (currentProcess)
    //              + 0x08           = middleHigh
    //              + 0x458          = bhkCharRigidBodyController (CC)
    //   CC + 0x20                  = body-wrapper (null until Havok attach start)
    //   (CC+0x20) + 0x20           = inner Havok object (null until attach done)
    // vtable slot 79 (byte 0x278) = setLinearVelocity(ccObj, float* vel4).
    // Gating on both attach levels is mandatory - slot 87 (the world-getter
    // called inside setLinearVelocity) dereferences the inner object without
    // a null check.
    // Returns true iff the velocity write actually landed on a live Havok
    // body. False = one of the middleHigh / CC / body-wrapper / inner-havok
    // gates was null (common for exterior proc-gen NPCs in MiddleLow/Low AI
    // state) - caller falls back to SetPosition for position sync.
    static bool SafeHavokSetLinearVelocity(RE::Actor* actor, float vx, float vy, float vz)
    {
        bool ok = false;
        __try {
            auto u = reinterpret_cast<uintptr_t>(actor);
            uintptr_t aiProc = *reinterpret_cast<uintptr_t*>(u + 0x228);
            if (!aiProc) return false;
            uintptr_t middleHigh = *reinterpret_cast<uintptr_t*>(aiProc + 0x08);
            if (!middleHigh) return false;
            void* ccObj = *reinterpret_cast<void**>(middleHigh + 0x458);
            if (!ccObj) return false;

            uintptr_t bodyWrapper = *reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uintptr_t>(ccObj) + 0x20);
            if (!bodyWrapper) return false;
            uintptr_t innerHavokObj = *reinterpret_cast<uintptr_t*>(bodyWrapper + 0x20);
            if (!innerHavokObj) return false;

            auto** vtable = *reinterpret_cast<void***>(ccObj);
            if (!vtable) return false;

            alignas(16) float vel4[4] = { vx, vy, vz, 0.0f };

            using SetVelFn = void(__fastcall*)(void*, float*);
            auto setVel = reinterpret_cast<SetVelFn>(vtable[0x278 / 8]);
            setVel(ccObj, vel4);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Body race despite gate - CC detached between the two checks.
            ok = false;
        }
        return ok;
    }

    // Havok CC body SET-POSITION - vtable slot 0x240/8, sig void(ccObj,float[4],
    // char,char). Moves the collision controller (the visible mesh follows it)
    // WITHOUT Actor::SetPosition, which resets the anim state-machine each frame
    // (= the "no walk anim / floating" we saw). Ported verbatim from the clone's
    // proc-cell mover (RemotePlayerManager::SafeHavokBodySetPositionImpl).
    static bool SafeNpcHavokBodySetPos(RE::Actor* actor, float x, float y, float z)
    {
        bool ok = false;
        __try {
            auto u = reinterpret_cast<uintptr_t>(actor);
            uintptr_t aiProc = *reinterpret_cast<uintptr_t*>(u + 0x228);
            if (!aiProc) return false;
            uintptr_t middleHigh = *reinterpret_cast<uintptr_t*>(aiProc + 0x08);
            if (!middleHigh) return false;
            void* ccObj = *reinterpret_cast<void**>(middleHigh + 0x458);
            if (!ccObj) return false;
            uintptr_t bodyWrapper = *reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uintptr_t>(ccObj) + 0x20);
            if (!bodyWrapper) return false;
            uintptr_t innerHavokObj = *reinterpret_cast<uintptr_t*>(bodyWrapper + 0x20);
            if (!innerHavokObj) return false;
            auto** vtable = *reinterpret_cast<void***>(ccObj);
            if (!vtable) return false;
            alignas(16) float pos4[4] = { x, y, z, 0.0f };
            using SetPosFn = void(__fastcall*)(void*, float*, char, char);
            auto setPos = reinterpret_cast<SetPosFn>(vtable[0x240 / 8]);
            setPos(ccObj, pos4, 1, 1);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        return ok;
    }

    // Raw data.location write (the logical position) + yaw, paired with the
    // Havok-body write so logic + visible mesh stay together. No SetPosition.
    static bool SafeNpcWriteDataLoc(RE::Actor* actor, float x, float y, float z, float rotZ)
    {
        __try {
            actor->data.location.x = x;
            actor->data.location.y = y;
            actor->data.location.z = z;
            actor->data.angle.z    = rotZ;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // --- Direct-vtable graph-var writes -------------------------------------
    // IAnimationGraphManagerHolder in CommonLibSF only exposes the getter
    // virtuals (slots 12/13). Writing goes through BSAnimationGraph's own
    // vtable: slot 0x20 = SetVariableInt, slot 0x21 = SetVariableFloat.
    // Same path used by RemotePlayerManager::ForceSetGraphIntDirect and the
    // NPCSync apply-loop.
    //
    // Plain-C SEH wrappers: __try cannot coexist with C++ unwinding (C2712),
    // so the caller owns the BSFixedString.
    static int SafeSetGraphFloatImpl(void* graphPtr, void* fnPtr, const void* namePtr, float val)
    {
        __try {
            using SetFloatFn = bool(__fastcall*)(void*, const void*, float);
            reinterpret_cast<SetFloatFn>(fnPtr)(graphPtr, namePtr, val);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static int SafeSetGraphIntImpl(void* graphPtr, void* fnPtr, const void* namePtr, int32_t val)
    {
        __try {
            using SetIntFn = bool(__fastcall*)(void*, const void*, int32_t);
            reinterpret_cast<SetIntFn>(fnPtr)(graphPtr, namePtr, val);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // SEH-wrapped SetPosition snap. Mirrors NPCSync.cpp::SafeNpcSetPosition.
    static int SafeSetPositionSnap(RE::Actor* actor, const RE::NiPoint3& target, float rotZ)
    {
        __try {
            actor->SetPosition(target, true);
            actor->data.angle.z = rotZ;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // SEH-wrapped yaw lerp. data.angle sits at actor+0x98 - safe to touch, but
    // we wrap anyway in case the actor is being torn down concurrently.
    static int SafeLerpRotZ(RE::Actor* actor, float target, float factor)
    {
        __try {
            const float cur   = actor->data.angle.z;
            const float delta = target - cur;
            actor->data.angle.z = cur + delta * factor;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // SEH-wrapped read of actor->data.location into a plain float[3]. Keeps
    // drift computation outside of any path that could fault mid-tick.
    static int SafeReadDataLocation(RE::Actor* actor, float out[3])
    {
        __try {
            out[0] = actor->data.location.x;
            out[1] = actor->data.location.y;
            out[2] = actor->data.location.z;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out[0] = 0; out[1] = 0; out[2] = 0;
            return 0;
        }
    }

    // SEH-wrapped read of actor->data.angle.z for heading computation.
    static int SafeReadRotZ(RE::Actor* actor, float* out)
    {
        __try {
            *out = actor->data.angle.z;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *out = 0.0f;
            return 0;
        }
    }

    bool ApplyLocomotion(RE::Actor* actor, const LocomotionInput& in)
    {
        if (!actor) return false;

        // #ragdoll-fix: never drive a DEAD actor. Late loco packets arriving right
        // after a kill would re-publish a lerp target / re-write graph vars / re-
        // assert DirectControl and pin the corpse upright, overriding the death anim
        // (the intermittent "dead but standing" freeze). Let it ragdoll; TickLerp
        // also bails + releases DirectControl on IsDead.
        // #ragdoll - also bail while kill-pending (async death gap) so we don't re-pin.
        if (actor->IsDead() || IsNpcKillPending(actor->formID)) return false;

        // Publish the target + derived velocity for the per-tick lerp. Done
        // first so even if a later step bails, the lerper still has a fresh
        // target.
        //
        // Velocity is derived from (current-pos - previous-pos) / Δt rather
        // than from the graph-var "Speed" field, because Starfield's Speed
        // var is only populated for MiddleHigh-AI actors - the 70 %+ of
        // exterior / background NPCs in MiddleLow or Low states report
        // Speed=0 despite visibly walking on the host. Δpos captures the
        // *actual* motion regardless of AI level.
        {
            const uint64_t now = NowMs();
            std::lock_guard lk(g_targetsMutex);
            auto& t = g_targets[actor->formID];

            if (t.hasPrev) {
                const uint64_t dtMs = now - t.updatedMs;
                if (dtMs > 0 && dtMs < 500) {    // sane Δ window; else skip (cell-change / gap)
                    const float dt = static_cast<float>(dtMs) / 1000.0f;
                    const float dx = in.targetPos.x - t.pos.x;
                    const float dy = in.targetPos.y - t.pos.y;
                    float newVx = dx / dt;
                    float newVy = dy / dt;
                    // Reject host teleports / proc-cell swaps / spawn-Jumps.
                    // Max plausible speed is sprint ≈ 5.5 m/s. Anything above
                    // 8 m/s is a discontinuity - reset velocity to 0 and treat
                    // the next packet as a fresh reference.
                    const float magSq = newVx * newVx + newVy * newVy;
                    if (magSq > 8.0f * 8.0f) {
                        t.velocity = RE::NiPoint3(0.0f, 0.0f, 0.0f);
                    } else {
                        // Exponential smoothing 0.6/0.4 - kills single-packet jitter
                        // while staying responsive to direction changes within a few packets.
                        t.velocity.x = t.velocity.x * 0.6f + newVx * 0.4f;
                        t.velocity.y = t.velocity.y * 0.6f + newVy * 0.4f;
                        t.velocity.z = 0.0f;
                    }
                } else {
                    // Gap too big or zero Δt - reset velocity to avoid overshoot.
                    t.velocity = RE::NiPoint3(0.0f, 0.0f, 0.0f);
                }
            }

            t.prevPos     = t.pos;
            t.prevMs      = t.updatedMs;
            t.pos         = in.targetPos;
            t.rotZ        = in.targetRotZ;
            t.updatedMs   = now;
            t.moveFlags   = in.moveFlags;
            t.weaponDrawn = in.weaponDrawn;
            t.graphSpeed  = in.speed;   // host-authoritative gait speed (for SetSpeed)
            t.hasPrev     = true;
        }

        // P3 combat-live: the NPC's own AI owns its position. We keep the lerp
        // TARGET fresh (above) for when it leaves combat, but must NOT write its
        // position/Havok-velocity here - this per-packet (~30 Hz, throttled 200 ms)
        // SetPosition snap was fighting the AI movement = the "jitter, can't hit
        // them" bug. Bail before any position write; TickLerp also skips them.
        if (NpcCombatAiLiveOn() && IsNpcCombatLive(actor->formID)) return true;

        // Graph vars + Havok velocity are now written every client tick via
        // TickLerp (see WriteGraphVarsImpl below). The per-packet
        // ApplyLocomotion call is responsible only for updating the stored
        // target + the drift-snap decision; writing graph vars at 30 Hz was
        // being out-voted by the local AI's 60 Hz graph writes, producing
        // visible anim flicker and stuck actors. 60 Hz writes from TickLerp
        // (after g_origActorUpdate) run LAST in the tick and win.
        //
        // Retained-in-function: legacy graph-write block below is now a
        // no-op skeleton kept only for git-history continuity; the real
        // writes happen in WriteGraphVarsImpl.
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphMgr;
        const bool graphOk = false &&
            actor->GetAnimationGraphManagerImpl(graphMgr) &&
            graphMgr &&
            graphMgr->graphs.size() > 0;

        if (graphOk) {
            auto& graph = graphMgr->graphs[graphMgr->activeGraph];
            if (graph) {
                auto** vt          = *reinterpret_cast<void***>(graph.get());
                void*  setIntFn    = vt[0x20];
                void*  setFloatFn  = vt[0x21];
                void*  graphPtr    = graph.get();

                // Intern once - BSFixedString hits a hashmap; we pay the lookup
                // cost exactly once per process.
                //
                // This set mirrors RemotePlayerManager::DoSetPosition's clone
                // graph-var writes. Learned the hard way: writing only
                // Speed + Direction + iSyncSprintState leaves other state
                // vars at engine defaults (often 0 / undefined), which makes
                // the anim graph's state-machine reject the walk transitions
                // and park the actor in idle-limbo → visibly frozen pose,
                // root-motion never translates the actor → "teleport" look.
                static const RE::BSFixedString kSpeed("Speed");
                static const RE::BSFixedString kCurSpeed("CurrentSpeed");
                static const RE::BSFixedString kDirection("Direction");
                static const RE::BSFixedString kIdleLoco("iSyncIdleLocomotion");
                static const RE::BSFixedString kSprint("iSyncSprintState");
                static const RE::BSFixedString kIState("iState");
                static const RE::BSFixedString kRelaxReadySneak("iSyncRelaxReadySneak");
                static const RE::BSFixedString kRelaxedReadyAlert("iSyncRelaxedReadyAlert");
                static const RE::BSFixedString kGravity("iSyncGravity");
                static const RE::BSFixedString kStance("iSyncActorStance");
                static const RE::BSFixedString kWeaponDraw("iSyncWeaponDrawState");
                static const RE::BSFixedString kHeadTrack("bHeadTrackingActive");
                static const RE::BSFixedString kNoHeadTrack("bNoHeadTrack");
                static const RE::BSFixedString kGunActions("iSyncGunActions");
                static const RE::BSFixedString kGunDown("iSyncGunDown");
                static const RE::BSFixedString kSwimState("iSyncSwimState");
                static const RE::BSFixedString kBleedout("iSyncBleedout");
                static const RE::BSFixedString kPartialBody("iSyncPartialBody");
                static const RE::BSFixedString kSighted("iSyncSighted");
                static const RE::BSFixedString kGraphDriven("bGraphDriven");

                // Sarah-pattern floats: Speed and CurrentSpeed must both be
                // the same actual-world-speed value - the anim graph blends
                // locomotion weights off their agreement.
                SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kSpeed,     in.speed);
                SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kCurSpeed,  in.speed);
                SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kDirection, in.direction);

                // Body-state ints. iState is the gate for the whole locomotion
                // subgraph: 10 = normal-stand, 11 = sneak, 17 = combat-ready.
                // We don't currently transmit stance, so default to normal;
                // combat NPCs keep their AI-driven pose anyway since our write
                // is overwritten next tick by the engine's combat update.
                const int32_t iState = 10;
                const int32_t relax  = 0;
                const int32_t stance = 0;
                const int32_t idleLoco = (in.speed > 0.1f) ? 0 : 1;  // engine convention: 1 = idle
                const int32_t sprint   = (in.moveFlags & 0x2) ? 1 : 0;

                SafeSetGraphIntImpl(graphPtr, setIntFn, &kIState,             iState);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kIdleLoco,           idleLoco);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kSprint,             sprint);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kRelaxReadySneak,    relax);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kRelaxedReadyAlert,  relax);

                // Unlocks: explicit zeros so the graph never parks on a
                // stale non-zero from the local AI before we took over.
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kGravity,     0);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kStance,      stance);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kWeaponDraw,  in.weaponDrawn ? 1 : 0);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kHeadTrack,   1);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kNoHeadTrack, 0);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kGunActions,  0);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kGunDown,     0);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kSwimState,   0);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kBleedout,    0);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kPartialBody, 0);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kSighted,     0);
            }
        }

        // --- 2. Havok linear velocity ----------------------------------------
        // World-space velocity from speed + (actor yaw + body-relative heading).
        // Bethesda forward = (sin(yaw), cos(yaw)) - matches the look-marker
        // convention documented in feedback_pitch_approaches.
        bool velApplied = false;
        if (in.speed > kVelocityEpsilon) {
            float curYaw = 0.0f;
            SafeReadRotZ(actor, &curYaw);
            const float heading = curYaw + in.direction;
            const float vx = std::sin(heading) * in.speed;
            const float vy = std::cos(heading) * in.speed;
            velApplied = SafeHavokSetLinearVelocity(actor, vx, vy, 0.0f);
        } else {
            velApplied = SafeHavokSetLinearVelocity(actor, 0.0f, 0.0f, 0.0f);
        }

        // --- 3. Drift correction / yaw lerp ----------------------------------
        float cur[3] = { 0, 0, 0 };
        SafeReadDataLocation(actor, cur);
        const float dx = cur[0] - in.targetPos.x;
        const float dy = cur[1] - in.targetPos.y;
        const float dz = cur[2] - in.targetPos.z;
        const float driftSq = dx * dx + dy * dy + dz * dz;

        // Snap conditions:
        //   (a) drift exceeds 0.5 m (anim blend can't close it fast enough at
        //       30 Hz packet rate - and client-side AI can wander the actor
        //       away independently of host, which happens on companions like
        //       Sarah who follow the *client* player locally), OR
        //   (b) velocity write failed (CC null for MiddleLow/Low-state NPCs -
        //       exterior proc-gen, background crowd) AND host reports motion.
        //       Without a Havok-driven push these actors would stand still
        //       while their anim graph plays a walk cycle.
        const bool driftSnap    = driftSq > kDriftThresholdSq;
        const bool fallbackSnap = !velApplied && in.speed > kVelocityEpsilon;
        const bool needSnap     = driftSnap || fallbackSnap;

        if (needSnap) {
            // Per-actor throttle: protects against 0x30B350E bone-array race
            // that Phase D v13's always-snap 30 Hz flood reliably triggered.
            static std::unordered_map<uint32_t, uint64_t> s_lastSnap;
            static std::mutex                             s_lastSnapMutex;
            const uint64_t now = NowMs();
            bool canSnap = false;
            {
                std::lock_guard lk(s_lastSnapMutex);
                auto& last = s_lastSnap[actor->formID];
                if (now - last >= kSnapThrottleMs) {
                    last = now;
                    canSnap = true;
                }
            }
            if (canSnap) {
                SafeSetPositionSnap(actor, in.targetPos, in.targetRotZ);
            } else {
                // Throttled this tick - still converge yaw so we don't sit
                // wrong-facing for up to 200 ms.
                SafeLerpRotZ(actor, in.targetRotZ, 0.2f);
            }
        } else {
            // ~5-tick convergence at 30 Hz (1 - 0.8^5 ≈ 67%).
            SafeLerpRotZ(actor, in.targetRotZ, 0.2f);
        }

        return true;
    }

    // --- Mesh world-transform overrides for proc-gen NPCs ---
    // Proc-gen NPCs (FF-prefix formIDs in proc cells) have flags2 bit 11
    // clear + collisionObject non-null; the engine's UpdateWorldData
    // delegates mesh.world computation to the Havok collision object and
    // SKIPS the normal `world = parent.world * local` recompute (T4 Sect 3).
    // Without origUpdate firing (our remote-owned skip), the collision
    // object never moves → mesh.world stays frozen at spawn → visible
    // "schweben / teleport" while data.location tracks host correctly.
    //
    // Fix: every tick, (a) write NiAVObject.world.translate directly at
    // +0xB0 so the root node's transform matches our target, and (b) call
    // NiAVObject::Update (vtable slot 73) with flags2 bit 11 set to force
    // the downward-pass propagation through child bones with the collision
    // branch SKIPPED. Net: mesh renders at our target, skeleton world
    // transforms rebuild from the new root.
    static void SafeWriteNodeWorld(RE::NiAVObject* node, float x, float y, float z)
    {
        __try {
            auto* wt = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(node) + 0xB0);
            wt[0] = x; wt[1] = y; wt[2] = z;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    static void SafeReadNodeWorld(RE::NiAVObject* node, float out[3])
    {
        __try {
            auto* wt = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(node) + 0xB0);
            out[0] = wt[0]; out[1] = wt[1]; out[2] = wt[2];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out[0] = 0; out[1] = 0; out[2] = 0;
        }
    }

    static void SafeForceDownwardPass(RE::NiAVObject* node)
    {
        __try {
            alignas(8) uint8_t buf[0x60]{};
            *reinterpret_cast<uint32_t*>(buf + 0x38) = 0x10;
            *reinterpret_cast<uint32_t*>(buf + 0x3C) = 0x800;  // bit 11 = skip collision delegation
            *reinterpret_cast<uint32_t*>(buf + 0x40) = 1;
            auto* vt = *reinterpret_cast<uintptr_t**>(node);
            using UpdateFn = void(*)(RE::NiAVObject*, void*);
            reinterpret_cast<UpdateFn>(vt[73])(node, buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Helper: drive the mesh transform + skeleton downward-pass. Needs the
    // loadedData read-lock around the data3D deref, so we can't put this
    // inside a __try (C2712). Pre-reads mesh pos into outPreMesh for the
    // caller's drift calculation / diagnostic.
    static void ForceMeshAndSkeleton(RE::Actor* actor,
                                     float x, float y, float z,
                                     float outPreMesh[3])
    {
        outPreMesh[0] = 0; outPreMesh[1] = 0; outPreMesh[2] = 0;
        auto guard = actor->loadedData.LockRead();
        const auto* data = *guard;
        if (!data || !data->data3D) return;
        auto* node = data->data3D.get();
        SafeReadNodeWorld(node, outPreMesh);
        SafeWriteNodeWorld(node, x, y, z);
        SafeForceDownwardPass(node);
    }

    // SEH-wrapped raw read+write of actor->data.location / angle.z -
    // TickLerp runs in the hot Actor::Update path, so we guard everything.
    // Z is DELIBERATELY NOT lerped: Havok ground-clamp manages Z for non-CC
    // NPCs, and interpolating Z at 60 Hz between packet-to-packet Z deltas
    // produced visible vertical oscillation ("floating"). Z gets corrected
    // exclusively via the periodic SetPosition snap in ApplyLocomotion.
    static int SafeLerpApply(RE::Actor* actor, const RE::NiPoint3& tgt, float tgtRotZ, float factor)
    {
        __try {
            RE::NiPoint3& loc = actor->data.location;
            loc.x += (tgt.x - loc.x) * factor;
            loc.y += (tgt.y - loc.y) * factor;
            // loc.z left untouched - see comment above.
            const float cur = actor->data.angle.z;
            actor->data.angle.z = cur + (tgtRotZ - cur) * factor;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // SEH-wrapped readback of a graph float var - diagnostic only.
    static float SafeReadGraphFloat(RE::Actor* actor, const RE::BSFixedString& v)
    {
        __try {
            float out = 0.0f;
            actor->GetGraphVariableImplFloat(v, out);
            return out;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -999.0f;
        }
    }

    // SEH-wrapped readback of a graph int var - diagnostic only (GRAPH-DIAG).
    static int32_t SafeReadGraphInt(RE::Actor* actor, const RE::BSFixedString& v)
    {
        __try {
            int32_t out = -999;
            actor->GetGraphVariableImplInt(v, out);
            return out;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1000;
        }
    }

    // ── crash-fix: anim-graph "settle" gate ───────────────────────────
    // A pure-virtual-call (exception 0xC0000025, STATUS_NONCONTINUABLE_EXCEPTION)
    // crashed the client when we drove a remote NPC's anim-graph while it was
    // still mid-construction - e.g. a freshly-spawned NPC right after a
    // no-load-screen interior door transition (NATheLodge → NAResidential02 /
    // Viewport). The graph manager exists but its behavior-graph vtable slots are
    // still pure-virtual stubs until the async .hkx finishes loading; calling
    // vt[0x21] (SetGraphFloat) then routes into the CRT _purecall handler, which
    // is NOT SEH-catchable - our __try/__except can't save us. loadMenu does NOT
    // open for interior cell loads, so IsQuiet() can't gate it either.
    //
    // The only defense is prevention: refuse to touch the graph until the actor's
    // 3D has been *continuously* loaded for a settle window (the behavior graph
    // finishes well inside it). Re-arms if 3D unloads (cell streaming).
    static std::unordered_map<uint32_t, uint64_t> g_npcGraphSettleTick;
    static std::mutex                             g_npcGraphSettleMutex;
    constexpr uint64_t kNpcGraphSettleMs = 2000;

    // Mirror ForceMeshAndSkeleton's loadedData read-lock idiom (can't be inside a
    // __try: the lock guard has a dtor → C2712). A mid-spawn/mid-teardown actor
    // has null loadedData or null data3D.
    static bool NpcActor3DLoaded(RE::Actor* actor)
    {
        auto guard = actor->loadedData.LockRead();
        const auto* data = *guard;
        return data && data->data3D;
    }

    // crash-fix v2: global cell-transition cooldown. When the player changes
    // cell (e.g. NATheLodge→NAResidential02 via an interior door - no loadMenu, so
    // IsQuiet can't see it) the engine re-initializes nearby actors' ANIM GRAPHS
    // while their 3D stays loaded, and the remote-player clone despawns/respawns in
    // the new cell. The behavior-graph vtable is briefly pure-virtual during that
    // window, so even a 3D-settled actor pure-virtual-crashes if we touch its graph.
    // Pause ALL NPC graph access for a few seconds after the player's cell changes.
    static std::atomic<uint32_t> g_lastPlayerCellID{ 0 };
    static std::atomic<uint64_t> g_playerCellChangeTick{ 0 };
    constexpr uint64_t kCellChangeCooldownMs = 5000;

    static uint32_t SafeReadPlayerCellID()
    {
        __try {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (pc && pc->parentCell) return pc->parentCell->formID;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    static bool PlayerCellSettling()
    {
        const uint64_t now    = NowMs();
        const uint32_t cellID = SafeReadPlayerCellID();
        const uint32_t prev   = g_lastPlayerCellID.load(std::memory_order_relaxed);
        if (cellID != prev) {
            g_lastPlayerCellID.store(cellID, std::memory_order_relaxed);
            g_playerCellChangeTick.store(now, std::memory_order_relaxed);
            return true;
        }
        return (now - g_playerCellChangeTick.load(std::memory_order_relaxed)) < kCellChangeCooldownMs;
    }

    // (2026-06-21) - zero-G / in-space gate. In a gravity-less station (or
    // any space context) NPCs run the engine's FLOAT behavior graph, not the
    // ground-locomotion graph. Forcing ground-loco vars (Speed/Direction/iState…)
    // onto a float graph (a) makes the NPC "walk" instead of float = visible
    // jitter (observed on Sarah) and (b) the float graph's vtable can route
    // SetGraphFloat (vt[0x21]) into _purecall → std::terminate →
    // STATUS_NONCONTINUABLE (0xC0000025, NOT SEH-catchable → hard client crash;
    // dump ef63caf1 on the LOCO write thread). Skip ALL locomotion driving while
    // the local player is in space/zero-G; let the engine drive native float loco.
    // Cached ~200ms (IsInSpace is a virtual call, NpcGraphSafeToDrive runs per
    // synced NPC at 60 Hz).
    static bool LocalEnvIsZeroG()
    {
        static std::atomic<uint64_t> s_lastCheck{ 0 };
        static std::atomic<bool>     s_cached{ false };
        const uint64_t now = NowMs();
        if (now - s_lastCheck.load(std::memory_order_relaxed) < 200)
            return s_cached.load(std::memory_order_relaxed);
        auto* pc = RE::PlayerCharacter::GetSingleton();
        const bool inSpace = pc && pc->IsInSpace(true);
        s_cached.store(inSpace, std::memory_order_relaxed);
        s_lastCheck.store(now, std::memory_order_relaxed);
        return inSpace;
    }

    static bool NpcGraphSafeToDrive(RE::Actor* actor)
    {
        if (!actor) return false;
        // never drive ground-locomotion in zero-G/space (float graph →
        // _purecall crash + walk-jitter). Engine drives native float loco there.
        if (LocalEnvIsZeroG()) return false;
        // Cell-transition teardown window → graphs mid-reinit → never touch them.
        if (PlayerCellSettling()) return false;
        const uint64_t now    = NowMs();
        const bool     has3D  = NpcActor3DLoaded(actor);
        std::lock_guard lk(g_npcGraphSettleMutex);
        if (!has3D) { g_npcGraphSettleTick.erase(actor->formID); return false; }
        auto [it, inserted] = g_npcGraphSettleTick.try_emplace(actor->formID, now);
        return (now - it->second) >= kNpcGraphSettleMs;
    }

    // Diag: ms since this NPC's graph settle-window started (0 = not seen/settled).
    static uint64_t NpcSettleElapsed(uint32_t formID)
    {
        std::lock_guard lk(g_npcGraphSettleMutex);
        auto it = g_npcGraphSettleTick.find(formID);
        return it == g_npcGraphSettleTick.end() ? 0 : (NowMs() - it->second);
    }

    // ── idle rotation latch (anti-jitter) ──────────────────────────────
    // While an NPC is on-target (idle) we pin its facing to the incoming network
    // rotZ every frame. Host-side idle micro-fidget (head-track body sway, idle
    // anim root rotation) makes that rotZ wobble a few degrees per packet, and a
    // raw per-frame write replays it as a left-right snap = the "idle jitter" seen
    // on guards. (Sarah stood with a dead-constant rotZ so she never showed it.)
    // Latch the held heading and deadband small deltas; ease in only real turns.
    static std::unordered_map<uint32_t, float> g_idleRotZLatch;
    static std::mutex                          g_idleRotZMutex;
    constexpr float kIdleRotDeadband = 0.12f;   // rad (~6.9°) - below = fidget, hold

    // Keep the latch tracking the live heading while the actor is moving, so it
    // re-pins at the correct facing the instant motion stops (no settle ease).
    static void IdleRotLatchTrack(uint32_t formID, float rotZ)
    {
        std::lock_guard lk(g_idleRotZMutex);
        g_idleRotZLatch[formID] = rotZ;
    }
    // Return the deadbanded heading to pin this idle frame.
    static float IdleRotLatchPin(uint32_t formID, float tgtRotZ)
    {
        std::lock_guard lk(g_idleRotZMutex);
        auto [it, ins] = g_idleRotZLatch.try_emplace(formID, tgtRotZ);
        float d = tgtRotZ - it->second;
        while (d >  3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        if (std::fabs(d) > kIdleRotDeadband) it->second += d * 0.30f;  // real turn → ease in
        return it->second;
    }

    // Tick-rate graph-var writer. Extracted so it can run every actor tick
    // instead of once per network packet - gives our writes a 60 Hz presence
    // that beats the local AI's 60 Hz overrides.
    static void WriteGraphVarsImpl(RE::Actor* actor, float effectiveSpeed,
                                   float effectiveDir, uint8_t moveFlags,
                                   bool weaponDrawn)
    {
        // V36: scripted-scene suspension (mirrors the TickLerp gate). While this
        // NPC is in a quest scene, skip ALL graph-var forcing. The idle branch
        // below pins rotation (DisableAllowRotation/IsUsingCodeDrivenRotation) and
        // kills head-track (bNoHeadTrack), and relied on TickLerp's per-frame rotZ
        // pin to hold the heading - but TickLerp is suspended for in-scene NPCs, so
        // that left rotation half-driven and the LISTENING NPC spun/jittered. Hand
        // the whole locomotion + look-at graph back to the local scene.
        if (actor && IsNpcInScene(actor->formID)) return;

        // crash-fix: never drive a not-yet-settled graph (pure-virtual-call).
        if (!NpcGraphSafeToDrive(actor)) return;

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphMgr;
        if (!actor->GetAnimationGraphManagerImpl(graphMgr) || !graphMgr ||
            graphMgr->graphs.size() == 0) return;
        auto& graph = graphMgr->graphs[graphMgr->activeGraph];
        if (!graph) return;

        auto** vt         = *reinterpret_cast<void***>(graph.get());
        void*  setIntFn   = vt[0x20];
        void*  setFloatFn = vt[0x21];
        void*  graphPtr   = graph.get();

        static const RE::BSFixedString kSpeed("Speed");
        // RE (reference_locomotion_graph_vars): "CurrentSpeed" is NOT a real
        // graph var - it's absent from the engine's locomotion var table
        // (0x144b34xxx), so the old kCurSpeed write was a silent no-op. The
        // smoothed-speed vars the blend tree actually samples for gait selection
        // are SpeedSampled + SpeedSmoothed. Feed both = effectiveSpeed so the
        // gait blend tracks the real ground speed (fixes "slow walk plays a jog").
        static const RE::BSFixedString kSpeedSampled("SpeedSampled");
        static const RE::BSFixedString kSpeedSmoothed("SpeedSmoothed");
        static const RE::BSFixedString kDirection("Direction");
        static const RE::BSFixedString kIdleLoco("iSyncIdleLocomotion");
        static const RE::BSFixedString kSprint("iSyncSprintState");
        static const RE::BSFixedString kIState("iState");
        static const RE::BSFixedString kRelaxReadySneak("iSyncRelaxReadySneak");
        static const RE::BSFixedString kRelaxedReadyAlert("iSyncRelaxedReadyAlert");
        static const RE::BSFixedString kGravity("iSyncGravity");
        static const RE::BSFixedString kStance("iSyncActorStance");
        static const RE::BSFixedString kWeaponDraw("iSyncWeaponDrawState");
        static const RE::BSFixedString kHeadTrack("bHeadTrackingActive");
        static const RE::BSFixedString kNoHeadTrack("bNoHeadTrack");
        static const RE::BSFixedString kGunActions("iSyncGunActions");
        static const RE::BSFixedString kGunDown("iSyncGunDown");
        static const RE::BSFixedString kSwimState("iSyncSwimState");
        static const RE::BSFixedString kBleedout("iSyncBleedout");
        static const RE::BSFixedString kPartialBody("iSyncPartialBody");
        static const RE::BSFixedString kSighted("iSyncSighted");
        static const RE::BSFixedString kAimActive("bAimActive");   // P3 (FIRE_RE2)
        // foot-IK (anti-slide) + idle-rotation pin vars.
        static const RE::BSFixedString kFootIK("bHumanoidFootIKEnable");
        static const RE::BSFixedString kDisableRot("DisableAllowRotation");
        static const RE::BSFixedString kCodeRot("IsUsingCodeDrivenRotation");
        static const RE::BSFixedString kTurnState("iSyncTurnState");
        static const RE::BSFixedString kTurnDelta("TurnDelta");
        static const RE::BSFixedString kTurnDeltaSm("TurnDeltaSmoothed");
        static const RE::BSFixedString kTween("TweenRotation");
        static const RE::BSFixedString kHeadingSpd("fHeadingSpeed");

        SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kSpeed,         effectiveSpeed);
        SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kSpeedSampled,  effectiveSpeed);
        SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kSpeedSmoothed, effectiveSpeed);
        // effectiveDir is radians (atan2(velocity) - yaw); the graph's Direction is a
        // fraction of a turn in [0, 1) (Animation/DirectionMap.h). Writing radians
        // raw put sideways NPCs at ~1.57 = outside the graph's range.
        SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kDirection,
                              Pulsar::Animation::RadiansToDirection(effectiveDir));

        // GRAPH-DIAG finding (player reference): the always-animating local player
        // reads iSyncIdleLocomotion = 1 while MOVING (Speed 5-7.7) and 0 while
        // standing - i.e. 1 = in-locomotion, NOT "1 = idle" as the old comment
        // claimed. We were writing it INVERTED (0 while moving), so the NPC graph
        // saw "not in locomotion" despite Speed>0 → stuck in idle / tippel-steps.
        // Match the player: 1 = moving.
        const int32_t idleLoco = (effectiveSpeed > 0.1f) ? 1 : 0;
        const int32_t sprint   = (moveFlags & 0x2) ? 1 : 0;

        const bool idle = (effectiveSpeed <= 0.1f);

        // Movement + always-safe vars (drive the locomotion blend in any state).
        SafeSetGraphIntImpl(graphPtr, setIntFn, &kIdleLoco,    idleLoco);
        SafeSetGraphIntImpl(graphPtr, setIntFn, &kSprint,      sprint);
        SafeSetGraphIntImpl(graphPtr, setIntFn, &kWeaponDraw,  weaponDrawn ? 1 : 0);
        SafeSetGraphIntImpl(graphPtr, setIntFn, &kSwimState,   0);
        SafeSetGraphIntImpl(graphPtr, setIntFn, &kGravity,     0);
        SafeSetGraphIntImpl(graphPtr, setIntFn, &kPartialBody, 0);
        SafeSetGraphIntImpl(graphPtr, setIntFn, &kBleedout,    0);

        // anti-foot-slide: keep foot IK on so the planted foot sticks to the
        // ground while the body translates at our lerped speed (RE: bHumanoidFootIKEnable).
        SafeSetGraphIntImpl(graphPtr, setIntFn, &kFootIK, 1);

        // idle-spin fix (Sarah rotated in place on the client). When standing
        // still, pin facing: tell the graph rotation is code-driven + disallowed,
        // zero the turn deltas, and stop head-track search-turning. The per-frame
        // rotZ pin in TickLerp then holds her heading. While moving, leave turning
        // and head-track enabled so walk turns + look-at still play.
        if (idle) {
            SafeSetGraphIntImpl  (graphPtr, setIntFn,   &kDisableRot,  1);
            SafeSetGraphIntImpl  (graphPtr, setIntFn,   &kCodeRot,     1);
            SafeSetGraphIntImpl  (graphPtr, setIntFn,   &kTurnState,   0);
            SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kTurnDelta,   0.0f);
            SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kTurnDeltaSm, 0.0f);
            SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kTween,       0.0f);
            SafeSetGraphFloatImpl(graphPtr, setFloatFn, &kHeadingSpd,  0.0f);
            SafeSetGraphIntImpl  (graphPtr, setIntFn,   &kHeadTrack,   0);
            SafeSetGraphIntImpl  (graphPtr, setIntFn,   &kNoHeadTrack, 1);
        } else {
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kDisableRot,  0);
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kCodeRot,     0);
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kHeadTrack,   1);
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kNoHeadTrack, 0);
        }

        // P1: the combat-state vars must NOT be forced while the NPC is in
        // combat (weapon drawn). Forcing iState=10(normal)/relax/stance/gunDown/
        // gunActions/sighted=0 every tick - post-origUpdate, so it WINS - resets
        // the NPC out of combat-ready stance and suppresses ALL aim/strafe/fire
        // anim. In combat, let the engine's combat controller own these. Out of
        // combat, force the idle/normal set so a walking civilian still leaves
        // idle-limbo (the reason this writer exists).
        if (!weaponDrawn) {
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kIState,            10);
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kRelaxReadySneak,   0);
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kRelaxedReadyAlert, 0);
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kStance,            0);
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kGunActions,        0);
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kGunDown,           0);
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kSighted,           0);
        } else {
            // P3 fire-sync: the NPC's AI is blocked, so the engine won't drive
            // its combat stance or fire - mirror the host's sampled state (moveFlags
            // bit 0x8 = combat-aim, bit 0x4 = firing this packet, set host-side from
            // iSyncGunActions/sighted). Hold the combat-ready aim stance + pulse the
            // gun action so the client actually SEES the enemy aim and shoot.
            const bool inCombat = (moveFlags & 0x8) != 0;
            if (inCombat) {
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kIState,    17);  // observed combat-ready
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kSighted,   1);
                SafeSetGraphIntImpl(graphPtr, setIntFn, &kAimActive, 1);   // hold weapon-up aim pose
            }
            // The actual fire is triggered by the "WeaponFire" anim EVENT
            // (NotifyFireEvent), not by writing iSyncGunActions (whose pulse value we
            // can't replicate) - keep it cleared so no stale half-fire state lingers.
            SafeSetGraphIntImpl(graphPtr, setIntFn, &kGunActions, 0);
        }

        // NOTE: bGraphDriven write REMOVED. Setting it to 1 appeared to
        // FREEZE the anim state machine instead of enabling external drive
        // (semantics of this var from T41 RE were unverified - likely it
        // signals "don't run AI-graph-driver, wait for MovementController
        // agent" which we don't provide). Revert: let engine's own graph
        // drive path tick normally.

        // Diagnostic readback: did our Speed write actually land?
        {
            static std::unordered_map<uint32_t, uint64_t> s_lastRB;
            static std::mutex                             s_rbMutex;
            const uint64_t now = NowMs();
            bool doRB = false;
            {
                std::lock_guard lk(s_rbMutex);
                auto& last = s_lastRB[actor->formID];
                if (now - last >= 3000) { last = now; doRB = true; }
            }
            if (doRB) {
                const float rbSpeed = SafeReadGraphFloat(actor, kSpeed);
                const float rbDir   = SafeReadGraphFloat(actor, kDirection);
                PLOG_INFO("[PULSAR] LOCO-RB actor=0x{:08X} wrote Speed={:.2f} Dir={:.2f} -> read Speed={:.2f} Dir={:.2f}"sv,
                          static_cast<uint32_t>(actor->formID),
                          effectiveSpeed, effectiveDir, rbSpeed, rbDir);
            }
        }
    }

    bool IsTracked(uint32_t formID)
    {
        std::lock_guard lk(g_targetsMutex);
        auto it = g_targets.find(formID);
        if (it == g_targets.end()) return false;
        // Same 5 s staleness rule as TickLerp's auto-GC. Don't return true
        // for an entry we're about to throw away.
        return (NowMs() - it->second.updatedMs) <= 5000;
    }

    // Forward decl - definition below (plain-C, uses __try so can't inline here).
    static void SetAIDisabled(RE::Actor* actor, bool disabled);

    // P3 fire-sync: forward decl - sends the "WeaponFire" anim event (deep-RE
    // found it interned alongside iSyncGunActions) so the AI-blocked client clone
    // plays the muzzle/fire anim. Throttled. Defined after SafeNotifyGraph below.
    static void NotifyFireEvent(RE::Actor* actor, uint8_t moveFlags);

    // ====================================================================
    // KANDIDAT 2 (de-risk, opt-in): IMovementDirectControl for NPCs
    // --------------------------------------------------------------------
    // A remote NPC's visible mesh is driven by Havok's character controller,
    // not by data.location. The data-lerp leaves the mesh parked and the
    // SetPosition snap can only teleport it (→ the 150 ms jitter). The player
    // clone moves smoothly because it drives the engine's OWN locomotion via
    // IMovementDirectControl (RemotePlayerManager.cpp). Replicate that per NPC:
    //   movCtrl  = *(Actor + 0x240)
    //   dispatch = *(movCtrl + 0x48)
    //   ctrl     = InterfaceLookup(dispatch, "IMovementDirectControl")
    //   vt[1] = Enable()   vt[5] = SetTargetPosition(NiPoint3*, speed)
    // Then feed the synced target every tick and let the engine walk it.
    // Gated behind PULSAR_NPC_DC=1 → opt-in A/B with an instant revert.
    using NpcIfaceLookup_t = void*(*)(void* dispatch, void* name);
    static REL::Relocation<NpcIfaceLookup_t> s_npcIfaceLookup{ REL::ID(49356) };
    using NpcDCEnable_t   = void(__fastcall*)(void* self);
    using NpcDCSetSpeed_t = void(__fastcall*)(void* self, float speed);

    struct NpcDirectCtrl {
        void*    ctrl      = nullptr;
        void**   vt        = nullptr;
        bool     enabled   = false;
        uint64_t nextTryMs = 0;
    };
    static std::unordered_map<uint32_t, NpcDirectCtrl> g_npcDC;
    static std::mutex                                  g_npcDCMutex;

    // Toggle, set from PULSAR.ini (npcDirectControl) via SetNpcDirectControl, or
    // overridden by env PULSAR_NPC_DC=1. The ini path is reliable (read from the
    // DLL's own dir); the env var can be swallowed by the Steam launch environment.
    static std::atomic<bool> g_npcDCFlag{ false };

    static bool NpcDCExperimentOn()
    {
        static const bool envOn = [] {
            char* v = nullptr; size_t n = 0;
            bool r = (_dupenv_s(&v, &n, "PULSAR_NPC_DC") == 0 && v && v[0] == '1');
            if (v) free(v);
            return r;
        }();
        return envOn || g_npcDCFlag.load(std::memory_order_relaxed);
    }

    void SetNpcDirectControl(bool on)
    {
        g_npcDCFlag.store(on, std::memory_order_relaxed);
        PLOG_INFO("[PULSAR] [DC] NPC DirectControl experiment {}"sv,
                  on ? "ENABLED (ini)" : "off");
    }

    // A/B diag toggle (PULSAR.ini npcNoImc=1, or env PULSAR_NPC_NO_IMC=1): skip
    // IMovementDirectControl (Enable + SetSpeed) entirely and drive the NPC purely
    // via graph-vars + mesh-move. Isolates whether IMC's SetSpeed is the "walk
    // turns into jog after the settle window" culprit, in-game, with no rebuild.
    static std::atomic<bool> g_npcNoImcFlag{ false };

    void SetNpcNoImc(bool on)
    {
        g_npcNoImcFlag.store(on, std::memory_order_relaxed);
        PLOG_INFO("[PULSAR] [DC] NPC NO-IMC (graph+mesh only) {}"sv,
                  on ? "ENABLED (ini)" : "off");
    }

    static bool NpcNoImc()
    {
        static const bool envOn = [] {
            char* v = nullptr; size_t n = 0;
            bool r = (_dupenv_s(&v, &n, "PULSAR_NPC_NO_IMC") == 0 && v && v[0] == '1');
            if (v) free(v);
            return r;
        }();
        return envOn || g_npcNoImcFlag.load(std::memory_order_relaxed);
    }

    // SEH-only helpers (no C++ unwinding object may share a frame with __try).
    static void* SafeDerefPtr(void* base, size_t off)
    {
        __try { return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    static void* SafeNpcIfaceLookup(void* dispatch, void* name)
    {
        __try { return s_npcIfaceLookup(dispatch, name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    static bool SafeNpcDCEnable(void* self, void* fn)
    {
        __try { reinterpret_cast<NpcDCEnable_t>(fn)(self); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool SafeNpcDCSetSpeed(void* self, void* fn, float speed)
    {
        __try { reinterpret_cast<NpcDCSetSpeed_t>(fn)(self, speed); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Probe + enable DirectControl once per NPC (2 s retry on failure). No inline
    // __try here (lock_guard present) - every faulting read goes via a helper.
    static bool EnsureNpcDirectControl(RE::Actor* actor, NpcDirectCtrl& out)
    {
        const uint32_t id = actor->formID;
        std::lock_guard lk(g_npcDCMutex);
        auto& dc = g_npcDC[id];
        if (dc.enabled) { out = dc; return true; }
        const uint64_t now = NowMs();
        if (now < dc.nextTryMs) return false;
        dc.nextTryMs = now + 2000;

        void* movCtrl = SafeDerefPtr(actor, 0x240);
        if (!movCtrl) return false;
        void* dispatch = SafeDerefPtr(movCtrl, 0x48);
        if (!dispatch) return false;
        RE::BSFixedString dcName("IMovementDirectControl");
        void* ctrl = SafeNpcIfaceLookup(dispatch, &dcName);
        if (!ctrl) return false;
        void** vt = reinterpret_cast<void**>(SafeDerefPtr(ctrl, 0));
        if (!vt) return false;
        if (!SafeNpcDCEnable(ctrl, vt[1])) {
            PLOG_WARN("[PULSAR] [DC] NPC 0x{:08X} Enable() faulted"sv, id);
            return false;
        }
        dc.ctrl = ctrl; dc.vt = vt; dc.enabled = true;
        out = dc;
        PLOG_INFO("[PULSAR] [DC] NPC 0x{:08X} DirectControl ENABLED"sv, id);
        return true;
    }

    // Engage DirectControl + drive the walk/run ANIM via SetSpeed (vt[3] - the
    // clone's primitive). Position is applied separately (per-frame lerped
    // SetPosition); SetTargetPosition(vt[5]) snapped the actor onto the target
    // every tick = the visible teleport. false → faulted; caller drops it and
    // the legacy path resumes next tick.
    static bool DriveNpcDirectControl(RE::Actor* actor, float speed)
    {
        NpcDirectCtrl dc;
        if (!EnsureNpcDirectControl(actor, dc)) return false;
        if (!SafeNpcDCSetSpeed(dc.ctrl, dc.vt[3], speed)) {
            std::lock_guard lk(g_npcDCMutex);
            g_npcDC.erase(actor->formID);
            return false;
        }
        return true;
    }

    // P1: release a DirectControl-puppet NPC - drop our DC state AND give the
    // engine AI back (unblock EvaluatePackage/PerformAction). Call on death /
    // untrack so corpses can ragdoll and departed NPCs aren't left frozen.
    // SEH-safe raw clear of the IMovementDirectControl "active" flag. Enable
    // (vt[1]) does `mov byte [ctrl+0x3d], 1` to mark itself active; clearing it
    // hands physics control of the actor back to the engine so a DEAD body can
    // ragdoll/play its death anim (DirectControl otherwise pins it upright = the
    // "dead but standing / no ragdoll" bug). Disasm-confirmed offset (1.16.236).
    static void SafeNpcDCClearActive(void* ctrl)
    {
        __try {
            if (ctrl) *(reinterpret_cast<uint8_t*>(ctrl) + 0x3D) = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void ReleaseNpcDc(uint32_t formID)
    {
        void* ctrl = nullptr;
        {
            std::lock_guard lk(g_npcDCMutex);
            auto it = g_npcDC.find(formID);
            if (it != g_npcDC.end()) { ctrl = it->second.ctrl; g_npcDC.erase(it); }
        }
        SafeNpcDCClearActive(ctrl);   // hand control back → dead body can ragdoll
        MarkNpcAiBlocked(formID, false);
    }

    // Public wrapper (declared in LocomotionApply.h). Lets the NPCSync kill path
    // unpin a puppet BEFORE the engine kill so the corpse ragdolls instead of dying
    // upright. Safe to call regardless of npcDirectControl mode: no cached entry →
    // SafeNpcDCClearActive(nullptr) is a no-op, AI-unblock is harmless.
    void ReleaseNpcDirectControl(uint32_t formID)
    {
        ReleaseNpcDc(formID);
    }

    void TickLerp(RE::Actor* actor, float factor)
    {
        if (!actor) return;

        // V2-Step-1c.1: never drive a DEAD actor's position. NPCSync pins the
        // corpse to the host's last position every frame (target ages out only
        // after 5 s), holding it upright so the ragdoll/death anim can't play -
        // symptom: host-killed enemy is "dead but standing" on the client until a
        // local hit perturbs the physics. Drop its lerp target and let it fall.
        // #ragdoll - also covers the kill-pending async-death gap (IsDead not true yet).
        if (actor->IsDead() || IsNpcKillPending(actor->formID)) {
            {
                std::lock_guard lk(g_targetsMutex);
                g_targets.erase(actor->formID);
            }
            if (NpcDCExperimentOn()) ReleaseNpcDc(actor->formID);  // resume AI → corpse can ragdoll
            return;
        }

        // V36 (2026-06-30): scripted-scene suspension. If this NPC is currently
        // in a quest scene (it recently spoke a scene line near a player), hand
        // DirectControl back to the engine and skip our position/graph apply so
        // its LOCAL scene plays out - fixed path, scene animations, gestures and
        // dialogue. Both machines run the same deterministic script, so they stay
        // in sync without the puppet. g_targets keeps updating in the background,
        // so the moment the scene window lapses TickLerp resumes normal sync.
        // Without this the host->client DirectControl froze the client's NPCs in
        // place and stalled the scene after the opening line.
        if (IsNpcInScene(actor->formID)) {
            if (NpcDCExperimentOn()) ReleaseNpcDc(actor->formID);
            return;
        }

        // REVERTED 2026-04-24: setting AIProcess+0xE7 to 1 did bail MC::Update
        // as intended, but ALSO made the engine treat the actor as disabled /
        // needing-cleanup → all tracked NPCs AND the player clone vanished
        // from the client's scene. The flag is consumed by multiple subsystems,
        // not just MC::Update. Keep SetAIDisabled defined (see below) for
        // future experiments with different byte values, but do not call it
        // indiscriminately here.
        // SetAIDisabled(actor, true);

        RE::NiPoint3 predicted;
        float        tgtRotZ;
        float        vx = 0.0f, vy = 0.0f;
        float        graphSpeed = 0.0f;
        uint8_t      moveFlags = 0;
        bool         weaponDrawn = false;
        {
            std::lock_guard lk(g_targetsMutex);
            auto it = g_targets.find(actor->formID);
            if (it == g_targets.end()) return;
            const uint64_t now = NowMs();
            const uint64_t age = now - it->second.updatedMs;
            if (age > 5000) {
                g_targets.erase(it);
                if (NpcDCExperimentOn()) ReleaseNpcDc(actor->formID);  // untracked → give AI back
                return;
            }
            // Prediction (dead reckoning) DISABLED 2026-04-23 - it caused
            // visible overshoot-then-snap: velocity smoothing lagged real
            // motion, so predicted pos was consistently ahead of the next
            // real packet pos. Net visual = NPC walks forward then teleports
            // back a bit when the packet arrives. Without prediction the
            // client lerps toward raw packet pos; net result is the actor
            // trails ~1 packet behind host (~33 ms = invisibly small lag),
            // no overshoot.
            (void)age;
            predicted.x = it->second.pos.x;
            predicted.y = it->second.pos.y;
            predicted.z = it->second.pos.z;
            tgtRotZ     = it->second.rotZ;
            vx          = it->second.velocity.x;
            vy          = it->second.velocity.y;
            graphSpeed  = it->second.graphSpeed;
            moveFlags   = it->second.moveFlags;
            weaponDrawn = it->second.weaponDrawn;
        }

        // (opt-in PULSAR.ini npcDirectControl=1): glide the remote NPC's
        // MESH smoothly. Root-cause: Havok holds the mesh, so the data.location
        // lerp never moves it and the legacy gated SetPosition only teleports.
        if (NpcDCExperimentOn()) {
            // crash-fix: skip the whole direct-control drive (SetSpeed +
            // mesh move + graph writes + diag reads) until the graph has settled.
            // Driving a mid-construction graph = pure-virtual-call = hard crash.
            if (!NpcGraphSafeToDrive(actor)) return;
            const float dcSpeed = std::sqrt(vx * vx + vy * vy);

            // P3 combat-live: when enabled, a weaponDrawn NPC runs its OWN
            // engine combat AI (aim + fire + damage the local player) instead of
            // being puppeted. We stop puppeting it (no DirectControl, no graph-var
            // forcing, no tight pin) and only host-correct position on a LARGE
            // drift, letting the AI position it locally. The EvaluatePackage/
            // PerformAction hooks pass its AI through via IsNpcCombatLive.
            // shared-aggro: combat-live used to track the host weaponDrawn
            // flag 1:1, but that flag flickers (holster between bursts, packet
            // gaps), and every drop snapped the NPC back to a hard-pinned puppet
            // mid-fight → the client's local combat AI kept losing the player
            // ("selten/inkonsistent" aggro). Make it STICKY: once the host reports
            // weaponDrawn, hold AI-live for kCombatLiveHoldMs so brief drops don't
            // interrupt the local fight. Game-thread only (TickLerp) → no lock.
            static std::unordered_map<uint32_t, uint64_t> s_lastArmedMs;
            constexpr uint64_t kCombatLiveHoldMs = 8000;
            const uint64_t nowMsCL = GetTickCount64();
            if (weaponDrawn) s_lastArmedMs[actor->formID] = nowMsCL;
            bool recentlyArmed = false;
            if (auto itA = s_lastArmedMs.find(actor->formID); itA != s_lastArmedMs.end()) {
                if (nowMsCL - itA->second < kCombatLiveHoldMs) recentlyArmed = true;
                else s_lastArmedMs.erase(itA);   // expired → bound the map
            }
            const bool combatLive = NpcCombatAiLiveOn() && (weaponDrawn || recentlyArmed);
            const bool heldOnly   = recentlyArmed && !weaponDrawn;  // kept alive by timer despite drop
            MarkNpcCombatLive(actor->formID, combatLive);
            // diag: log combat-live ON/OFF transitions per NPC. heldOnly=1
            // means the sticky timer is bridging a weaponDrawn drop (= the flicker
            // we are trying to ride out - if this fires a lot, flicker was the cause).
            {
                static std::unordered_map<uint32_t, bool> s_clState;
                bool& prev = s_clState[actor->formID];
                if (prev != combatLive) {
                    prev = combatLive;
                    PLOG_INFO("[AGGRO] NPC 0x{:08X} combat-live {} (weaponDrawn={} heldOnly={})"sv,
                              actor->formID, combatLive ? "ON" : "OFF",
                              weaponDrawn ? 1 : 0, heldOnly ? 1 : 0);
                }
            }
            // diag (throttled): also log while HELD past a drop, so we can see
            // the flicker frequency even without an ON/OFF transition.
            if (heldOnly) {
                static std::unordered_map<uint32_t, uint64_t> s_lastHeldLog;
                auto& last = s_lastHeldLog[actor->formID];
                if (nowMsCL - last > 1000) {
                    last = nowMsCL;
                    PLOG_INFO("[AGGRO] NPC 0x{:08X} holding combat-live through weaponDrawn=0 drop"sv,
                              actor->formID);
                }
            }
            if (combatLive) {
                MarkNpcAiBlocked(actor->formID, false);   // AI owns anim + fire + facing
                // v2: REMOVED the per-frame 0.2 soft position-pull. It wrote the
                // NPC position toward the host's position EVERY frame, fighting the
                // local combat AI's own locomotion → the NPC "spun in circles" and
                // never closed on / faced the client (combat-live was confirmed stable
                // ON in the v1b logs, so flicker was NOT the cause - this pull was).
                // Let the AI fully own position during combat; only hard-catch-up on
                // gross desync (cell change / teleport). Positional divergence vs the
                // host during a fight is the accepted tradeoff.
                float cur[3] = { 0, 0, 0 };
                SafeReadDataLocation(actor, cur);
                const float ddx = predicted.x - cur[0];
                const float ddy = predicted.y - cur[1];
                const float ddz = predicted.z - cur[2];
                const float gap2 = ddx*ddx + ddy*ddy + ddz*ddz;
                // v5 drift control - thread the needle between v2/v3/v4:
                //   v2 (per-frame f=0.2)    → strong → fought the AI → circling.
                //   v3 (no correction)      → drifted 15-18 units → "too far apart".
                //   v4 (throttled 50% jump) → discrete → looked like a teleport.
                // v5 = a SMOOTH, WEAK, per-frame pull. Continuous (no discrete jump →
                // no teleport) but ~7x weaker than v2 (so it biases the position
                // toward the host without redirecting the AI's path → no circling).
                // Equilibrium drift ≈ AI-speed / (60·f). tuned it tighter
                // (fxy=0.05, deadband 1u) once the tamed AI stopped repositioning →
                // ~1-2 units. Small deadband so a near-aligned NPC isn't micro-jittered.
                if (gap2 > 30.0f * 30.0f) {
                    // gross desync / cell-change → one hard catch-up
                    SafeSetPositionSnap(actor,
                        RE::NiPoint3(predicted.x, predicted.y, predicted.z), tgtRotZ);
                } else if (gap2 > 1.0f * 1.0f) {
                    // fine-tune: tighter deadband (1u, was 2u) + a slightly
                    // stronger horizontal bias. Safe now that tamed the AI so it
                    // barely repositions → almost nothing to fight → no circling.
                    float curRotZ = 0.0f;
                    SafeReadRotZ(actor, &curRotZ);         // keep the AI's aim facing
                    const float fxy = 0.05f;               // horizontal pull (was 0.03)
                    // Vertical stays GENTLE on purpose (NOT stricter): the client's
                    // local ground Z is the correct one for the client's world; a hard
                    // pull toward the host's Z would float/sink the NPC off the local
                    // floor. Z drift is invisible per-player anyway (each sees its NPC
                    // on its own ground).
                    const float fz  = 0.03f;
                    const float sx = cur[0] + ddx * fxy;
                    const float sy = cur[1] + ddy * fxy;
                    const float sz = cur[2] + ddz * fz;
                    SafeNpcWriteDataLoc(actor, sx, sy, sz, curRotZ);
                    SafeNpcHavokBodySetPos(actor, sx, sy, sz);
                }
                return;   // AI owns anim/fire; we only weakly bias position on drift
            }
            // jog fix: feed IMovementDirectControl SetSpeed the host's
            // authoritative graph "Speed" (smooth, in the gait-ref unit - the exact
            // value the host NPC's engine used to pick its gait), exactly like the
            // player clone does. The Δpos velocity (dcSpeed) spikes on packet gaps,
            // and IMC turned those spikes into a jog. Fall back to dcSpeed only when
            // graphSpeed is 0 (low-AI NPCs that don't populate the var).
            const float imcSpeed = (graphSpeed > 0.05f) ? graphSpeed : dcSpeed;
            // P1 combo (the clone's full puppet recipe, never tried together
            // on NPCs): (a) drive the engine's locomotion via IMovementDirectControl
            // SetSpeed → plays the walk/run anim WITHOUT the AI; (b) block this NPC's
            // AI (EvaluatePackage + PerformAction, static cells) so it stops fighting
            // the host-driven sync (no companion-follow, no independent combat/aim).
            // Only block the AI if DirectControl actually engaged (else we'd kill the
            // anim with nothing driving it).
            // A/B: NpcNoImc() skips IMovementDirectControl but keeps AI blocked, so
            // the NPC is driven purely by graph-vars + mesh-move (isolates whether
            // IMC's SetSpeed is the jog source). Otherwise normal combo.
            const bool noImc = NpcNoImc();
            const bool dcOk  = noImc ? true : DriveNpcDirectControl(actor, imcSpeed);
            MarkNpcAiBlocked(actor->formID, dcOk);
            {
                // Move the mesh a fraction toward the target EVERY frame - this
                // re-asserts all 5 layers incl. the Havok body (drift ~0.02 m,
                // smooth). We deliberately do NOT enable IMovementDirectControl:
                // the de-risk proved it kills the walk anim (NPCs glided but
                // T-posed) because the NPC already has a full graph-var
                // (iState/Speed/Direction/sprint) + NotifyMovementEvent anim
                // pipeline, and DirectControl takes the actor into a direct-control
                // mode that bypasses it. So move the mesh here and leave the
                // engine's graph-driven anim alone. Skip when on-target (a no-move
                // SetPosition reads as a teleport to Havok = jitter).
                float cur[3] = { 0, 0, 0 };
                SafeReadDataLocation(actor, cur);
                const float dpx = predicted.x - cur[0];
                const float dpy = predicted.y - cur[1];
                const float dpz = predicted.z - cur[2];
                const float gap2 = dpx * dpx + dpy * dpy + dpz * dpz;
                // hitch fix: the rescue must ALSO fire when the MESH (Havok
                // body world pos = data3D world.translate @ +0xB0) has drifted from
                // target even though data.location is close - that mesh-only drift
                // (observed up to ~33 m) is the visible "hitch" the old data-gated
                // rescue missed. Only pay the locked node read when the data gap is
                // small (else we rescue on the data gap anyway).
                float meshGap2 = 0.0f;
                if (gap2 <= 5.0f * 5.0f) {
                    float mw[3] = { 0, 0, 0 };
                    bool  mok = false;
                    {
                        auto guard = actor->loadedData.LockRead();
                        const auto* data = *guard;
                        if (data && data->data3D) { SafeReadNodeWorld(data->data3D.get(), mw); mok = true; }
                    }
                    if (mok) {
                        const float mdx = predicted.x - mw[0];
                        const float mdy = predicted.y - mw[1];
                        const float mdz = predicted.z - mw[2];
                        meshGap2 = mdx * mdx + mdy * mdy + mdz * mdz;
                    }
                }
                // rotation source = SPEED, not position. The gap2 branches
                // below decide POSITION handling; an on-target but WALKING NPC still
                // falls into the "idle" (else) branch because its mesh is well-synced.
                // Picking the facing there by position would hold (deadband) the
                // heading while the body moves → curYaw stays fixed → Direction =
                // atan2(vel) - curYaw goes large → the engine plays a fixed-stare
                // strafe/jog instead of a forward walk (the "looks one way + jogs"
                // bug). So: follow the host heading LIVE while moving (body turns to
                // face its path, Direction≈0, clean forward walk), deadband ONLY when
                // truly idle (the standing-guard anti-jitter).
                // Moving-decision for the facing source uses the host's clean
                // graphSpeed (≈0 at true idle for high-AI NPCs) OR a HIGH Δpos
                // threshold for low-AI NPCs. The old dcSpeed>0.15 gate flipped to
                // "moving" on idle Δpos network-noise → the left-right idle jitter
                // came back. graphSpeed is host-authoritative and noise-free.
                const bool rotMoving = (graphSpeed > 0.1f) || (dcSpeed > 0.4f);
                float pinRotZ;
                if (rotMoving) {
                    IdleRotLatchTrack(actor->formID, tgtRotZ);
                    pinRotZ = tgtRotZ;
                } else {
                    pinRotZ = IdleRotLatchPin(actor->formID, tgtRotZ);
                }
                const float kMeshRescue = 3.0f;   // m - mesh-only drift snap threshold (tunable)
                if (gap2 > 5.0f * 5.0f || meshGap2 > kMeshRescue * kMeshRescue) {
                    // Rescue snap: a huge gap (fresh-track / cell-change / host
                    // teleport / mesh-only drift) is the "hitch" - the per-frame
                    // lerp would crawl across it visibly. One hard SetPosition
                    // catches up in a single frame; the 1-frame anim reset is fine
                    // for a rare large jump.
                    SafeSetPositionSnap(actor,
                        RE::NiPoint3(predicted.x, predicted.y, predicted.z), pinRotZ);
                } else if (gap2 > 0.02f * 0.02f) {
                    const float f = 0.5f;   // per-frame convergence (60 Hz → smooth)
                    const float sx = cur[0] + dpx * f;
                    const float sy = cur[1] + dpy * f;
                    const float sz = cur[2] + dpz * f;
                    // Move the MESH via the Havok CC body + data.location (clone
                    // proc-cell technique) instead of Actor::SetPosition, which
                    // resets the anim graph every frame. The Havok body drives the
                    // collision object → the mesh follows, and the anim keeps playing.
                    SafeNpcWriteDataLoc(actor, sx, sy, sz, pinRotZ);
                    SafeNpcHavokBodySetPos(actor, sx, sy, sz);
                } else {
                    // On-target → pin facing (deadbanded above when idle) so the
                    // local head-track / turn drive can't spin the actor in place.
                    // Raw data.location+angle write (no Havok teleport / anim reset).
                    SafeNpcWriteDataLoc(actor, cur[0], cur[1], cur[2], pinRotZ);
                }
                // Drive the locomotion ANIM from the actual ground speed, applied
                // here (post-origUpdate) so it WINS over the local AI's idle write.
                // The legacy TickLerp tail did this at ~line 1032, which our early
                // return below skips - without it the anim sits at idle/slow while
                // the body glides = the "super-slow walk / floating" the NPCs showed.
                // speedMag = derived ground velocity, clamped to the anim blend
                // range (walk ~1.5 / run ~3.5 / sprint ~5.5).
                {
                    float animSpeed = dcSpeed;            // sqrt(vx*vx + vy*vy)
                    if (animSpeed > 6.0f) animSpeed = 6.0f;
                    float animDir = 0.0f;
                    if (animSpeed > 0.1f) {
                        float curYaw = 0.0f;
                        SafeReadRotZ(actor, &curYaw);
                        animDir = std::atan2(vx, vy) - curYaw;
                        while (animDir >  3.14159265f) animDir -= 6.28318531f;
                        while (animDir < -3.14159265f) animDir += 6.28318531f;
                    }
                    WriteGraphVarsImpl(actor, animSpeed, animDir, moveFlags, weaponDrawn);
                    NotifyMovementEvent(actor, animSpeed, moveFlags);
                    NotifyFireEvent(actor, moveFlags);   // P3: mirror host weapon fire
                }
                // Periodic mesh-vs-target dump so the de-risk run shows smoothness.
                static std::unordered_map<uint32_t, uint64_t> s_dcDump;
                static std::mutex                              s_dcDumpMutex;
                const uint64_t nowDc = NowMs();
                bool doDump = false;
                {
                    std::lock_guard lk(s_dcDumpMutex);
                    auto& last = s_dcDump[actor->formID];
                    if (nowDc - last >= 5000) { last = nowDc; doDump = true; }
                }
                if (doDump) {
                    float mesh[3] = { 0, 0, 0 };
                    bool  ok = false;
                    {
                        auto guard = actor->loadedData.LockRead();
                        const auto* data = *guard;
                        if (data && data->data3D) { SafeReadNodeWorld(data->data3D.get(), mesh); ok = true; }
                    }
                    if (ok) {
                        const float md = std::sqrt(
                            (mesh[0] - predicted.x) * (mesh[0] - predicted.x) +
                            (mesh[1] - predicted.y) * (mesh[1] - predicted.y) +
                            (mesh[2] - predicted.z) * (mesh[2] - predicted.z));
                        PLOG_INFO("[PULSAR] [DC] actor=0x{:08X} tgt=({:.1f},{:.1f},{:.1f}) spd={:.2f} meshDrift={:.2f}m"sv,
                                  static_cast<uint32_t>(actor->formID),
                                  predicted.x, predicted.y, predicted.z, dcSpeed, md);
                    }
                    // GRAPH-DIAG: which graph are we writing + what state is it
                    // REALLY in? (We know Speed lands but the walk never commits.)
                    {
                        RE::BSTSmartPointer<RE::BSAnimationGraphManager> gm;
                        if (actor->GetAnimationGraphManagerImpl(gm) && gm) {
                            static const RE::BSFixedString kSp("Speed");
                            static const RE::BSFixedString kSt("iState");
                            static const RE::BSFixedString kSi("iSyncSighted");
                            static const RE::BSFixedString kIl("iSyncIdleLocomotion");
                            static const RE::BSFixedString kWd("iSyncWeaponDrawState");
                            PLOG_INFO("[PULSAR] [GRAPH-DIAG] NPC 0x{:08X} graphs={} active={} dcOk={} noImc={} settleMs={} rb: Speed={:.2f} iState={} sighted={} idleLoco={} wpnDraw={} (we set weaponDrawn={})"sv,
                                      static_cast<uint32_t>(actor->formID),
                                      static_cast<int>(gm->graphs.size()),
                                      static_cast<int>(gm->activeGraph),
                                      dcOk ? 1 : 0, noImc ? 1 : 0,
                                      NpcSettleElapsed(actor->formID),
                                      SafeReadGraphFloat(actor, kSp), SafeReadGraphInt(actor, kSt),
                                      SafeReadGraphInt(actor, kSi), SafeReadGraphInt(actor, kIl),
                                      SafeReadGraphInt(actor, kWd), weaponDrawn ? 1 : 0);
                            // scale probe: the gait blend selects walk/jog/run
                            // by comparing the speed vars to these reference speeds.
                            // Logging both lets us see the unit/scale + which speed
                            // var the blend actually reads (jog-vs-walk root cause).
                            static const RE::BSFixedString kSSa("SpeedSampled");
                            static const RE::BSFixedString kSSm("SpeedSmoothed");
                            static const RE::BSFixedString kWalk("fWalkSpeed");
                            static const RE::BSFixedString kJog("fJogSpeed");
                            static const RE::BSFixedString kRun("fRunSpeed");
                            PLOG_INFO("[PULSAR] [GRAPH-DIAG] NPC 0x{:08X} gait: Sampled={:.2f} Smoothed={:.2f} | refs walk={:.2f} jog={:.2f} run={:.2f} (SetSpeed={:.2f} src={} dcSpeed={:.2f} graphSpeed={:.2f})"sv,
                                      static_cast<uint32_t>(actor->formID),
                                      SafeReadGraphFloat(actor, kSSa), SafeReadGraphFloat(actor, kSSm),
                                      SafeReadGraphFloat(actor, kWalk), SafeReadGraphFloat(actor, kJog),
                                      SafeReadGraphFloat(actor, kRun),
                                      imcSpeed, (graphSpeed > 0.05f) ? "graph" : "dpos",
                                      dcSpeed, graphSpeed);
                        }
                    }
                    // Player reference (always animates) - once per ~10 s.
                    {
                        static uint64_t s_lastPlayerDiag = 0;
                        if (nowDc - s_lastPlayerDiag >= 10000) {
                            s_lastPlayerDiag = nowDc;
                            auto* pc = RE::PlayerCharacter::GetSingleton();
                            RE::BSTSmartPointer<RE::BSAnimationGraphManager> pgm;
                            if (pc && pc->GetAnimationGraphManagerImpl(pgm) && pgm) {
                                static const RE::BSFixedString kSp("Speed");
                                static const RE::BSFixedString kSt("iState");
                                static const RE::BSFixedString kIl("iSyncIdleLocomotion");
                                static const RE::BSFixedString kSSa("SpeedSampled");
                                static const RE::BSFixedString kSSm("SpeedSmoothed");
                                static const RE::BSFixedString kWalk("fWalkSpeed");
                                static const RE::BSFixedString kJog("fJogSpeed");
                                static const RE::BSFixedString kRun("fRunSpeed");
                                PLOG_INFO("[PULSAR] [GRAPH-DIAG] PLAYER graphs={} active={} rb: Speed={:.2f} iState={} idleLoco={} | gait: Sampled={:.2f} Smoothed={:.2f} refs walk={:.2f} jog={:.2f} run={:.2f}"sv,
                                          static_cast<int>(pgm->graphs.size()),
                                          static_cast<int>(pgm->activeGraph),
                                          SafeReadGraphFloat(pc, kSp), SafeReadGraphInt(pc, kSt),
                                          SafeReadGraphInt(pc, kIl),
                                          SafeReadGraphFloat(pc, kSSa), SafeReadGraphFloat(pc, kSSm),
                                          SafeReadGraphFloat(pc, kWalk), SafeReadGraphFloat(pc, kJog),
                                          SafeReadGraphFloat(pc, kRun));
                            }
                        }
                    }
                }
                return;
            }
        }

        // Adaptive lerp: close small gaps gently so anim root-motion can
        // play, close big gaps aggressively so we don't fall so far behind
        // that SetPosition has to snap (which resets the anim state-machine
        // and interrupts the walk cycle - user's "walk anim starts then
        // gets interrupted by a forward jump" symptom).
        float curData[3] = { 0, 0, 0 };
        SafeReadDataLocation(actor, curData);
        const float cdx = curData[0] - predicted.x;
        const float cdy = curData[1] - predicted.y;
        const float currentDriftSq = cdx * cdx + cdy * cdy;
        float useFactor = factor;
        if (currentDriftSq > 9.0f) {
            // > 3 m gap - fresh-track or host jumped. Direct-write data.location
            // instead of ramping up factor (factor > ~0.6 visibly pops); writing
            // the full value avoids multi-tick catch-up and avoids SetPosition
            // which is what breaks the anim state.
            useFactor = 1.0f;
        } else if (currentDriftSq > 0.25f) {
            // 0.5-3 m gap - accelerate catch-up.
            useFactor = 0.5f;
        }
        SafeLerpApply(actor, predicted, tgtRotZ, useFactor);

        // --- Periodic SetPosition for mesh+rotation+Havok consistency ------
        // We tried force-writing NiAVObject.world.translate every tick -
        // LOCO-DUMP confirmed our writes land (drift=0.00m immediately after)
        // but Havok's own thread overwrites mesh.world back from collisionObject
        // within 1-2 frames, AND mesh.world.rotation is never touched so the
        // NPC looks fixed in one direction (Sarah = stuck looking right).
        //
        // Only Actor::SetPosition updates all five layers atomically (data,
        // mesh.translate, mesh.rotate, Havok body, cell hashmap). Call it on
        // a 150 ms throttle - fast enough that between calls the data-lerp +
        // graph-var writes produce smooth visual, slow enough to stay well
        // under the Phase D v13 always-snap rate that triggered 0x30B350E.
        float meshBefore[3] = { 0, 0, 0 };
        float meshAfter[3]  = { 0, 0, 0 };
        bool  meshReadOk    = false;
        {
            auto guard = actor->loadedData.LockRead();
            const auto* data = *guard;
            if (data && data->data3D) {
                SafeReadNodeWorld(data->data3D.get(), meshBefore);
                meshReadOk = true;
            }
        }
        // SetPosition snap - only when drift exceeds threshold AND throttle
        // allows. Unconditional 150 ms snaps caused visible "light teleport"
        // at user-perception level even when drift was < 0.2 m. With a drift
        // gate, snaps fire only when something real has gone wrong (cell
        // change, packet gap, or host NPC teleport) - smooth frames get no
        // snap, just the 60 Hz lerp + origUpdate pipeline.
        {
            // root-cause fix: gate on MESH drift, not data.location drift.
            // The adaptive lerp above writes data.location≈predicted every tick,
            // so a data-vs-predicted gate reads ≈0 and the snap NEVER fires - yet
            // the visible MESH gets parked metres away (Havok overwrites
            // mesh.world from the collisionObject and never lets go), which is the
            // client jitter. meshBefore (read above) vs predicted is what actually
            // detects the detach; SetPosition then pulls all five layers (data,
            // mesh.translate, mesh.rotate, Havok body, cell hashmap) back together.
            float driftSq;
            if (meshReadOk) {
                const float dx = meshBefore[0] - predicted.x;
                const float dy = meshBefore[1] - predicted.y;
                const float dz = meshBefore[2] - predicted.z;
                driftSq = dx * dx + dy * dy + dz * dz;
            } else {
                // Mesh unreadable (no 3D yet) - fall back to data.location so a
                // truly stuck actor is still rescued; rare.
                float cur[3] = { 0, 0, 0 };
                SafeReadDataLocation(actor, cur);
                const float dx = cur[0] - predicted.x;
                const float dy = cur[1] - predicted.y;
                const float dz = cur[2] - predicted.z;
                driftSq = dx * dx + dy * dy + dz * dz;
            }
            // 3 m mesh-detach threshold. In smooth play the engine keeps the mesh
            // on data.location (< ~1 m), so this stays silent and the anim plays;
            // only a real Havok park / cell change / host teleport trips it. The
            // 150 ms throttle bounds anim-state-machine churn from the snap.
            if (driftSq > 3.0f * 3.0f) {
                static std::unordered_map<uint32_t, uint64_t> s_tickSnap;
                static std::mutex                             s_tickSnapMutex;
                const uint64_t now = NowMs();
                bool canSnap = false;
                {
                    std::lock_guard lk(s_tickSnapMutex);
                    auto& last = s_tickSnap[actor->formID];
                    if (now - last >= 150) { last = now; canSnap = true; }
                }
                if (canSnap) {
                    RE::NiPoint3A snapPos(predicted.x, predicted.y, predicted.z);
                    SafeSetPositionSnap(actor,
                                        RE::NiPoint3(snapPos.x, snapPos.y, snapPos.z),
                                        tgtRotZ);
                }
            }
        }
        if (meshReadOk) {
            auto guard = actor->loadedData.LockRead();
            const auto* data = *guard;
            if (data && data->data3D) {
                SafeReadNodeWorld(data->data3D.get(), meshAfter);
            }
        }

        // --- Per-actor diagnostic dump (rate-limited 5 s) -------------------
        // Shows the full chain: target → data.location → mesh.world.  If
        // mesh.world doesn't equal data.location after our write, we have
        // another engine override path still active.
        {
            static std::unordered_map<uint32_t, uint64_t> s_lastDump;
            static std::mutex                              s_dumpMutex;
            const uint64_t now = NowMs();
            bool doDump = false;
            {
                std::lock_guard lk(s_dumpMutex);
                auto& last = s_lastDump[actor->formID];
                if (now - last >= 5000) { last = now; doDump = true; }
            }
            if (doDump && meshReadOk) {
                float curData[3] = { 0, 0, 0 };
                SafeReadDataLocation(actor, curData);
                const float md = std::sqrt(
                    (meshAfter[0] - predicted.x) * (meshAfter[0] - predicted.x) +
                    (meshAfter[1] - predicted.y) * (meshAfter[1] - predicted.y) +
                    (meshAfter[2] - predicted.z) * (meshAfter[2] - predicted.z));
                PLOG_INFO("[PULSAR] LOCO-DUMP actor=0x{:08X} tgt=({:.1f},{:.1f},{:.1f}) drift={:.2f}m"sv,
                          static_cast<uint32_t>(actor->formID),
                          predicted.x, predicted.y, predicted.z,
                          md);
                PLOG_INFO("[PULSAR] LOCO-DUMP   data=({:.1f},{:.1f},{:.1f}) mesh=({:.1f},{:.1f},{:.1f})"sv,
                          curData[0], curData[1], curData[2],
                          meshAfter[0], meshAfter[1], meshAfter[2]);
            }
        }

        // Effective speed from *derived* velocity, CLAMPED to the sane
        // human-actor range. Even with discontinuity-rejection in
        // ApplyLocomotion, smoothing can still transiently produce values
        // above sprint - clamp hard so the anim blend tree stays in the
        // defined speed range (walk ≈ 1.5, run ≈ 3.5, sprint ≈ 5.5).
        float speedMag = std::sqrt(vx * vx + vy * vy);
        if (speedMag > 6.0f) speedMag = 6.0f;
        float direction = 0.0f;
        if (speedMag > 0.1f) {
            float curYaw = 0.0f;
            SafeReadRotZ(actor, &curYaw);
            const float worldHeading = std::atan2(vx, vy);     // sin=X, cos=Y
            direction = worldHeading - curYaw;
            // wrap to [-pi, pi]
            while (direction >  3.14159265f) direction -= 6.28318531f;
            while (direction < -3.14159265f) direction += 6.28318531f;
        }

        WriteGraphVarsImpl(actor, speedMag, direction, moveFlags, weaponDrawn);
    }

    // Diagnostic: read Speed/Direction only, rate-limited per (actor, label).
    // Used by the three-point timing probe in HookedActorUpdate to tell us
    // whether origUpdate overwrites our pre-tick graph-var writes.
    void ReadGraphSnapshot(RE::Actor* actor, const char* label)
    {
        if (!actor || !label) return;
        // crash-fix: don't read a not-yet-settled graph (pure-virtual-call
        // is possible on the read path too, and __try can't catch it).
        if (!NpcGraphSafeToDrive(actor)) return;
        static std::unordered_map<uint64_t, uint64_t> s_lastLog;
        static std::mutex                             s_mutex;
        const uint64_t key = (static_cast<uint64_t>(actor->formID) << 32)
                           | static_cast<uint8_t>(label[0]);
        const uint64_t now = NowMs();
        {
            std::lock_guard lk(s_mutex);
            auto& last = s_lastLog[key];
            if (now - last < 3000) return;
            last = now;
        }
        static const RE::BSFixedString kSpeed("Speed");
        static const RE::BSFixedString kDir("Direction");
        const float spd = SafeReadGraphFloat(actor, kSpeed);
        const float dir = SafeReadGraphFloat(actor, kDir);
        PLOG_INFO("[PULSAR] LOCO-PROBE {} actor=0x{:08X} Speed={:.2f} Dir={:.2f}"sv,
                  label, static_cast<uint32_t>(actor->formID), spd, dir);
    }

    // --- Pre-tick graph-var writer (public export) --------------------------
    // Same logic as the one inside TickLerp, but designed to be called BEFORE
    // g_origActorUpdate. The engine's anim-graph tick reads graph vars during
    // origUpdate; if we write after, our values lag by one frame and the
    // walk-cycle transition never commits. Pre-writing closes that gap - the
    // anim tick sees Speed=1.5 etc. in the SAME frame we set them, so the
    // blend tree enters the walk state immediately.
    void PreTickWriteGraphVars(RE::Actor* actor)
    {
        if (!actor) return;
        // P3 combat-live: this NPC's own AI drives its anim - do NOT force our
        // graph vars (Speed/Direction/iState) on top, that fought the AI and made
        // it jitter / hard to hit. Let the AI own the whole anim+position.
        if (NpcCombatAiLiveOn() && IsNpcCombatLive(actor->formID)) return;

        RE::NiPoint3 velocity;
        uint8_t      moveFlags = 0;
        bool         weaponDrawn = false;
        bool         found = false;
        {
            std::lock_guard lk(g_targetsMutex);
            auto it = g_targets.find(actor->formID);
            if (it == g_targets.end()) return;
            if (NowMs() - it->second.updatedMs > 5000) {
                g_targets.erase(it);
                return;
            }
            velocity    = it->second.velocity;
            moveFlags   = it->second.moveFlags;
            weaponDrawn = it->second.weaponDrawn;
            found       = true;
        }
        if (!found) return;

        float speedMag = std::sqrt(velocity.x * velocity.x +
                                   velocity.y * velocity.y);
        if (speedMag > 6.0f) speedMag = 6.0f;   // clamp - see TickLerp comment
        float direction = 0.0f;
        if (speedMag > 0.1f) {
            float curYaw = 0.0f;
            SafeReadRotZ(actor, &curYaw);
            const float worldHeading = std::atan2(velocity.x, velocity.y);
            direction = worldHeading - curYaw;
            while (direction >  3.14159265f) direction -= 6.28318531f;
            while (direction < -3.14159265f) direction += 6.28318531f;
        }

        WriteGraphVarsImpl(actor, speedMag, direction, moveFlags, weaponDrawn);
        NotifyMovementEvent(actor, speedMag, moveFlags);
    }

    // Set the AIProcess byte-flag that MovementControllerNPC::Update checks
    // first-thing and bails on (see T116 decomp: `if (AIProcess+0xe7 != 0)
    // return 0;`). Effect: tracked NPCs' MC::Update becomes a no-op, which
    // stops engine AI-driven movement + navmesh pathing without needing to
    // install a full DirectControl MovementAgent (that path is polymorphic
    // via IBSTCreator registry, not cleanly reachable - see T117-T119).
    //
    // Anim graph tick runs via the renderer pipeline, not via MC::Update,
    // so walk anim still plays driven by our graph-var writes.
    //
    // actor + 0x228 = AIProcess*, + 0xE7 = "AI disabled" flag (1 byte).
    static void SetAIDisabled(RE::Actor* actor, bool disabled)
    {
        __try {
            auto u = reinterpret_cast<uintptr_t>(actor);
            uintptr_t aiProc = *reinterpret_cast<uintptr_t*>(u + 0x228);
            if (!aiProc) return;
            uint8_t* flag = reinterpret_cast<uint8_t*>(aiProc + 0xE7);
            *flag = disabled ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Dispatch animation-graph event string. Triggers state-machine
    // transitions that graph-var-only writes can't. Pattern from Skyrim MP
    // mods: walk anim requires event "moveStart" / "moveStop" sent via
    // IAnimationGraphManagerHolder::NotifyAnimationGraph (virtual, slot 1
    // on CommonLibSF). We probe for it and call if available.
    static int SafeNotifyGraph(RE::Actor* actor, const RE::BSFixedString& evt)
    {
        __try {
            // IAnimationGraphManagerHolder vtable slot 1 = NotifyAnimationGraphImpl
            return actor->NotifyAnimationGraphImpl(evt) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    // P3 fire-sync: mirror the host NPC's weapon fire on the AI-blocked client
    // clone. Deep-RE (FIRE_RE) found "WeaponFire" is an interned graph EVENT (set via
    // FUN_1428cfb80 alongside iSyncGunActions) - so the proper trigger is the anim
    // event, NOT writing the iSyncGunActions int (whose pulse value 23/34/44 we'd
    // have had to guess). moveFlags bit 0x4 = host is firing this packet. Throttle to
    // a plausible cadence so sustained fire (bit stays set across 30Hz packets) plays
    // repeated shots instead of a per-frame strobe.
    static void NotifyFireEvent(RE::Actor* actor, uint8_t moveFlags)
    {
        if (!actor || !(moveFlags & 0x4)) return;
        static std::unordered_map<uint32_t, uint64_t> s_lastFire;
        static std::mutex                             s_fireMutex;
        const uint64_t now = NowMs();
        {
            std::lock_guard lk(s_fireMutex);
            auto& last = s_lastFire[actor->formID];
            if (now - last < 150) return;   // ~6-7 shots/s cap
            last = now;
        }
        // RE pass 2 (FIRE_RE2): "WeaponFire" Notify returned 0 (rejected - it's a
        // var, not a consumable event). The real attack/fire anim is driven by the
        // "AttackWinStart" event (attack-window open @0x144B30A10). Send that; log
        // both rc's so we can confirm which the graph accepts.
        static const RE::BSFixedString kAttackWin("AttackWinStart");
        static const RE::BSFixedString kWeaponFire("WeaponFire");
        const int r  = SafeNotifyGraph(actor, kAttackWin);
        const int r2 = SafeNotifyGraph(actor, kWeaponFire);
        static std::unordered_map<uint32_t, uint64_t> s_lastLog;
        static std::mutex                             s_logMutex;
        {
            std::lock_guard lk(s_logMutex);
            auto& ll = s_lastLog[actor->formID];
            if (now - ll > 3000) { ll = now;
                PLOG_INFO("[FIRE-DBG] client fire actor=0x{:08X} AttackWinStart.rc={} WeaponFire.rc={}"sv,
                          static_cast<uint32_t>(actor->formID), r, r2); }
        }
    }

    void NotifyMovementEvent(RE::Actor* actor, float speed, uint8_t moveFlags)
    {
        if (!actor) return;

        static std::unordered_map<uint32_t, int> s_lastState;  // 0=idle 1=walk 2=run 3=sprint
        static std::mutex s_stateMutex;

        int newState = 0;
        if      (moveFlags & 0x2)       newState = 3;   // sprint
        else if (speed > 3.5f)          newState = 2;   // run
        else if (speed > 0.5f)          newState = 1;   // walk
        else                            newState = 0;   // idle

        int oldState = 0;
        {
            std::lock_guard lk(s_stateMutex);
            auto it = s_lastState.find(actor->formID);
            if (it != s_lastState.end()) oldState = it->second;
            if (oldState == newState) return;   // no transition
            s_lastState[actor->formID] = newState;
        }

        static const RE::BSFixedString kMoveStart("moveStart");
        static const RE::BSFixedString kMoveStop ("moveStop");
        static const RE::BSFixedString kRunStart ("runStart");
        static const RE::BSFixedString kSprintStart("sprintStart");

        // Transitioning into motion?
        if (oldState == 0 && newState >= 1) {
            SafeNotifyGraph(actor, kMoveStart);
        }
        // Transitioning to stop?
        else if (newState == 0 && oldState >= 1) {
            SafeNotifyGraph(actor, kMoveStop);
        }
        // Upshift idle->run or walk->run
        if (newState >= 2 && oldState < 2) {
            SafeNotifyGraph(actor, kRunStart);
        }
        if (newState == 3 && oldState < 3) {
            SafeNotifyGraph(actor, kSprintStart);
        }
    }

}
