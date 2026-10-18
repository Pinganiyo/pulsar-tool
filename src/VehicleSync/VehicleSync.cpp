#include "PCH.h"
#include "VehicleSync/VehicleSync.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "NASLog.h"

#include <RE/A/Actor.h>
#include <RE/A/AIProcess.h>
#include <RE/B/BSPointerHandle.h>
#include <RE/F/FormTypes.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiPoint.h>
#include <RE/N/NiSmartPointer.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESDataHandler.h>
#include <RE/P/ProcessLists.h>

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "Config.h"
#include "VehicleSync/RidePolicy.h"
#include "NPCSync/PluginLookup.h"

// L.7 forward decl from Main.cpp. Triggered when host's clone
// unseats on this client, fires a diag snapshot to capture the engine
// state-change that accompanies the pilot-lock release.
namespace Pulsar::Diag293 { void OnHostCloneUnseated(); }
#include "NPCSync/NPCSync.h"

#include <RE/G/GameVM.h>
#include <RE/I/IVirtualMachine.h>
#include <RE/B/BSScriptUtil.h>

using namespace std::string_view_literals;

namespace Pulsar::VehicleSync
{
    bool IsHostMode();

    // ---- occupiedFurniture handle read -----------------------------------
    // Raw read + zero-check on the PLAYER's AIProcess chain. When the player
    // isn't in any furniture the handle is 0 and we must NOT call into
    // Starfield's GetSmartPointer - the handle lookup derefs a stale
    // free-list entry on non-Actor handle buckets and crashes (observed on
    // Bondar proc-cell, dump a857dda8 @ 2026-04-22).
    //
    // Layout (CommonLibSF 1.16.236):
    //   Actor + 0x228 = AIProcess*
    //   AIProcess + 0x08 = MiddleHighProcessData*
    //   MiddleHighProcessData + 0x428 = BSPointerHandle<TESObjectREFR> (uint32)

    static uint32_t SafeReadFurnHandle(RE::Actor* a_actor)
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

    // Diagnostic - read both furniture-handle slots raw. Moon Jumper might use
    // currentFurniture (+0x424) rather than occupiedFurniture (+0x428), or
    // ships might not use either. Plain uint32 raw reads → SEH-safe.
    static void SafeReadFurnHandlesDiag(RE::Actor* a_actor,
                                        uint32_t& a_outCurrent,
                                        uint32_t& a_outOccupied)
    {
        a_outCurrent = 0;
        a_outOccupied = 0;
        __try {
            if (!a_actor) return;
            auto* a = reinterpret_cast<uint8_t*>(a_actor);
            auto* proc = *reinterpret_cast<uint8_t**>(a + 0x228);
            if (!proc) return;
            auto* mh = *reinterpret_cast<uint8_t**>(proc + 0x08);
            if (!mh) return;
            a_outCurrent  = *reinterpret_cast<uint32_t*>(mh + 0x424);
            a_outOccupied = *reinterpret_cast<uint32_t*>(mh + 0x428);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // leave zeroed
        }
    }

