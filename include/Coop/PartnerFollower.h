#pragma once

// Game-thread side of Coop/FollowerBonus.h: while the co-op partner's clone is
// in the same cell and within kNearMetres, the game counts one more human
// follower (Extrovert / Introvert see the partner). Setting:
// CoopPartnerCountsAsFollower (default on).
namespace Pulsar::Coop::PartnerFollower {

    inline constexpr float kNearMetres = 40.0f;

    // Main.cpp game-thread tick.
    void Tick(bool a_enabled);

    // A save bridge arrived: the global will come from the host's save.
    void OnSaveLoaded();
}
