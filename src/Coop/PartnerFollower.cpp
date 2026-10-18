#include "PCH.h"
#include "Coop/PartnerFollower.h"

#include "Coop/FollowerBonus.h"
#include "Network/NetworkHost.h"
#include "RemotePlayer/RemotePlayerManager.h"

#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESGlobal.h>

#include <Windows.h>

using namespace std::string_view_literals;

namespace Pulsar::Coop::PartnerFollower {

    namespace {
        RE::TESGlobal*     g_countFollowing = nullptr;
        std::uint64_t      g_lastTickMs     = 0;
        constexpr std::uint64_t kTickMs     = 1000;   // the follower script also runs on a timer

        // Same cell and within kNearMetres. SEH: the clone pointer can go stale
        // between RemotePlayerManager's refreshes.
        bool SafePartnerNear(RE::PlayerCharacter* a_player)
        {
            __try {
                RE::Actor* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
                if (!clone || !a_player->parentCell || clone->parentCell != a_player->parentCell)
                    return false;
                const RE::NiPoint3 pp = a_player->GetPosition();
                const RE::NiPoint3 cp = clone->GetPosition();
                const float dx = cp.x - pp.x, dy = cp.y - pp.y, dz = cp.z - pp.z;
                return dx * dx + dy * dy + dz * dz <= kNearMetres * kNearMetres;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        RE::TESGlobal* CountFollowing()
        {
            if (!g_countFollowing) {
                g_countFollowing = RE::TESForm::LookupByEditorID<RE::TESGlobal>(
                    RE::BSFixedString("PlayerFollowers_CountFollowing"));
            }
            return g_countFollowing;
        }
    }

    void Tick(bool a_enabled)
    {
        const std::uint64_t now = GetTickCount64();
        if (now - g_lastTickMs < kTickMs) return;
        g_lastTickMs = now;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* global = CountFollowing();
        if (!player || !global) return;

        const bool want = a_enabled && Pulsar::Network::GetSingleton().IsConnected() &&
                          SafePartnerNear(player);
        if (auto v = FollowerBonus::Tick(want, global->value)) {
            PLOG_INFO("[PULSAR] PartnerFollower: PlayerFollowers_CountFollowing {} -> {} ({})"sv,
                      global->value, *v, want ? "partner near" : "partner not near");
            global->value = *v;
        }
    }

    void OnSaveLoaded()
    {
        g_countFollowing = nullptr;   // re-resolve after the load; values self-identify (x.5)
    }
}