    // Diagnostic - is the player inside a spaceship? Moon Jumper may be
    // classified as spaceship-REF, not furniture. TESObjectREFR::GetSpaceship
    // is a non-virtual helper - plain call, but wrap anyway in case the
    // actor's cell isn't loaded cleanly.
    static RE::TESObjectREFR* SafeGetSpaceship(RE::Actor* a_actor)
    {
        __try {
            if (!a_actor) return nullptr;
            return a_actor->GetSpaceship(true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // ---- diagnostic - dump MJ FURN + bhkCollisionObject raw fields ------
    // Called on mode-change (host enter/exit MJ, client SnapInto moment) and
    // periodically while seated. Captures:
    //   FURN vtable[0],  FURN+0x8C (data.location), FURN+0x98 (angle)
    //   FURN+0xA8 (loadedData ptr), loadedData->various (refr internal)
    //   FURN+0x70 path - extras-array pointer (not bhkColObj as first thought;
    //   bhkColObj lives through loadedData instead)
    //   Actor actorState1/actorState2 for the host player
    // Plain C read, SEH-guarded. Output: one-line log with all hex fields.
    static void SafeDumpFurnDiag(const char* a_who, RE::TESObjectREFR* a_furn,
                                 RE::Actor* a_actor)
    {
        if (!a_furn) {
            PLOG_INFO("[PULSAR] FURN-DIAG {}: furn=null"sv, a_who);
            return;
        }
        uint64_t f_vtbl = 0, f_loc_x = 0, f_loc_y = 0, f_loc_z = 0;
        uint64_t f_plus70 = 0, f_plus_ed = 0, f_plus_e8 = 0;
        uint64_t f_loadedData = 0, f_ld_plus_a0 = 0, f_ld_plus_a8 = 0;
        uint64_t f_ld_plus_b0 = 0;
        uint32_t f_formFlags = 0, f_formID = 0;
        uint8_t  f_formType = 0;
        uint32_t as1 = 0, as2 = 0;
        uint64_t vt_slot_89 = 0;  // function pointer at vtable[0x89]
        uint64_t vt_slot_88 = 0;
        uint64_t vt_slot_8a = 0;
        uint64_t exe_base = 0;
        __try {
            auto* f = reinterpret_cast<uint8_t*>(a_furn);
            f_vtbl       = *reinterpret_cast<uint64_t*>(f);
            f_formFlags  = *reinterpret_cast<uint32_t*>(f + 0x20);
            f_formID     = *reinterpret_cast<uint32_t*>(f + 0x28);
            f_formType   = *reinterpret_cast<uint8_t*>(f + 0x2E);
            f_plus70     = *reinterpret_cast<uint64_t*>(f + 0x70);
            // data.location @ +0x8C is 3 floats - pack as u64 bits for hex log
            std::memcpy(&f_loc_x, f + 0x8C, 4);
            std::memcpy(&f_loc_y, f + 0x90, 4);
            std::memcpy(&f_loc_z, f + 0x94, 4);
            f_plus_e8    = *reinterpret_cast<uint64_t*>(f + 0xE8);
            f_loadedData = *reinterpret_cast<uint64_t*>(f + 0xE0);
            if (f_loadedData) {
                auto* ld = reinterpret_cast<uint8_t*>(f_loadedData);
                f_ld_plus_a0 = *reinterpret_cast<uint64_t*>(ld + 0xA0);
                f_ld_plus_a8 = *reinterpret_cast<uint64_t*>(ld + 0xA8);
                f_ld_plus_b0 = *reinterpret_cast<uint64_t*>(ld + 0xB0);
            }
            // Dump vt[0x88..0x8A] as RVA so we can diff against the known
            // SetPosition RVA (0x140B47... per memory). If slot 0x89 is
            // something other than SetPosition on this subclass, we'll see.
            if (f_vtbl) {
                auto* vt = reinterpret_cast<uint64_t*>(f_vtbl);
                vt_slot_88 = vt[0x88];
                vt_slot_89 = vt[0x89];
                vt_slot_8a = vt[0x8A];
            }
            // Module base so we can turn absolute pointers into RVAs.
            exe_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (a_actor) {
            __try {
                as1 = static_cast<uint32_t>(a_actor->actorState1);
                as2 = static_cast<uint32_t>(a_actor->actorState2);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        // Compute RVAs for vtable + slot fn pointers (exe_base may be 0 if
        // GetModuleHandleW failed - then RVAs stay absolute).
        uint64_t vt_rva = exe_base && f_vtbl > exe_base ? f_vtbl - exe_base : f_vtbl;
        uint64_t s88_rva = exe_base && vt_slot_88 > exe_base ? vt_slot_88 - exe_base : vt_slot_88;
        uint64_t s89_rva = exe_base && vt_slot_89 > exe_base ? vt_slot_89 - exe_base : vt_slot_89;
        uint64_t s8a_rva = exe_base && vt_slot_8a > exe_base ? vt_slot_8a - exe_base : vt_slot_8a;
        PLOG_INFO("[PULSAR] FURN-DIAG {}: formID=0x{:08X} ft={} ff=0x{:08X} "
                  "loc=(bits {:08X}/{:08X}/{:08X}) +70=0x{:016X} +E0(loadedData)=0x{:016X} "
                  "ld+A0=0x{:016X} ld+A8=0x{:016X} ld+B0=0x{:016X} vt-rva=0x{:X} "
                  "vt[0x88]-rva=0x{:X} vt[0x89]-rva=0x{:X} vt[0x8A]-rva=0x{:X} "
                  "actor.as1=0x{:08X} actor.as2=0x{:08X}"sv,
                  a_who, f_formID, f_formType, f_formFlags,
                  static_cast<uint32_t>(f_loc_x), static_cast<uint32_t>(f_loc_y), static_cast<uint32_t>(f_loc_z),
                  f_plus70, f_loadedData, f_ld_plus_a0, f_ld_plus_a8, f_ld_plus_b0,
                  vt_rva, s88_rva, s89_rva, s8a_rva, as1, as2);
    }

    // ---- position writers -------------------------------------------------
    // TESObjectREFR::SetPosition lives at vtable slot 0x89 - CommonLibSF
    // doesn't bind it by name, but RemotePlayerManager proves it works on
    // non-Actor refs (marker moves). Signature matches Actor::SetPosition
    // (NiPoint3 by ref + bool). For a FURN there's no char controller, so
    // the second arg is passed as false.
    //
    // __try can't coexist with C++ unwinding in the same function (C2712),
    // so the vtable call lives in this plain-C helper.

    static int SafeRefSetPosition(RE::TESObjectREFR* a_ref,
                                  float a_x, float a_y, float a_z)
    {
        __try {
            using SetPosFn = void(__fastcall*)(void*, const RE::NiPoint3&, bool);
            auto** vt = *reinterpret_cast<void***>(a_ref);
            auto setPos = reinterpret_cast<SetPosFn>(vt[0x89]);
            RE::NiPoint3 pos(a_x, a_y, a_z);
            // Third arg = "also teleport Havok body". FURN has a Havok body
            // (Moon Jumper is a vehicle); without Havok teleport the engine
            // rolls data.location back to Havok body pos each tick
            // (observed 2026-04-22: drift=12.9m despite setResult=1).
            setPos(a_ref, pos, true);

            // Diag: MJ FURN observed NOT writing to data.location via vtable[0x89]
            // on 1.16.236 - slot is wrong for this subclass, or furniture system
            // pins position post-SnapInto. Force raw write as backup so we can
            // at least DIAG whether engine reverts it.
            auto* f = reinterpret_cast<uint8_t*>(a_ref);
            *reinterpret_cast<float*>(f + 0x8C) = a_x;
            *reinterpret_cast<float*>(f + 0x90) = a_y;
            *reinterpret_cast<float*>(f + 0x94) = a_z;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Read back ref data.location (for verifying SetPosition took effect).
    static void SafeReadRefPos(RE::TESObjectREFR* a_ref, float& x, float& y, float& z)
    {
        __try {
            auto* loc = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(a_ref) + 0x8C);
            x = loc[0]; y = loc[1]; z = loc[2];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            x = 0; y = 0; z = 0;
        }
    }

    // SetPosition doesn't take a rotation; angle.z is written raw afterwards.
    static int SafeRefWriteAngleZ(RE::TESObjectREFR* a_ref, float a_rotZ)
    {
        __try {
            auto* anglePtr = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(a_ref) + 0x98);
            anglePtr[2] = a_rotZ;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Raw write all 3 Euler angles - DISABLED: writing to +0x98/+0x9C crashed
    // the client with garbage-pointer formType deref (rbx=float bits, saved
    // return=0x3c888889=float 1/60). Starfield's TESObjectREFR layout apparently
    // has a non-float field at +0x9C that must not be float-written.
    // Only angle.z at +0xA0 is safe (old SafeRefWriteAngleZ worked for months).
    // Keep the helper declared but route through SafeRefWriteAngleZ for now.
    static int SafeRefWriteAngleFull(RE::TESObjectREFR* a_ref,
                                     float /*a_rotX*/, float /*a_rotY*/, float a_rotZ)
    {
        return SafeRefWriteAngleZ(a_ref, a_rotZ);
    }

    // SEH-guarded raw read of FURN+0xA0 (angle.z). Plain-C function so __try
    // can coexist without C2712.
    static float SafeRefReadAngleZ(RE::TESObjectREFR* a_ref)
    {
        __try {
            if (!a_ref) return 0.0f;
            auto* ap = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(a_ref) + 0xA0);
            return *ap;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0.0f;
        }
    }

    // Force-write Yaw rotation onto a NiAVObject's world.rotate (3x3 matrix
    // at +0x8C per NPCSync pattern - it writes world.translate at +0xB0, so
    // the 52-byte NiTransform is rotate@+0x8C, translate@+0xB0).
    // Starfield's Bethesda convention: Z-up, Y-forward (maybe). Build a
    // standard Z-rotation matrix ignoring rotX/rotY for now (pitch/roll lives
    // in Havok body quaternion anyway - we'd need the Havok write for those).
    //
    //    [  cos(z)  -sin(z)   0 ]
    //    [  sin(z)   cos(z)   0 ]
    //    [    0        0      1 ]
    //
    // NiMatrix3 is ROW-MAJOR in Bethesda's engine (3 rows × 3 floats).
    static void SafeWriteNodeYaw(RE::NiAVObject* a_node, float a_yaw)
    {
        __try {
            if (!a_node) return;
            float c = std::cos(a_yaw);
            float s = std::sin(a_yaw);
            auto* rot = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(a_node) + 0x8C);
            rot[0] = c;  rot[1] = -s; rot[2] = 0.0f;
            rot[3] = s;  rot[4] =  c; rot[5] = 0.0f;
            rot[6] = 0.0f; rot[7] = 0.0f; rot[8] = 1.0f;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Pull the NiAVObject root of the FURN's 3D mesh (from loadedData.data3D)
    // and force-write the yaw rotation matrix. Mirrors the NPCSync approach
    // for dynamic NPC position sync.
    static void ForceFurnNodeYaw(RE::TESObjectREFR* a_ref, float a_yaw)
    {
        if (!a_ref) return;
        RE::NiAVObject* node = nullptr;
        {
            auto guard = a_ref->loadedData.LockRead();
            const auto* data = *guard;
            if (data && data->data3D) node = data->data3D.get();
        }
        if (!node) return;
        SafeWriteNodeYaw(node, a_yaw);
    }

    // Diagnostic: read raw bytes at NiNode+0x8C (world.rotate matrix) and
    // NiNode+0xB0 (world.translate) so we can verify offsets + see whether
    // the engine overwrites our write.
    static void SafeDumpNodeTransform(const char* a_who, RE::NiAVObject* a_node)
    {
        if (!a_node) {
            PLOG_INFO("[PULSAR] NODE-DIAG {}: node=null"sv, a_who);
            return;
        }
        uint32_t r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0,r8=0;
        uint32_t t0=0,t1=0,t2=0;
        uint64_t vt = 0;
        __try {
            auto* n = reinterpret_cast<uint8_t*>(a_node);
            vt = *reinterpret_cast<uint64_t*>(n);
            auto* rp = reinterpret_cast<uint32_t*>(n + 0x8C);
            r0 = rp[0]; r1 = rp[1]; r2 = rp[2];
            r3 = rp[3]; r4 = rp[4]; r5 = rp[5];
            r6 = rp[6]; r7 = rp[7]; r8 = rp[8];
            auto* tp = reinterpret_cast<uint32_t*>(n + 0xB0);
            t0 = tp[0]; t1 = tp[1]; t2 = tp[2];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        PLOG_INFO("[PULSAR] NODE-DIAG {}: node=0x{:X} vt=0x{:X} rot+8C=({:08X},{:08X},{:08X}|{:08X},{:08X},{:08X}|{:08X},{:08X},{:08X}) trans+B0=({:08X},{:08X},{:08X})"sv,
                  a_who, reinterpret_cast<uintptr_t>(a_node), vt,
                  r0,r1,r2, r3,r4,r5, r6,r7,r8,
                  t0,t1,t2);
    }

    // Variant of ForceFurnNodeYaw that also dumps BEFORE and AFTER state.
    static void ForceFurnNodeYawDiag(RE::TESObjectREFR* a_ref, float a_yaw, bool a_doDiag)
    {
        if (!a_ref) return;
        RE::NiAVObject* node = nullptr;
        {
            auto guard = a_ref->loadedData.LockRead();
            const auto* data = *guard;
            if (data && data->data3D) node = data->data3D.get();
        }
        if (!node) {
            if (a_doDiag) PLOG_INFO("[PULSAR] NODE-DIAG: FURN has no data3D"sv);
            return;
        }
        if (a_doDiag) SafeDumpNodeTransform("PRE", node);
        SafeWriteNodeYaw(node, a_yaw);
        if (a_doDiag) SafeDumpNodeTransform("POST", node);
    }

    // -------- Host side state ---------------------------------------------

    struct HostSent
    {
        uint64_t timeMs = 0;
        float    x = 0, y = 0, z = 0;
    };
    static std::unordered_map<uint32_t, HostSent> s_hostSent;
    static uint32_t s_hostLastCell = 0;

    // -------- Client side state -------------------------------------------

    static std::unordered_map<uint32_t, RE::TESObjectREFR*> s_remap;
    static std::mutex s_remapMutex;

    struct ClientSent
    {
        uint64_t timeMs = 0;
        float    x = 0, y = 0, z = 0;
    };
    static std::unordered_map<uint32_t, ClientSent> s_clientApplied;

    static std::vector<Pulsar::Network::VehiclePosData> s_pending;
    static std::mutex s_pendingMutex;

    // Latest host-authoritative FURN info: set whenever PKT_VEHICLE_POS
    // arrives. Read by OnRemoteSeatedUpdate when the seated bit fires so
    // we filter the local FURN scan by the RESOLVED base (cross-plugin).
    struct HostFurnInfo {
        uint32_t resolvedBaseID = 0;   // client-local base FormID after T34 re-encode
        uint32_t baseLocalID    = 0;
        char     plugin[Pulsar::Network::NPC_SOURCE_PLUGIN_MAX] = {};
        float    posX = 0, posY = 0, posZ = 0;
        float    rotX = 0, rotY = 0, rotZ = 0;
        uint64_t lastUpdateMs = 0;
    };
    static HostFurnInfo s_hostFurn;
    static std::mutex   s_hostFurnMutex;

    // ---------------------------------------------------------------------

    // SetAngle native fn pointer (resolved at init time from module base).
    // Definition lives near SetMotionType below; declare here to use in Init.
    using SetAngle_t = void(__fastcall*)(void*, uint32_t, RE::TESObjectREFR*, float, float, float);
    static SetAngle_t g_setAngle = nullptr;

    bool Init()
    {
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        if (base) {
            g_setAngle = reinterpret_cast<SetAngle_t>(base + 0x207F690);
        }
        PLOG_INFO("[PULSAR] VehicleSync: ready (PKT_VEHICLE_POS = 0x72, SetAngle fn={})"sv,
                  reinterpret_cast<void*>(g_setAngle));
        return true;
    }

    void OnPacketReceived(const uint8_t* a_data, std::size_t a_length)
    {
        using namespace Pulsar::Network;
        if (!a_data || a_length < 1 + sizeof(VehiclePosData)) return;
        if (a_data[0] != PKT_VEHICLE_POS) return;

        VehiclePosData v{};
        std::memcpy(&v, a_data + 1, sizeof(v));

        // T34-style cross-plugin resolve: turn host's {plugin, localID} into
        // this client's load-order baseFormID. Stored in s_hostFurn so the
        // seated state machine (fired via PKT_POSITION bit 10) can filter
        // FURN scans by the correct base.
        uint32_t resolved = 0;
        if (v.basePlugin[0] != '\0' && v.baseLocalID != 0) {
            resolved = Pulsar::NPCSync::PluginLookup::ResolveCrossPlugin(
                v.basePlugin, v.baseLocalID);
        }

        {
            std::lock_guard lock(s_hostFurnMutex);
            s_hostFurn.resolvedBaseID = resolved;
            s_hostFurn.baseLocalID    = v.baseLocalID;
            std::memcpy(s_hostFurn.plugin, v.basePlugin, sizeof(s_hostFurn.plugin));
            s_hostFurn.posX = v.posX; s_hostFurn.posY = v.posY; s_hostFurn.posZ = v.posZ;
            s_hostFurn.rotX = v.rotX; s_hostFurn.rotY = v.rotY; s_hostFurn.rotZ = v.rotZ;
            s_hostFurn.lastUpdateMs = GetTickCount64();
        }

        static uint64_t s_lastRecvLog = 0;
        uint64_t nowLog = GetTickCount64();
        if (nowLog - s_lastRecvLog > 3000) {
            s_lastRecvLog = nowLog;
            PLOG_INFO("[PULSAR] VehicleSync PKT_VEHICLE_POS: host-ref=0x{:08X} host-base=0x{:08X} localID=0x{:08X} plugin='{}' resolvedBase=0x{:08X}"sv,
                      v.refFormID, v.baseFormID, v.baseLocalID,
                      v.basePlugin[0] ? v.basePlugin : "(empty)",
                      resolved);
        }

        std::lock_guard lock(s_pendingMutex);
        s_pending.push_back(v);
    }

    // Raw, SEH-wrapped nearest-FURN scan. Avoids ForEachReference because
    // during Moon-Jumper activation in a proc-cell the cell's references
    // array is mid-teardown (observed data=NULL, size=0xC4E60000 garbage -
    // dump eb28ecb1 @ 2026-04-22). The lock ForEachReference takes doesn't
    // protect against that state.
    //
    // Layout (CommonLibSF 1.16.236, TESObjectCELL):
    //   cell + 0x80 = BSTArray<NiPointer<TESObjectREFR>> references
    //     +0x00 allocator
    //     +0x08 size (uint32)        → cell + 0x88
    //     +0x10 data (T**)           → cell + 0x90
    static RE::TESObjectREFR* SafeFindNearestFurnInCell(void* a_cell,
                                                        float a_px,
                                                        float a_py,
                                                        float a_pz,
                                                        float a_radius)
    {
        __try {
            if (!a_cell) return nullptr;
            auto* cb = reinterpret_cast<uint8_t*>(a_cell);
            // BSTArray layout (CommonLibSF): _size@+0, _capacity@+4, _data@+8.
            // references lives at cell+0x80 so size@+0x80, data@+0x88.
            uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > 10000) return nullptr;

            RE::TESObjectREFR* best = nullptr;
            float bestDist2 = a_radius * a_radius;

            for (uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;
                auto* base = *reinterpret_cast<uint8_t**>(
                    reinterpret_cast<uint8_t*>(ref) + 0x98);  // data.objectReference
                if (!base) continue;
                uint8_t ft = *reinterpret_cast<uint8_t*>(base + 0x2E);  // TESForm.formType
                if (ft != static_cast<uint8_t>(RE::FormType::kFURN)) continue;
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(ref) + 0x8C);  // data.location
                float dx = loc[0] - a_px, dy = loc[1] - a_py, dz = loc[2] - a_pz;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    best = ref;
                }
            }
            return best;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // Resolves player->occupiedFurniture without touching the handle manager
    // (REL::ID 72432 is Actor-only on 1.16.236, returns false for REFR handles).
    // When the handle is non-zero we know the player is seated; scan cell
    // refs for the nearest FURN. Scan is raw + SEH-wrapped because the cell
    // references array is often torn mid-frame during proc-cell activation.
    static RE::TESObjectREFR* GetOccupiedFurniture(RE::Actor* a_actor)
    {
        uint32_t h = SafeReadFurnHandle(a_actor);
        if (h == 0) return nullptr;
        if (!a_actor || !a_actor->parentCell) return nullptr;

        auto ppos = a_actor->GetPosition();
        return SafeFindNearestFurnInCell(a_actor->parentCell,
                                         ppos.x, ppos.y, ppos.z,
                                         25.0f);
    }

    // Client-side matcher - same raw SEH-wrapped scan as host. Filters by
    // baseFormID when provided; otherwise falls back to any nearest FURN.
    static RE::TESObjectREFR* FindLocalFurn(RE::TESObjectCELL* a_cell,
                                            uint32_t a_hostBaseID,
                                            const RE::NiPoint3& a_hostPos,
                                            float a_radius)
    {
        __try {
            if (!a_cell) return nullptr;
            auto* cb = reinterpret_cast<uint8_t*>(a_cell);
            uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > 10000) return nullptr;

            RE::TESObjectREFR* best = nullptr;
            float bestDist2 = a_radius * a_radius;

            for (uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;
                auto* base = *reinterpret_cast<uint8_t**>(
                    reinterpret_cast<uint8_t*>(ref) + 0x98);
                if (!base) continue;
                uint8_t ft = *reinterpret_cast<uint8_t*>(base + 0x2E);
                if (ft != static_cast<uint8_t>(RE::FormType::kFURN)) continue;
                if (a_hostBaseID != 0) {
                    uint32_t baseID = *reinterpret_cast<uint32_t*>(base + 0x28);
                    if (baseID != a_hostBaseID) continue;
                }
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(ref) + 0x8C);
                float dx = loc[0] - a_hostPos.x;
                float dy = loc[1] - a_hostPos.y;
                float dz = loc[2] - a_hostPos.z;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    best = ref;
                }
            }
            return best;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // read a FURN's data3D root world.rotate (NiMatrix3 @ node+0x8C,
    // row-major 9 floats). Split __try from the loadedData read-lock (C2712).
    static void SafeReadNodeRotate9(RE::NiAVObject* a_node, float out[9])
    {
        __try {
            auto* rp = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(a_node) + 0x8C);
            for (int i = 0; i < 9; ++i) out[i] = rp[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            for (int i = 0; i < 9; ++i) out[i] = 0.0f;
        }
    }
    static void SafeGetFurnNodeRotate(RE::TESObjectREFR* a_ref, float out[9])
    {
        for (int i = 0; i < 9; ++i) out[i] = 0.0f;
        if (!a_ref) return;
        RE::NiAVObject* node = nullptr;
        {
            auto guard = a_ref->loadedData.LockRead();
            const auto* d = *guard;
            if (d && d->data3D) node = d->data3D.get();
        }
        if (node) SafeReadNodeRotate9(node, out);
    }

    static void RunHostTick()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;

        uint32_t cellID = player->parentCell->formID;
        if (cellID != s_hostLastCell) {
            s_hostLastCell = cellID;
            s_hostSent.clear();
        }

        // Diagnostic: heartbeat every 1s with all candidate furniture-slot
        // states. Logs only when something non-zero shows up OR every 10s
        // otherwise - avoids spam but captures the moment the user enters
        // the Moon Jumper.
        {
            uint64_t dnow = GetTickCount64();
            static uint64_t s_lastDiag = 0;
            static uint32_t s_lastCurr = 0, s_lastOccu = 0;
            static uint32_t s_lastShipID = 0;

            uint32_t hCurr = 0, hOccu = 0;
            SafeReadFurnHandlesDiag(player, hCurr, hOccu);

            RE::TESObjectREFR* ship = SafeGetSpaceship(player);
            uint32_t shipID = ship ? ship->formID : 0;

            bool changed = (hCurr != s_lastCurr) ||
                           (hOccu != s_lastOccu) ||
                           (shipID != s_lastShipID);
            bool heartbeat = (dnow - s_lastDiag > 10000);
            if (changed || heartbeat) {
                s_lastDiag = dnow;
                s_lastCurr = hCurr;
                s_lastOccu = hOccu;
                s_lastShipID = shipID;
                PLOG_INFO("[PULSAR] VehicleSync diag: currFurn=0x{:08X} occFurn=0x{:08X} spaceship=0x{:08X}"sv,
                          hCurr, hOccu, shipID);
                // Deep dump on every transition (enter, exit, ship change) so we
                // can diff MJ internal fields across the mode change.
                if (changed) {
                    RE::TESObjectREFR* furnNow = GetOccupiedFurniture(player);
                    SafeDumpFurnDiag("HOST-transition", furnNow, player);
                }
            }
            // Additional heartbeat dump when actively seated - every 1.5s while
            // occupiedFurniture is held, so we can see fields evolve DURING
            // drive (settles, vehicle-state activation, etc.).
            static uint64_t s_lastSitDump = 0;
            if (hOccu != 0 && (dnow - s_lastSitDump > 1500)) {
                s_lastSitDump = dnow;
                RE::TESObjectREFR* furnNow = GetOccupiedFurniture(player);
                SafeDumpFurnDiag("HOST-seated", furnNow, player);
            }
        }

        auto* furn = GetOccupiedFurniture(player);
        if (!furn) {
            // Logged-once-per-entry diag: handle was non-zero but scan found
            // no FURN in cell. Tells us the cell scan is the blocker.
            static uint32_t s_loggedForHandle = 0;
            uint32_t hnow = SafeReadFurnHandle(player);
            if (hnow != 0 && hnow != s_loggedForHandle) {
                s_loggedForHandle = hnow;
                PLOG_INFO("[PULSAR] VehicleSync host: occFurn=0x{:08X} but cell-scan found no FURN (scan radius 25m)"sv,
                          hnow);
            }
            return;
        }

        auto base = furn->GetBaseObject();
        if (!base) {
            PLOG_WARN("[PULSAR] VehicleSync host: FURN ref=0x{:08X} has no baseObject"sv, furn->formID);
            return;
        }

        auto pos = furn->GetPosition();
        float rotX = furn->data.angle.x;
        float rotY = furn->data.angle.y;
        float rotZ = furn->data.angle.z;
        uint32_t hostRefID = furn->formID;
        uint32_t hostBaseID = base->GetFormID();

        uint64_t now = GetTickCount64();
        auto& ls = s_hostSent[hostRefID];
        float dx = pos.x - ls.x, dy = pos.y - ls.y, dz = pos.z - ls.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        bool shouldSend = (ls.timeMs == 0) ||
                          (now - ls.timeMs > 200) ||
                          (d2 > 0.25f);
        if (!shouldSend) return;

        ls.timeMs = now;
        ls.x = pos.x; ls.y = pos.y; ls.z = pos.z;

        using namespace Pulsar::Network;
        uint8_t buf[1 + sizeof(VehiclePosData)]{};
        buf[0] = PKT_VEHICLE_POS;

        VehiclePosData v{};
        v.refFormID = hostRefID;
        v.baseFormID = hostBaseID;
        v.cellFormID = cellID;
        v.posX = pos.x; v.posY = pos.y; v.posZ = pos.z;
        v.rotX = rotX; v.rotY = rotY; v.rotZ = rotZ;
        // exact orientation: host vehicle's render world.rotate (3x3).
        SafeGetFurnNodeRotate(furn, v.rot3x3);

        // Resolve cross-plugin identity of the FURN base: plugin filename +
        // flag-aware local ID. Same T34 pattern NPCSync uses for TESNPC base
        // resolution - lets the client re-encode the base under its own
        // load order (ESL/medium plugins have different FE/FF prefixes per
        // machine depending on load index).
        if ((hostBaseID & 0xFF000000) != 0xFF000000) {
            if (Pulsar::NPCSync::PluginLookup::FormIDToPluginName(
                    hostBaseID, v.basePlugin, sizeof(v.basePlugin))) {
                v.baseLocalID = Pulsar::NPCSync::PluginLookup::FormIDToLocalID(hostBaseID);
            }
        }

        std::memcpy(buf + 1, &v, sizeof(v));

        net.Send(buf, sizeof(buf), /*reliable*/ false);

        static uint64_t s_lastLog = 0;
        if (now - s_lastLog > 3000) {
            s_lastLog = now;
            PLOG_INFO("[PULSAR] VehicleSync host: ref=0x{:08X} base=0x{:08X} pos=({:.1f},{:.1f},{:.1f}) rot=({:.3f},{:.3f},{:.3f})"sv,
                      hostRefID, hostBaseID, pos.x, pos.y, pos.z, rotX, rotY, rotZ);
        }
    }

    // ===== Havok body resolve (244 RE, reference_vehicle_havok_re) =====
    // The vehicle render follows its live hknp body. To move it we need the body
    // (hknpWorld* + 24-bit bodyId) reached via the FURN's bhkNPCollisionObject.
    // vehhavok-v1 read NiAVObject+0x110 only on the ROOT data3D node -> always
    // null (ok=0): the collision object lives on a CHILD node for this vehicle.
    // This pass walks the node tree to find it, then resolves + LOGS the body.
    // The actual world-commit (FUN_14235dce0, REL::ID 125070) is kept OFF
    // (kEnableHavokCommit) until the resolve is verified in the log.

    using HknpSetXform_t = char(__fastcall*)(void* /*world*/, uint32_t /*bodyId*/,
                                             const float* /*xform16*/, uint32_t /*activate*/,
                                             bool /*flagA*/, uint32_t /*flagB*/);
    static REL::Relocation<HknpSetXform_t> g_hknpSetXform{ REL::ID(125070) };
    static constexpr bool kEnableHavokCommit = true;  // veh-havok-v2: resolve verified -> commit ON

    // Cache of the resolved (world, bodyId) per local FURN ref. The body is
    // stable while driving (verified: same world/bodyId every tick), so we
    // resolve via the LockRead + node-tree walk only every 3s and commit from
    // the cache each tick. This avoids the per-tick tree walk that raced the
    // engine's skeleton update in vehhavok-v1 (clone glitch/death). Game-thread
    // only (RunClientTick) -> no mutex.
    struct FurnBodyCache {
        void* world = nullptr; uint32_t bodyId = 0; uint64_t lastResolveMs = 0;
        bool valid = false; bool frozen = false;
        float lastX = 0, lastY = 0, heading = 0;
        bool hasLast = false, hasHeading = false;
    };
    static std::unordered_map<uint32_t, FurnBodyCache> s_furnBody;

    // Per-vehicle interpolation state for jitter smoothing. PKT_VEHICLE_POS
    // arrives at ~5-10 Hz; committing the frozen Havok body only on packet
    // arrival makes the chassis step/jitter. Instead we hold a target (latest
    // packet) and a current (rendered) pose, and every game-thread tick (~60 Hz)
    // ease current toward target and commit THAT. Heading is derived from the
    // TARGET stream (large packet-to-packet deltas = stable) and eased
    // separately, so per-frame micro-steps never feed the atan2. Game-thread
    // only (RunClientTick) -> no mutex.
    struct FurnInterp {
        RE::TESObjectREFR* local = nullptr;
        float curX = 0, curY = 0, curZ = 0;        // rendered (eased) position
        float tgtX = 0, tgtY = 0, tgtZ = 0;        // latest packet target
        float tgtRotZ = 0;                          // host data.angle.z (logical)
        float prevTgtX = 0, prevTgtY = 0;          // for travel-heading derivation
        bool  hasPrevTgt = false;
        float tgtHeading = 0; bool hasTgtHeading = false;  // travel-dir from target stream
        float curHeading = 0; bool hasCurHeading = false;  // eased heading (rendered)
        bool  hasCur = false;
        uint64_t lastPacketMs = 0;
    };
    static std::unordered_map<uint32_t, FurnInterp> s_furnInterp;
    static uint64_t s_lastInterpTickMs = 0;
    static constexpr float    kInterpTauMs   = 80.0f;   // ease time constant (ms); higher = smoother + laggier
    static constexpr float    kInterpSnap    = 30.0f;   // snap (no ease) if target jumps beyond this (cell switch)
    static constexpr uint64_t kInterpStaleMs = 3000;    // stop committing if no packet for this long (host idle/left)

    // veh-havok-v3 experiment: the wheeled-vehicle solver owns the chassis
    // orientation (yaw varies in our commit but the body doesn't visibly turn).
    // Freezing the body to Stationary (motionType 1, native REL::ID 118463 via
    // SafeSetMotionType) should stop the solver from re-deriving orientation so
    // our per-tick setTransform yaw takes. Off-switch in case it breaks the
    // (now working) position follow.
    static constexpr bool kFreezeVehicleBody = true;
    // Yaw offset (rad): added after extracting the heading from the host matrix.
    // 0 first; bump by +-pi/2 if heading is a clean 90deg off, +pi if reversed.
    static constexpr float kYawOffsetRad = -1.5707963f;  // -pi/2: map travel dir onto chassis local +Y forward
    static int SafeSetMotionType(RE::TESObjectREFR* ref, int32_t motionType, bool allowActivate);  // fwd; defined below

    // Raw +0x110 (collisionObject) read - own __try fn (callers hold C++ guards).
    static void* SafeReadNodeColObj(RE::NiAVObject* a_node)
    {
        __try {
            return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(a_node) + 0x110);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // DFS for the first node with a non-null collisionObject. Depth/count capped.
    // No __try here (GetAsNiNode/children are typed); the raw read is in the
    // helper above.
    static RE::NiAVObject* FindNodeWithColObj(RE::NiAVObject* a_node, int a_depth, int& io_count)
    {
        if (!a_node || a_depth > 6 || io_count > 256) return nullptr;
        io_count++;
        if (SafeReadNodeColObj(a_node) != nullptr) return a_node;
        RE::NiNode* asNode = a_node->GetAsNiNode();
        if (!asNode) return nullptr;
        // Torn children array: a node mid-update (observed during combat) can
        // report size>0 while its data pointer is null -> the range-for derefs
        // null. Read data+size and bail on null/empty/insane before iterating.
        auto& kids = asNode->children;
        const uint32_t n = kids.size();
        RE::NiPointer<RE::NiAVObject>* data = kids.data();
        if (!data || n == 0 || n > 4096) return nullptr;
        for (uint32_t i = 0; i < n; ++i) {
            RE::NiAVObject* c = data[i].get();
            if (!c) continue;
            RE::NiAVObject* hit = FindNodeWithColObj(c, a_depth + 1, io_count);
            if (hit) return hit;
        }
        return nullptr;
    }

    // refr -> bhkNPCollisionObject. data3D via the loadedData read-lock (proven
    // in ForceFurnNodeYaw), then the tree walk. a_outNode reports which node.
    static void* SafeGetFurnColObj(RE::TESObjectREFR* a_ref, RE::NiAVObject** a_outNode, bool* a_out3D)
    {
        if (a_outNode) *a_outNode = nullptr;
        if (a_out3D) *a_out3D = false;
        if (!a_ref) return nullptr;
        RE::NiAVObject* root = nullptr;
        {
            auto guard = a_ref->loadedData.LockRead();
            const auto* data = *guard;
            if (data && data->data3D) root = data->data3D.get();
        }
        if (!root) return nullptr;
        if (a_out3D) *a_out3D = true;
        int count = 0;
        RE::NiAVObject* hit = FindNodeWithColObj(root, 0, count);
        if (a_outNode) *a_outNode = hit;
        if (!hit) return nullptr;
        return SafeReadNodeColObj(hit);
    }

    // node-colObj (wrapper, vtRVA 0x4D636B0) -> (hknpWorld*, hknpBodyId).
    // HK-DUMP showed the node's collisionObject is a WRAPPER; the real
    // bhkNPCollisionObject (vtRVA 0x4D63478, what make-Dynamic FUN_142377270
    // operates on) is at wrapper+0x20. The make-Dynamic recipe then applies to
    // that BNP object:
    //   own    = *(BNP+0x20)
    //   idx    = *(int32)(BNP+0x28)
    //   holder = *(own+0x20)
    //   count  = *(holder+0x38); bodyId = ((uint32*)*(holder+0x30))[idx]&0xFFFFFF
    //   world  = *(holder+0x28) - 0x18
    static bool SafeResolveFurnBody(void* a_colObj, void*& a_outWorld, uint32_t& a_outId)
    {
        __try {
            if (!a_colObj) return false;
            void* bnp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(a_colObj) + 0x20);
            if (!bnp) return false;
            auto* b = reinterpret_cast<uint8_t*>(bnp);
            void* own = *reinterpret_cast<void**>(b + 0x20);
            if (!own) return false;
            int32_t idx = *reinterpret_cast<int32_t*>(b + 0x28);
            void* holder = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(own) + 0x20);
            if (!holder) return false;
            auto* hd = reinterpret_cast<uint8_t*>(holder);
            uint32_t count = *reinterpret_cast<uint32_t*>(hd + 0x38);
            if (idx < 0 || static_cast<uint32_t>(idx) >= count) return false;
            auto* idArr = *reinterpret_cast<uint32_t**>(hd + 0x30);
            if (!idArr) return false;
            uint32_t id = idArr[idx] & 0xFFFFFF;
            if (id == 0xFFFFFF) return false;
            void* w1 = *reinterpret_cast<void**>(hd + 0x28);
            if (!w1) return false;
            a_outWorld = reinterpret_cast<uint8_t*>(w1) - 0x18;
            a_outId = id;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Commit a world transform onto the cached hknp body. Yaw-only for now
    // (column-major hkTransform: cols 0..2 rotation basis, col3 translation).
    // SEH-guarded; the cached body could be freed if the vehicle despawns
    // between re-resolves (re-resolve interval bounds the stale window).
    // Build a flat Z-rotation (heading) hkTransform and commit it. The host node
    // matrix is not a usable rotation for this vehicle (degenerate columns), so
    // the heading is derived from travel direction by the caller; here we just
    // apply it. Ground vehicle stays level (no pitch/roll).
    static void SafeHknpCommit(void* a_world, uint32_t a_bodyId,
                               float x, float y, float z, float headingYaw)
    {
        if (!a_world) return;
        __try {
            const float c = std::cos(headingYaw), s = std::sin(headingYaw);
            alignas(16) float xf[16] = {
                 c,    s,    0.0f, 0.0f,
                -s,    c,    0.0f, 0.0f,
                 0.0f, 0.0f, 1.0f, 0.0f,
                 x,    y,    z,    1.0f,
            };
            g_hknpSetXform(a_world, a_bodyId, xf, 0u, false, 1u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Smallest signed angle a-b wrapped to [-pi, pi].
    static float AngleDelta(float a, float b)
    {
        float d = a - b;
        while (d > 3.14159265f)  d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        return d;
    }

    // ===== Ride-Together (soft passenger,) =====
    // Without formally entering the vehicle (which hits the engine occupant-list
    // crash 0x1422C9045), we soft-lock the CLIENT player to the synced vehicle:
    // when the host is driving and the client is near, board, then each commit
    // SetPosition the player onto the seat. The per-tick Havok body commit
    // carries them between updates. Game-thread only (RunClientTick).
    // Enabled by SharedVehicleRide = carry (RidePolicy.h); off by default.
    static bool CarryEnabled()
    {
        return Pulsar::GetConfig().sharedVehicleRide == static_cast<int>(RideMode::kCarry);
    }
    // Game units (~1.4 cm), not metres: these were 4 / 1 / 0.6 "m", which compared
    // against REFR positions meant boarding only within ~6 cm of the vehicle origin.
    static constexpr float kBoardRadius     = 280.0f; // ~4 m: board on a deliberate walk-up
    static constexpr float kSeatUp          = 70.0f;  // ~1 m above the vehicle origin (tune)
    static constexpr float kSeatLateral     = 42.0f;  // ~0.6 m to the side, inside the open cabin
    static bool s_passengerBoarded = false;

    static int SafeSetPlayerPos(RE::PlayerCharacter* p, float x, float y, float z)
    {
        __try {
            RE::NiPoint3 pos(x, y, z);
            p->SetPosition(pos, true);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static void HandlePassenger(RE::TESObjectREFR* a_furn, float vx, float vy, float vz, float heading)
    {
        if (!CarryEnabled()) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        auto pp = p->GetPosition();
        const float dx = pp.x - vx, dy = pp.y - vy, dz = pp.z - vz;
        const float dist2 = dx * dx + dy * dy + dz * dz;
        if (!s_passengerBoarded) {
            if (dist2 < kBoardRadius * kBoardRadius) {
                s_passengerBoarded = true;
                PLOG_INFO("[PULSAR] VehicleSync passenger: BOARDED (dist={:.0f} units)"sv,
                          std::sqrt(dist2));
            } else {
                return;
            }
        }
        // Pure soft-carry: position the player at the passenger seat each tick.
        // Inside the open cabin (small lateral, seat height) -> camera clear, no
        // side-wall collision/ragdoll. NO engine seat (SnapInto fails on vehicle
        // markers and risks the occupant-list crash).
        const float rx = std::sin(heading) * kSeatLateral;   // vehicle right * lateral
        const float ry = -std::cos(heading) * kSeatLateral;
        SafeSetPlayerPos(p, vx + rx, vy + ry, vz + kSeatUp);
    }

    static void PassengerUnboard()
    {
        if (s_passengerBoarded) {
            s_passengerBoarded = false;
            PLOG_INFO("[PULSAR] VehicleSync passenger: unboarded"sv);
        }
    }

    // Per-tick: resolve (cached, refreshed every 3s) then commit the host
    // transform to the vehicle's live Havok body so the rendered chassis follows.
    static void CommitFurnHavokCached(RE::TESObjectREFR* a_ref,
                                      float x, float y, float z, float yaw, const float* a_heading)
    {
        uint32_t id = a_ref->formID;
        auto& c = s_furnBody[id];
        uint64_t now = GetTickCount64();

        // Heading from TRAVEL DIRECTION. The vehicle's own node matrix is
        // degenerate (not a usable rotation) and data.angle.z wobbles, but the
        // host's position delta is the clean, stable forward direction (driver
        // sits front, faces where it drives). With smoothing on, RunClientTick
        // hands us the eased travel-heading directly (a_heading) because per-frame
        // micro-steps are too small for a stable atan2; without it, fall back to
        // deriving from the position delta (reject ~180deg reverse jumps).
        if (a_heading) {
            c.heading = *a_heading; c.hasHeading = true;
        } else if (c.hasLast) {
            const float dx = x - c.lastX, dy = y - c.lastY;
            if (dx * dx + dy * dy > 0.25f) {  // moved > 0.5u
                const float newH = std::atan2(dy, dx);
                if (!c.hasHeading) { c.heading = newH; c.hasHeading = true; }
                else if (std::fabs(AngleDelta(newH, c.heading)) < 2.094f) { c.heading = newH; }  // <120deg = forward turn
            }
        }
        c.lastX = x; c.lastY = y; c.hasLast = true;

        if (!c.valid || now - c.lastResolveMs > 3000) {
            c.lastResolveMs = now;
            RE::NiAVObject* node = nullptr; bool has3D = false;
            void* colObj = SafeGetFurnColObj(a_ref, &node, &has3D);
            void* world = nullptr; uint32_t bodyId = 0;
            if (colObj && SafeResolveFurnBody(colObj, world, bodyId)) {
                c.world = world; c.bodyId = bodyId; c.valid = true;
            } else {
                c.valid = false;
            }
            // Freeze the body once so the wheeled-vehicle solver stops owning the
            // chassis orientation -> our per-tick yaw should then take.
            if (kFreezeVehicleBody && c.valid && !c.frozen) {
                SafeSetMotionType(a_ref, 1 /*Stationary*/, false);
                c.frozen = true;
                c.lastResolveMs = 0;  // re-resolve next tick (body repr may shift after freeze)
            }
        }
        {
            // Rotation tracking: log the travel-derived heading we actually apply,
            // every 3s, so a circle-drive shows the heading sweeping smoothly.
            static uint64_t s_lastLog = 0;
            if (now - s_lastLog > 3000) {
                s_lastLog = now;
                PLOG_INFO("[PULSAR] VehicleSync HK-COMMIT: ref=0x{:08X} valid={} frozen={} bodyId=0x{:06X} pos=({:.1f},{:.1f},{:.1f}) velHeading={:.3f} hasH={} applied={:.3f} eulerYaw={:.3f}"sv,
                          id, c.valid, c.frozen, c.bodyId, x, y, z,
                          c.heading, c.hasHeading, c.heading + kYawOffsetRad, yaw);
            }
        }
        if (c.valid) {
            const float h = (c.hasHeading ? c.heading : yaw) + kYawOffsetRad;
            SafeHknpCommit(c.world, c.bodyId, x, y, z, h);
            HandlePassenger(a_ref, x, y, z, c.hasHeading ? c.heading : yaw);  // seat + carry the client
        }
    }

    static void ApplyPos(RE::TESObjectREFR* a_ref, float a_x, float a_y, float a_z, float a_rotZ,
                         const float* a_heading)
    {
        if (!a_ref) return;
        SafeRefSetPosition(a_ref, a_x, a_y, a_z);
        SafeRefWriteAngleZ(a_ref, a_rotZ);
        // drive the live Havok body so the rendered chassis follows.
        if (kEnableHavokCommit) CommitFurnHavokCached(a_ref, a_x, a_y, a_z, a_rotZ, a_heading);
    }

    static void RunClientTick()
    {
        // 1) Drain pending packets -> update each vehicle's target + travel-heading.
        std::vector<Pulsar::Network::VehiclePosData> batch;
        {
            std::lock_guard lock(s_pendingMutex);
            if (!s_pending.empty()) batch.swap(s_pending);
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;
        auto* cell = player->parentCell;

        uint64_t now = GetTickCount64();

        for (const auto& v : batch) {
            RE::TESObjectREFR* local = nullptr;
            {
                std::lock_guard lock(s_remapMutex);
                auto it = s_remap.find(v.refFormID);
                if (it != s_remap.end()) local = it->second;
            }

            if (!local) {
                RE::NiPoint3 hp{ v.posX, v.posY, v.posZ };
                local = FindLocalFurn(cell, v.baseFormID, hp, 200.0f);
                if (!local) {
                    static uint64_t s_lastMissLog = 0;
                    if (now - s_lastMissLog > 3000) {
                        s_lastMissLog = now;
                        PLOG_INFO("[PULSAR] VehicleSync client: no local FURN match for host-ref=0x{:08X} base=0x{:08X} (retrying)"sv,
                                  v.refFormID, v.baseFormID);
                    }
                    continue;
                }
                {
                    std::lock_guard lock(s_remapMutex);
                    s_remap[v.refFormID] = local;
                }
                PLOG_INFO("[PULSAR] VehicleSync client: matched host-ref=0x{:08X} -> local=0x{:08X} base=0x{:08X}"sv,
                          v.refFormID, local->formID, v.baseFormID);
            }

            auto& it = s_furnInterp[local->formID];
            it.local = local;
            // Travel-heading from the TARGET stream (packet-to-packet deltas are
            // large + stable). Reject ~180deg reverse so backing up keeps fwd.
            if (it.hasPrevTgt) {
                const float dx = v.posX - it.prevTgtX, dy = v.posY - it.prevTgtY;
                if (dx * dx + dy * dy > 0.25f) {
                    const float nh = std::atan2(dy, dx);
                    if (!it.hasTgtHeading) { it.tgtHeading = nh; it.hasTgtHeading = true; }
                    else if (std::fabs(AngleDelta(nh, it.tgtHeading)) < 2.094f) it.tgtHeading = nh;
                }
            }
            it.prevTgtX = v.posX; it.prevTgtY = v.posY; it.hasPrevTgt = true;
            it.tgtX = v.posX; it.tgtY = v.posY; it.tgtZ = v.posZ; it.tgtRotZ = v.rotZ;
            it.lastPacketMs = now;
            if (!it.hasCur) { it.curX = v.posX; it.curY = v.posY; it.curZ = v.posZ; it.hasCur = true; }
            if (!it.hasCurHeading && it.hasTgtHeading) { it.curHeading = it.tgtHeading; it.hasCurHeading = true; }
            // Snap (no ease) on a large jump = cell switch / teleport, not driving.
            const float jx = it.tgtX - it.curX, jy = it.tgtY - it.curY, jz = it.tgtZ - it.curZ;
            if (jx * jx + jy * jy + jz * jz > kInterpSnap * kInterpSnap) {
                it.curX = it.tgtX; it.curY = it.tgtY; it.curZ = it.tgtZ;
                if (it.hasTgtHeading) it.curHeading = it.tgtHeading;
            }
        }

        // 2) Every tick (~60 Hz): ease current -> target and commit, so the
        //    rendered chassis glides smoothly between the ~5-10 Hz packets.
        float dtMs = (s_lastInterpTickMs == 0) ? 16.0f
                                               : static_cast<float>(now - s_lastInterpTickMs);
        s_lastInterpTickMs = now;
        if (dtMs < 0.0f)   dtMs = 0.0f;
        if (dtMs > 200.0f) dtMs = 200.0f;                       // clamp after a frame hitch
        const float ease = 1.0f - std::exp(-dtMs / kInterpTauMs);  // frame-rate independent

        for (auto& [id, it] : s_furnInterp) {
            if (!it.local || !it.hasCur) continue;
            if (now - it.lastPacketMs > kInterpStaleMs) continue;  // host idle/left -> stop driving it
            it.curX += (it.tgtX - it.curX) * ease;
            it.curY += (it.tgtY - it.curY) * ease;
            it.curZ += (it.tgtZ - it.curZ) * ease;
            if (it.hasCurHeading && it.hasTgtHeading) {
                it.curHeading += AngleDelta(it.tgtHeading, it.curHeading) * ease;
            }
            const float* hp = it.hasCurHeading ? &it.curHeading : nullptr;
            ApplyPos(it.local, it.curX, it.curY, it.curZ, it.tgtRotZ, hp);
        }
    }

    void OnGameThreadTick()
    {
        if (IsHostMode()) {
            RunHostTick();
        } else {
            RunClientTick();
        }
    }

    // ---- Remote-seated state machine (client) ---------------------------
    //
    // Protocol: host sets animFlags bit 10 when occupiedFurniture != 0.
    // Client receives this via PKT_POSITION. RemotePlayerManager forwards the
    // flag here along with the clone's current (host-authoritative) position.
    //
    // On 0→1 transition: find the nearest FURN-REFR to the clone across the
    //   clone's parentCell + a wider radius via raw SEH-wrapped scan, call
    //   baseObj->Activate(actionRef=clone) - engine-native sit sequence.
    // While 1: each tick, SetPosition the FURN to the clone's position.
    //   Seat-anchored clone follows the FURN, so as PKT_POSITION moves the
    //   clone, the FURN moves with it and the clone visually stays seated.
    // On 1→0: no-op. Engine stands the clone up once the FURN sit-marker is
    //   no longer held by our drive.

    struct SeatedState {
        bool                 seated          = false;
        RE::TESObjectREFR*   furn            = nullptr;
        uint64_t             lastFurnMoveMs  = 0;
        // Schritt B state:
        bool                 pendingSpawn    = false;   // awaiting Papyrus FURN spawn
        bool                 spawnedByUs     = false;   // we PlaceAtMe'd this FURN → Delete on unseat
        uint32_t             expectedBaseID  = 0;       // filter for post-spawn rescan
        uint64_t             spawnRequestMs  = 0;
    };
    static std::unordered_map<uint32_t, SeatedState> s_seatedByCloneID;
    static std::mutex s_seatedMutex;

    // Dispatch Papyrus PulsarFurnSync.SpawnAndSitForClone(base, x, y, z, angle, clone).
    // Async - the spawned FURN appears after engine processes the call.
    // We re-scan in steady state to pick it up.
    static bool CallPapyrusSpawnAndSit(uint32_t baseFormID,
                                       float x, float y, float z, float angleZ,
                                       RE::Actor* clone)
    {
        if (!clone) return false;
        auto* base = RE::TESForm::LookupByID(baseFormID);
        if (!base) return false;

        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return false;
        auto* vm = gameVM->GetVM();
        if (!vm) return false;

        RE::BSScript::Variable baseVar, xVar, yVar, zVar, angVar, cloneVar;
        RE::BSScript::PackVariable(baseVar, base);
        RE::BSScript::PackVariable(xVar, x);
        RE::BSScript::PackVariable(yVar, y);
        RE::BSScript::PackVariable(zVar, z);
        RE::BSScript::PackVariable(angVar, angleZ);
        RE::BSScript::PackVariable(cloneVar, static_cast<RE::TESObjectREFR*>(clone));

        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(6);
            args[0] = baseVar;
            args[1] = xVar;
            args[2] = yVar;
            args[3] = zVar;
            args[4] = angVar;
            args[5] = cloneVar;
            return true;
        };

        RE::BSFixedString objName("PulsarFurnSync");
        RE::BSFixedString funcName("SpawnAndSitForClone");
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        return vm->DispatchStaticCall(objName, funcName, argFn, callback, 0);
    }

    // Call PulsarFurnSync.DoSetMotionType(ref, motionType).
    // motionType: 1=Dynamic (default), 4=Keyframed (kinematic, teleportable).
    // Flipping the MJ to keyframed on sit lets us SetPosition it without
    // fighting the Havok vehicle physics solver.
    static bool CallPapyrusSetMotionType(RE::TESObjectREFR* ref, int32_t motionType)
    {
        if (!ref) return false;
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return false;
        auto* vm = gameVM->GetVM();
        if (!vm) return false;

        RE::BSScript::Variable refVar, mtVar;
        RE::BSScript::PackVariable(refVar, ref);
        RE::BSScript::PackVariable(mtVar, motionType);

        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(2);
            args[0] = refVar;
            args[1] = mtVar;
            return true;
        };

        RE::BSFixedString objName("PulsarFurnSync");
        RE::BSFixedString funcName("DoSetMotionType");
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        return vm->DispatchStaticCall(objName, funcName, argFn, callback, 0);
    }

    static bool CallPapyrusDespawnFurn(RE::TESObjectREFR* furn)
    {
        if (!furn) return false;
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return false;
        auto* vm = gameVM->GetVM();
        if (!vm) return false;

        RE::BSScript::Variable furnVar;
        RE::BSScript::PackVariable(furnVar, furn);
        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(1);
            args[0] = furnVar;
            return true;
        };
        RE::BSFixedString objName("PulsarFurnSync");
        RE::BSFixedString funcName("DespawnFurn");
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        return vm->DispatchStaticCall(objName, funcName, argFn, callback, 0);
    }

    // Direct engine API - Actor::SnapIntoInteraction(actor, furn). Sets
    // occupiedFurniture handle, validates 3D, triggers sit animation,
    // anchors actor to FURN sit-marker. Bypasses the AI package path that
    // our PerformActionHook was blocking (ActionGetUp was observed in log).
    // REL::ID 97243 @ RVA 0x1799E50, identified via Ghidra scan for the
    // error string "Cannot call SnapIntoInteraction on actors without 3D"
    // and its SnapInto implementation callee.
    using SnapIntoInteraction_t = uint8_t(__fastcall*)(RE::Actor*, RE::TESObjectREFR*);
    static REL::Relocation<SnapIntoInteraction_t> g_snapInto{ REL::ID(97243) };

    // Direct native call - ObjectReference.SetMotionType(motionType, allowActivate).
    // Identified via Ghidra: Papyrus registration at 0x14208E6E0 maps
    // "SetMotionType" to NativeLatentFunction2<TESObjectREFR, void, uint, bool>
    // with handler FUN_142083b40 (REL::ID 118463). Functor Run() at RVA 0x2080600:
    //   motionType == 1 → Stationary: calls bhkColObj.vtable[0x38] via 0x142BE6710,
    //                     sets +0x10+0x88 = 1 "stationary" flag (physics frozen).
    //   motionType == 2 → Dynamic:    calls bhkColObj.vtable[0x40] (normal physics).
    //   Only motionType ∈ {1,2} is accepted; others silently logged + skipped.
    // Bypasses Papyrus VM dispatch entirely (no PEX registration needed).
    using SetMotionType_t = char(__fastcall*)(void*, uint32_t, RE::TESObjectREFR*, int32_t, bool);
    static REL::Relocation<SetMotionType_t> g_setMotionType{ REL::ID(118463) };

    // ObjectReference.SetAngle(float afX, float afY, float afZ) - Papyrus native.
    // Handler found via "SetAngle" string xref at 0x14208D699, function ptr via
    // `lea r9, [rip - 0xae62]` → VA 0x14207F690 = RVA 0x207F690. Verified:
    // entry prolog saves xmm3 (1st float arg = x), mov r14, r8 (Self), mov ebx,
    // edx (stackID), mov rsi, rcx (vm). Args: (vm, stackID, self, x, y, z).
    // Latent - creates SetAngleFunctor queued for Run(). Engine path writes
    // both data.angle AND propagates to Havok body quaternion - exactly what
    // we need for MJ rotation to visually match.
    // (g_setAngle fn ptr is declared up top near Init(); init happens in Init().)

    static int SafeCallSetAngle(RE::TESObjectREFR* a_ref, float a_x, float a_y, float a_z)
    {
        if (!a_ref || !g_setAngle) return 0;
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return 0;
        auto* vm = gameVM->GetVM();
        if (!vm) return 0;
        __try {
            g_setAngle(vm, 0, a_ref, a_x, a_y, a_z);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static int SafeSetMotionType(RE::TESObjectREFR* ref, int32_t motionType, bool allowActivate)
    {
        if (!ref) return 0;
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return 0;
        auto* vm = gameVM->GetVM();
        if (!vm) return 0;
        __try {
            return g_setMotionType(vm, 0, ref, motionType, allowActivate);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static int SafeSnapInto(RE::Actor* a_actor, RE::TESObjectREFR* a_furn)
    {
        __try {
            return g_snapInto(a_actor, a_furn) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Plain-C distance computation to keep __try out of the vector-holding
    // function. Returns squared-distance from (x,y,z) to cand->data.location.
    static float SafeComputeRefDist2(RE::TESObjectREFR* cand,
                                     float ax, float ay, float az)
    {
        __try {
            auto* loc = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(cand) + 0x8C);
            float dx = loc[0] - ax, dy = loc[1] - ay, dz = loc[2] - az;
            return dx * dx + dy * dy + dz * dz;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 1e30f;
        }
    }

    static RE::TESObjectREFR* ScanNPCSyncCells(RE::TESObjectCELL* a_skipCell,
                                               const RE::NiPoint3& a_anchor,
                                               float a_radius,
                                               uint32_t a_baseFilter,
                                               float& io_bestDist2,
                                               int& o_cellsScanned,
                                               int& o_furnsFound)
    {
        auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
        RE::TESObjectREFR* winner = nullptr;
        for (auto* cell : cells) {
            if (!cell || cell == a_skipCell) continue;
            o_cellsScanned++;
            auto* cand = SafeFindNearestFurnInCell(cell, a_anchor.x, a_anchor.y, a_anchor.z,
                                                   a_radius);
            if (!cand) continue;
            float d2 = SafeComputeRefDist2(cand, a_anchor.x, a_anchor.y, a_anchor.z);
            if (d2 >= 1e30f) continue;
            o_furnsFound++;
            if (d2 < io_bestDist2) {
                io_bestDist2 = d2;
                winner = cand;
            }
        }
        return winner;
    }

    // Iterate TESDataHandler.formArrays[kCELL] - every CELL form loaded in
    // memory - filter to attached cells, run the raw FURN scan on each.
    // When a_baseFilter is non-zero, only REFRs with that exact TESFurniture
    // base are accepted - critical for picking the correct Moon Jumper
    // instead of a random nearby bench. a_anchorX/Y/Z is the search center
    // (typically the host's reported FURN position, not the clone's).
    static RE::TESObjectREFR* FindNearestFurnToClone(RE::Actor* a_clone,
                                                     float a_radius,
                                                     uint32_t a_baseFilter,
                                                     float a_anchorX,
                                                     float a_anchorY,
                                                     float a_anchorZ)
    {
        if (!a_clone) return nullptr;
        RE::NiPoint3 p{};
        if (a_anchorX == 0.0f && a_anchorY == 0.0f && a_anchorZ == 0.0f) {
            p = a_clone->GetPosition();
        } else {
            p.x = a_anchorX; p.y = a_anchorY; p.z = a_anchorZ;
        }
        // Clone's parentCell can go stale when we SetPosition data.location
        // directly without a full SetParentCell. The CLIENT player's
        // parentCell is always live - that's where the user is standing and
        // where map-placed FURNs are guaranteed to be loaded.
        RE::TESObjectCELL* primaryCell = a_clone->parentCell;
        auto* localPlayer = RE::PlayerCharacter::GetSingleton();
        if (localPlayer && localPlayer->parentCell && localPlayer->parentCell != primaryCell) {
            primaryCell = localPlayer->parentCell;
        }

        RE::TESObjectREFR* best = nullptr;
        float bestDist2 = a_radius * a_radius;

        // 1. Primary cell scan (client player's parentCell preferred over
        //    clone's - clone's cell pointer goes stale).
        if (primaryCell) {
            best = FindLocalFurn(primaryCell, a_baseFilter, p, a_radius);
            if (best) {
                auto bp = best->GetPosition();
                float dx = bp.x - p.x, dy = bp.y - p.y, dz = bp.z - p.z;
                bestDist2 = dx * dx + dy * dy + dz * dz;
            }
        }

        // 2. Walk every active cell observed by NPCSync's Actor::Update hook.
        // Delegated to helper (moved out of this function because std::vector
        // dtor + __try in same function = C2712).
        {
            int cellsFromNPCSync = 0;
            int furnsViaNPCSync = 0;
            RE::TESObjectREFR* hit = ScanNPCSyncCells(primaryCell, p, a_radius,
                                                     a_baseFilter, bestDist2,
                                                     cellsFromNPCSync, furnsViaNPCSync);
            if (hit) {
                best = hit;
            }
            static uint64_t s_lastCellLog = 0;
            uint64_t clnow = GetTickCount64();
            if (clnow - s_lastCellLog > 1000) {
                s_lastCellLog = clnow;
                PLOG_INFO("[PULSAR] VehicleSync NPCSync-cells: count={} furnsFound={} primary={}"sv,
                          cellsFromNPCSync, furnsViaNPCSync,
                          static_cast<const void*>(primaryCell));
            }
            if (best) return best;  // found via adjacent cells → skip legacy path
        }

        auto* dh = RE::TESDataHandler::GetSingleton();
        {
            static uint64_t s_dhLog = 0;
            uint64_t dnow0 = GetTickCount64();
            if (dnow0 - s_dhLog > 2000) {
                s_dhLog = dnow0;
                PLOG_INFO("[PULSAR] VehicleSync SCAN entry: dh={} parentCell={} fastFurn=0x{:08X}"sv,
                          static_cast<void*>(dh),
                          static_cast<void*>(a_clone->parentCell),
                          best ? *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(best) + 0x28) : 0u);
            }
        }
        if (!dh) return best;

        RE::TESObjectREFR* foundBest = best;
        float foundBestDist2 = bestDist2;

        int cellsScanned = 0;
        int attachedCells = 0;
        int furnsSeen = 0;
        int furnsMatchingBase = 0;
        float nearestAnyDist = 1e30f;
        uint32_t nearestAnyRef = 0;
        uint32_t nearestAnyBase = 0;
        float nearestMatchDist = 1e30f;
        uint32_t nearestMatchRef = 0;

        uint32_t rawSize0 = 0;
        uint32_t rawSize8 = 0;
        uint32_t rawSize4 = 0;
        void*    rawData0 = nullptr;
        void*    rawData8 = nullptr;
        void*    rawData10 = nullptr;
        bool     scanEntered = false;

        // One-shot: confirmed formArrays@+0x80 with 24-byte stride. Dump
        // kCELL (73), kREFR (74), kACHR (75) UNFILTERED to see if they
        // really are empty on client. Also log ProcessLists.highActorHandles
        // so we can iterate active actors → union of parentCells.
        static bool s_dumpedKey = false;
        if (!s_dumpedKey) {
            s_dumpedKey = true;
            __try {
                auto* b = reinterpret_cast<uint8_t*>(dh);
                for (int i : { 73, 74, 75, 76 }) {
                    auto* slot = b + 0x80 + i * 0x18;
                    uint32_t sz = *reinterpret_cast<uint32_t*>(slot + 0x00);
                    uint32_t cap = *reinterpret_cast<uint32_t*>(slot + 0x04);
                    void*    data = *reinterpret_cast<void**>(slot + 0x08);
                    PLOG_INFO("[PULSAR] FormArray[{:3d}] @+0x{:04X}: size={} cap={} data={}"sv,
                              i, 0x80 + i * 0x18, sz, cap, data);
                }
                auto* pl = RE::ProcessLists::GetSingleton();
                if (pl) {
                    auto& hh = pl->highActorHandles;
                    PLOG_INFO("[PULSAR] ProcessLists.highActorHandles size={} cap={} data={}"sv,
                              hh.size(), hh.capacity(),
                              static_cast<const void*>(hh.data()));
                } else {
                    PLOG_WARN("[PULSAR] ProcessLists::GetSingleton() is NULL"sv);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN("[PULSAR] key dump: AV"sv);
            }
        }
        __try {
            auto& cellArray = dh->formArrays[static_cast<size_t>(RE::FormType::kCELL)];
            auto* cb = reinterpret_cast<uint8_t*>(&cellArray.formArray);
            // Log raw bytes at multiple candidate offsets so we can see
            // what the actual BSTArray layout is in 1.16.236.
            rawSize0 = *reinterpret_cast<uint32_t*>(cb + 0x0);
            rawSize4 = *reinterpret_cast<uint32_t*>(cb + 0x4);
            rawSize8 = *reinterpret_cast<uint32_t*>(cb + 0x8);
            rawData0 = *reinterpret_cast<void**>(cb + 0x0);
            rawData8 = *reinterpret_cast<void**>(cb + 0x8);
            rawData10 = *reinterpret_cast<void**>(cb + 0x10);

            uint32_t size = rawSize0;
            auto** data = reinterpret_cast<RE::TESObjectCELL**>(rawData8);
            if (!data || size == 0 || size > 100000) {
                // Log once and fall through so we get the SCAN line too
                static uint64_t s_lastBad = 0;
                uint64_t bnow = GetTickCount64();
                if (bnow - s_lastBad > 2000) {
                    s_lastBad = bnow;
                    PLOG_INFO("[PULSAR] VehicleSync BSTArray RAW: size@+0={} size@+4={} size@+8={} data@+0={} data@+8={} data@+10={}"sv,
                              rawSize0, rawSize4, rawSize8,
                              rawData0, rawData8, rawData10);
                }
                goto scan_done;
            }
            scanEntered = true;

            for (uint32_t i = 0; i < size; ++i) {
                auto* cell = data[i];
                if (!cell) continue;
                cellsScanned++;
                uint8_t state = *reinterpret_cast<uint8_t*>(
                    reinterpret_cast<uint8_t*>(cell) + 0x47);
                if (state != 4) continue;
                attachedCells++;
                if (cell == a_clone->parentCell) continue;

                // references array at cell+0x80: size@+0x80, data@+0x88.
                auto* ccb = reinterpret_cast<uint8_t*>(cell);
                uint32_t refSize = *reinterpret_cast<uint32_t*>(ccb + 0x80);
                auto** refData = *reinterpret_cast<RE::TESObjectREFR***>(ccb + 0x88);
                if (!refData || refSize == 0 || refSize > 10000) continue;
                for (uint32_t j = 0; j < refSize; ++j) {
                    auto* ref = refData[j];
                    if (!ref) continue;
                    auto* base = *reinterpret_cast<uint8_t**>(
                        reinterpret_cast<uint8_t*>(ref) + 0x98);
                    if (!base) continue;
                    uint8_t ft = *reinterpret_cast<uint8_t*>(base + 0x2E);
                    if (ft != static_cast<uint8_t>(RE::FormType::kFURN)) continue;
                    furnsSeen++;
                    uint32_t baseID = *reinterpret_cast<uint32_t*>(base + 0x28);
                    auto* loc = reinterpret_cast<float*>(
                        reinterpret_cast<uint8_t*>(ref) + 0x8C);
                    float dx = loc[0] - p.x, dy = loc[1] - p.y, dz = loc[2] - p.z;
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < nearestAnyDist) {
                        nearestAnyDist = d2;
                        nearestAnyRef = *reinterpret_cast<uint32_t*>(
                            reinterpret_cast<uint8_t*>(ref) + 0x28);
                        nearestAnyBase = baseID;
                    }
                    if (a_baseFilter != 0 && baseID != a_baseFilter) continue;
                    furnsMatchingBase++;
                    if (d2 < nearestMatchDist) {
                        nearestMatchDist = d2;
                        nearestMatchRef = *reinterpret_cast<uint32_t*>(
                            reinterpret_cast<uint8_t*>(ref) + 0x28);
                    }
                    if (d2 < foundBestDist2) {
                        foundBestDist2 = d2;
                        foundBest = ref;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    scan_done:

        static uint64_t s_lastDiag = 0;
        uint64_t dnow = GetTickCount64();
        if (dnow - s_lastDiag > 1000) {
            s_lastDiag = dnow;
            float nearestAnyM = (nearestAnyDist < 1e30f) ? std::sqrt(nearestAnyDist) : -1.0f;
            float nearestMatchM = (nearestMatchDist < 1e30f) ? std::sqrt(nearestMatchDist) : -1.0f;
            PLOG_INFO("[PULSAR] VehicleSync SCAN: cells={} attached={} furnsSeen={} matchBase={} nearestAny=0x{:08X}(base=0x{:08X} @{:.1f}m) nearestMatch=0x{:08X}@{:.1f}m filter=0x{:08X} anchor=({:.0f},{:.0f},{:.0f})"sv,
                      cellsScanned, attachedCells, furnsSeen, furnsMatchingBase,
                      nearestAnyRef, nearestAnyBase, nearestAnyM,
                      nearestMatchRef, nearestMatchM,
                      a_baseFilter,
                      p.x, p.y, p.z);
        }

        return foundBest;
    }

    // SEH-wrapped helpers - one per anim event. Each helper owns ONE
    // BSFixedString (destructor → C2712 if combined with __try in same
    // function), so we split. NotifyAnimationGraphImpl can crash if the
    // actor graph is in transient state, hence __try.
    static void SafeNotifyAnimImpl(RE::Actor* a_clone, const RE::BSFixedString& a_evt)
    {
        if (!a_clone) return;
        __try {
            a_clone->NotifyAnimationGraphImpl(a_evt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void SafeFireStandUpAnim(RE::Actor* a_clone)
    {
        if (!a_clone) return;
        // Declare BSFixedStrings here (outside __try); helper uses __try.
        RE::BSFixedString e1("GetUpStart");
        RE::BSFixedString e2("ChairGetUp");
        RE::BSFixedString e3("ExitFurniture");
        SafeNotifyAnimImpl(a_clone, e1);
        SafeNotifyAnimImpl(a_clone, e2);
        SafeNotifyAnimImpl(a_clone, e3);
    }

    // Direct memory clear of occupiedFurniture handle on the clone Actor.
    // Layout: Actor+0x228 → AIProcess*, +0x08 → MiddleHighProcessData*,
    // +0x428 = uint32_t handle. Setting to 0 = "not seated" engine-side.
    static void SafeClearFurnHandle(RE::Actor* a_clone)
    {
        if (!a_clone) return;
        __try {
            auto* a = reinterpret_cast<std::uint8_t*>(a_clone);
            auto* proc = *reinterpret_cast<std::uint8_t**>(a + 0x228);
            if (proc) {
                auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x08);
                if (mh) {
                    *reinterpret_cast<std::uint32_t*>(mh + 0x428) = 0;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Try SnapIntoInteraction with nullptr furn - likely inverse of seat-snap.
    static void SafeSnapOut(RE::Actor* a_clone)
    {
        if (!a_clone) return;
        __try {
            g_snapInto(a_clone, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void OnRemoteSeatedUpdate(RE::Actor* a_clone, bool a_seated,
                              float a_x, float a_y, float a_z)
    {
        // BISECT REVERT 15:25 - keep the state-map update path so
        // IsCloneSeated() returns true and RemotePlayer::ProcessPending
        // skips the per-tick clone SetPosition (= no clone-on-chair
        // conflict with local player). Skip ONLY the SafeSetMotionType
        // call (the "Dynamic FURN" flag that blocked local sit-down).
        if (!a_clone) return;

        // Heartbeat log so we can see whether the call is reaching us at all.
        static uint64_t s_lastCallLog = 0;
        static bool s_lastCallSeated = false;
        uint64_t cnow = GetTickCount64();
        if (a_seated != s_lastCallSeated || cnow - s_lastCallLog > 5000) {
            s_lastCallLog = cnow;
            s_lastCallSeated = a_seated;
            PLOG_INFO("[PULSAR] VehicleSync recv: clone=0x{:08X} seated={} at=({:.1f},{:.1f},{:.1f})"sv,
                      a_clone->formID, a_seated, a_x, a_y, a_z);
        }

        std::lock_guard lock(s_seatedMutex);
        auto& st = s_seatedByCloneID[a_clone->formID];

        static uint64_t s_lastStLog = 0;
        uint64_t stLogNow = GetTickCount64();
        if (stLogNow - s_lastStLog > 2000) {
            s_lastStLog = stLogNow;
            PLOG_INFO("[PULSAR] VehicleSync state: clone=0x{:08X} st.seated={} st.pendingSpawn={} st.furn={} a_seated={}"sv,
                      a_clone->formID, st.seated, st.pendingSpawn,
                      static_cast<void*>(st.furn), a_seated);
        }

        // clone seating RE-ENABLED. The vehicle-follow is driven by the
        // FURN sit-anchor (SnapInto + the steady-state FURN-position drive below);
        // disabling seating (/) killed that follow → clone no longer
        // tracked the moving vehicle. The actual crash (engine CondEval walking a
        // DANGLING list at obj+0xA0 during furniture-enter, RVA 0x140B7D69D /
        // 0x1422CC350 family - dangling ref from clone despawn churn, NOT the
        // furniture occupant) is now caught by CrashGuard Guard #4 (SEH shield),
        // independent of seating, so seating is safe again. Flag kept for a quick
        // off-switch. Proper despawn de-register is post-Alpha.
        static const bool kEnableCloneSeating = true;
        if (!kEnableCloneSeating) {
            st.seated       = false;  // → IsCloneSeated()=false → RemotePlayer keeps driving pos
            st.pendingSpawn = false;
            st.spawnedByUs  = false;
            st.furn         = nullptr;
            return;
        }

        // 1 → 0: clear state AND fire stand-up animation event on the clone.
        // Without the anim event, the clone stays visually snapped into the
        // seat pose (because SnapInto on 0→1 didn't auto-revert). Engine
        // sees the seat as still-occupied → blocks other actors (including
        // the local player) from sitting. L.3 V4 pilot-handover requires
        // this transition.
        if ((st.seated || st.pendingSpawn) && !a_seated) {
            PLOG_INFO("[PULSAR] VehicleSync: clone 0x{:08X} unseated (spawnedByUs={}) - firing GetUp anim"sv,
                      a_clone->formID, st.spawnedByUs);
            PassengerUnboard();  // host stopped driving -> release the soft passenger

            // L.7 diag - capture client engine state right at the
            // moment host's clone unseats. Per user reports, this is when
            // the local-client pilot lock releases, so diffing diag values
            // before/after this call should reveal the lock field.
            Pulsar::Diag293::OnHostCloneUnseated();

            // L.3 V4 motionType=Stationary restore SKIPPED 15:25 - pairs
            // with the 0→1 SafeSetMotionType skip below. We never set
            // Dynamic in the first place, so no need to restore Stationary.
            // if (st.furn) {
            //     int mtRestoreOk = SafeSetMotionType(st.furn, 1, false);
            //     PLOG_INFO(...)
            // }

            st.seated = false;
            st.pendingSpawn = false;
            st.spawnedByUs = false;
            st.furn = nullptr;
            st.lastFurnMoveMs = 0;
            st.expectedBaseID = 0;
            st.spawnRequestMs = 0;

            // Plan C: forced respawn. The previously-working version of this
            // sync exhibited "host clone despawned briefly, then re-spawned
            // standing" behavior - that's exactly RequestForceRespawn.
            // Despawns clone now, respawns ~2s later in fresh standing pose.
            //
            // We previously also tried SafeFireStandUpAnim, SafeSnapOut, and
            // SafeClearFurnHandle - they all fired without crashing but
            // didn't help (anim events were wrong names, SnapInto(nullptr)
            // was likely a no-op or worse, manual handle-clear redundant).
            // Removed all three: the engine state clears via ForceRespawn
            // alone (clone removed → re-spawned fresh = identical effect).
            PLOG_INFO("[PULSAR] VehicleSync: triggering ForceRespawn on clone unseat"sv);
            Pulsar::RemotePlayer::GetRemotePlayerManager().RequestForceRespawn();
            return;
        }

        // 0 → 1: match a local FURN with the SAME base as host's FURN
        // (Schritt A of the A+B plan). Pulls host's cross-plugin-resolved
        // base and position from s_hostFurn (populated by PKT_VEHICLE_POS).
        if (!st.seated && a_seated) {
            uint32_t hostBaseResolved = 0;
            float hostPosX = a_x, hostPosY = a_y, hostPosZ = a_z;
            {
                std::lock_guard lock(s_hostFurnMutex);
                hostBaseResolved = s_hostFurn.resolvedBaseID;
                if (s_hostFurn.lastUpdateMs != 0) {
                    hostPosX = s_hostFurn.posX;
                    hostPosY = s_hostFurn.posY;
                    hostPosZ = s_hostFurn.posZ;
                }
            }

            // BISECT FIX 15:30 - set seated=true EARLY so RemotePlayer's
            // position-apply skip kicks in IMMEDIATELY. Without this, the
            // clone gets teleported onto the chair (where local player sits)
            // and the engine sees 2 actors on 1 chair → both stuck, neither
            // can stand up. Even before FURN match resolves, marking seated
            // freezes the clone in place, preventing the chair conflict.
            st.seated = true;
            st.spawnedByUs = false;
            st.lastFurnMoveMs = 0;

            // Without a resolved base we can't tell the MJ apart from a
            // bench. Log and bail - Schritt B (PlaceAtMe fallback) will
            // take over once implemented; for now we stay passive.
            if (hostBaseResolved == 0) {
                static uint64_t s_lastNoBase = 0;
                uint64_t now = GetTickCount64();
                if (now - s_lastNoBase > 3000) {
                    s_lastNoBase = now;
                    PLOG_INFO("[PULSAR] VehicleSync: seated but no resolved host base yet (PKT_VEHICLE_POS pending?)"sv);
                }
                return;
            }

            // Map-placed FURN (like Moon Jumper in Bondar) has the SAME base
            // form on both machines. If client has the MJ in its parentCell
            // at all, it IS the same vehicle - match by base with huge
            // radius so distance from host doesn't gate it.
            auto* furn = FindNearestFurnToClone(a_clone, 10000.0f, hostBaseResolved,
                                                hostPosX, hostPosY, hostPosZ);
            if (furn) {
                // Schritt A hit - reuse client's existing FURN.
                auto base = furn->GetBaseObject();
                uint32_t baseID = base ? base->GetFormID() : 0;
                auto furnPos = furn->GetPosition();
                float dfx = furnPos.x - hostPosX, dfy = furnPos.y - hostPosY, dfz = furnPos.z - hostPosZ;
                float furnDist = std::sqrt(dfx * dfx + dfy * dfy + dfz * dfz);

                // Dump BEFORE motion/snap - baseline client-side MJ state.
                SafeDumpFurnDiag("CLIENT-pre-snap", furn, a_clone);

                // BISECT 15:25 - SafeSetMotionType(2/Dynamic) SKIPPED.
                // Setting Dynamic on FURN blocks LOCAL player from sitting
                // (Bethesda's sit-validation refuses Dynamic FURNs). With
                // both peers sharing a static cell, leaving FURN Stationary
                // means clone won't follow ship motion in flight, but local
                // player can sit/stand normally. Visual ship-flight sync of
                // clone-on-chair remains broken - separate fix.
                int mtOk = -1;  // intentionally not setting motionType
                int snapOk = SafeSnapInto(a_clone, furn);
                // Dump AFTER snap - see what SnapInto+motionType changed.
                SafeDumpFurnDiag("CLIENT-post-snap", furn, a_clone);
                PLOG_INFO("[PULSAR] VehicleSync: [A] SnapInto clone=0x{:08X} -> furn=0x{:08X} base=0x{:08X} dist={:.1f}m motion(skipped)={} snap={}"sv,
                          a_clone->formID, furn->formID, baseID, furnDist, mtOk, snapOk);
                st.seated = true;
                st.spawnedByUs = false;
                st.furn = furn;
                st.lastFurnMoveMs = 0;
                return;
            }

            // Schritt B is disabled - `CallPapyrusSpawnAndSit` triggered an
            // IDDB REL::ID=0 resolution fail inside BSScriptUtil when the
            // TESForm pack path hit a stale 1.16.236 binding (dll popup:
            // "Rel/IDDB.cpp 417 failed to find offset ... Invalid id: 0").
            // Until we either move the spawn to GLOB-polling (like NPCSync
            // Phase B's trampoline) or hunt down the specific zero REL::ID,
            // silently give up when A can't match.
            static uint64_t s_lastNoB = 0;
            uint64_t nnow = GetTickCount64();
            if (nnow - s_lastNoB > 3000) {
                s_lastNoB = nnow;
                PLOG_INFO("[PULSAR] VehicleSync: [A] no match, [B] disabled - vehicle unsynced this pass"sv);
            }
            return;
        }

        // Steady state - still seated. Two cases:
        // (a) pendingSpawn: re-scan for the FURN we requested. The Papyrus
        //     PlaceAtMe completes asynchronously, usually within a frame or
        //     two. Once we find it, we latch st.furn and mark spawnedByUs.
        // (b) active match: SetPosition the FURN to host position so the
        //     seat-anchored clone follows.
        if (st.pendingSpawn && !st.furn) {
            auto* furn = FindNearestFurnToClone(a_clone, 10.0f, st.expectedBaseID,
                                                a_x, a_y, a_z);
            if (furn) {
                st.furn = furn;
                st.seated = true;
                st.spawnedByUs = true;
                st.pendingSpawn = false;
                st.lastFurnMoveMs = 0;
                PLOG_INFO("[PULSAR] VehicleSync: [B] spawn resolved clone=0x{:08X} furn=0x{:08X}"sv,
                          a_clone->formID, furn->formID);
            } else if (GetTickCount64() - st.spawnRequestMs > 5000) {
                // Spawn didn't complete within 5 s - give up.
                PLOG_WARN("[PULSAR] VehicleSync: [B] spawn timeout, abandoning"sv);
                st.pendingSpawn = false;
                st.expectedBaseID = 0;
            }
            return;
        }

        // Steady state (seated). Drive FURN position to host's AUTHORITATIVE
        // FURN position (from PKT_VEHICLE_POS), NOT the clone-actor pos -
        // the actor sits at a seat-offset from the FURN origin, so driving
        // the FURN to the actor pos would be ~1-2m off. Seat-anchoring puts
        // the clone at seat-offset from FURN, which on client yields the
        // correct actor world pos if FURN is at host-FURN pos.
        // No time-throttle - every PKT_POSITION-driven update teleports.
        if (st.seated && a_seated && st.furn) {
            float fx = a_x, fy = a_y, fz = a_z;
            float rx = 0.0f, ry = 0.0f, rz = 0.0f;
            bool haveRot = false;
            {
                std::lock_guard lockH(s_hostFurnMutex);
                if (s_hostFurn.lastUpdateMs != 0) {
                    fx = s_hostFurn.posX;
                    fy = s_hostFurn.posY;
                    fz = s_hostFurn.posZ;
                    rx = s_hostFurn.rotX;
                    ry = s_hostFurn.rotY;
                    rz = s_hostFurn.rotZ;
                    haveRot = true;
                }
            }
            uint64_t now = GetTickCount64();
            st.lastFurnMoveMs = now;
            int ok = SafeRefSetPosition(st.furn, fx, fy, fz);
            // Read angle.z BEFORE and AFTER write so we can see if engine
            // reverts our write immediately. Plain-C helper for SEH safety.
            float angleBefore = 0.0f, angleAfter = 0.0f;
            int angleOk = 0;
            if (haveRot) {
                // SetAngle Papyrus-native call DISABLED - triggered client
                // game-thread freeze (log thread kept logging ~30s after
                // disconnect, no DMP = not a hard exception, latent-functor
                // deadlock likely). Keep the fn pointer resolved for later
                // investigation but don't call it.
                angleBefore = SafeRefReadAngleZ(st.furn);
                angleOk = SafeRefWriteAngleFull(st.furn, rx, ry, rz);
                angleAfter = SafeRefReadAngleZ(st.furn);
                static uint64_t s_lastNodeDiag = 0;
                bool doNodeDiag = (now - s_lastNodeDiag > 1000);
                if (doNodeDiag) s_lastNodeDiag = now;
                ForceFurnNodeYawDiag(st.furn, rz, doNodeDiag);
            }
            static uint64_t s_lastFurnLog = 0;
            if (now - s_lastFurnLog > 1000) {
                s_lastFurnLog = now;
                float curX = 0, curY = 0, curZ = 0;
                SafeReadRefPos(st.furn, curX, curY, curZ);
                float dx = curX - fx, dy = curY - fy, dz = curZ - fz;
                float drift = std::sqrt(dx * dx + dy * dy + dz * dz);
                PLOG_INFO("[PULSAR] VehicleSync FURN-follow: ref=0x{:08X} setPos=({:.1f},{:.1f},{:.1f}) actualPos=({:.1f},{:.1f},{:.1f}) drift={:.1f}m setResult={} rotZ-target={:.3f} before={:.3f} after={:.3f} ok={}"sv,
                          st.furn->formID, fx, fy, fz,
                          curX, curY, curZ, drift, ok,
                          rz, angleBefore, angleAfter, angleOk);
                // Periodic dump of MJ internals on client - see if Havok/engine
                // flips any fields between our SetPosition calls.
                SafeDumpFurnDiag("CLIENT-steady", st.furn, a_clone);
            }
        }
    }

    bool IsHostMode()
    {
        return Pulsar::Network::GetSingleton().IsHost();
    }

    bool IsCloneSeated(RE::Actor* a_clone)
    {
        if (!a_clone) return false;
        std::lock_guard lock(s_seatedMutex);
        auto it = s_seatedByCloneID.find(a_clone->formID);
        return (it != s_seatedByCloneID.end()) && it->second.seated;
    }

    void Reset()
    {
        std::lock_guard lock(s_seatedMutex);
        size_t n = s_seatedByCloneID.size();
        s_seatedByCloneID.clear();
        PLOG_INFO(
            "[PULSAR] VehicleSync: Reset() - cleared {} cached clone/FURN entries"sv,
            n);
    }

    // ── Shared vehicle entry guard (SharedVehicleRide, RidePolicy.h) ──

    static void NotifyHud(const char* a_msg)
    {
        auto* game = RE::GameVM::GetSingleton();
        auto* vm = game ? game->GetVM() : nullptr;
        if (!vm) return;
        RE::BSScript::Variable msg;
        RE::BSScript::PackVariable(msg, RE::BSFixedString(a_msg));
        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(1);
            args[0] = msg;
            return true;
        };
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        vm->DispatchStaticCall(RE::BSFixedString("Debug"), RE::BSFixedString("Notification"),
                               argFn, callback, 0);
    }

    bool ShouldBlockClientEntry(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref) return false;
        auto& net = Pulsar::Network::GetSingleton();
        const bool isClient = net.IsConnected() && !net.IsHost();
        if (!isClient) return false;
        const auto mode = static_cast<RideMode>(Pulsar::GetConfig().sharedVehicleRide);
        if (mode == RideMode::kEnter) return false;

        HostVehicle host;
        {
            std::lock_guard lock(s_hostFurnMutex);
            host.baseID       = s_hostFurn.resolvedBaseID;
            host.x            = s_hostFurn.posX;
            host.y            = s_hostFurn.posY;
            host.z            = s_hostFurn.posZ;
            host.lastUpdateMs = s_hostFurn.lastUpdateMs;
        }
        auto* base = a_ref->data.objectReference.get();
        const std::uint32_t baseID = base ? base->formID : 0;
        const auto& p = a_ref->data.location;
        if (!ShouldBlockEntry(mode, true, baseID, p.x, p.y, p.z, host, GetTickCount64()))
            return false;

        static std::uint64_t s_lastMsgMs = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - s_lastMsgMs > 5000) {
            s_lastMsgMs = now;
            PLOG_DIAG("[PULSAR] co-op: refused entering the host's vehicle (SharedVehicleRide = {})"sv,
                      RideModeName(mode));
            NotifyHud(mode == RideMode::kCarry
                ? "PULSAR: stand next to the vehicle to ride along as a passenger"
                : "PULSAR: riding the host's vehicle is disabled (known crash). Use your own vehicle.");
        }
        return true;
    }
}
