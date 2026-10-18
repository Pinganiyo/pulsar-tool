#pragma once

// "My co-op partner counts as a human follower" - so traits like Extrovert /
// Introvert (and anything else using TRAIT_CND_HasHumanFollower) see the partner.
//
// The vanilla condition (Starfield.esm, CNDF 0x00147E2E) is
//   PlayerFollowers_CountFollowing >= 1 AND
//   (Vasco.FOL_ActiveFollower == 0 OR PlayerFollowers_CountFollowing >= 2)
// The partner's clone is not a recruited follower, so it never counts. While
// the partner is near, PULSAR raises PlayerFollowers_CountFollowing.
//
// SQ_FollowersScript owns that global: it recounts the active followers and
// SetValueInt()s the total (on follower changes, location changes, a timer), so
// it can overwrite our change at any time - including with a value equal to the
// one we wrote. To tell the two apart we add 1.5 instead of 1: the script only
// ever writes whole numbers, so a value ending in .5 is always ours. The
// condition only compares >= 1 and >= 2, so x.5 behaves exactly like x+1, and a
// script reading it as an int gets x+1. No other record condition and no other
// script reads this global. A .5 value caught in a save is recognised and
// removed the next time the partner is not near. Game-free
// (tests/test_follower_bonus.cpp).

#include <cmath>
#include <optional>

namespace Pulsar::Coop {

    class FollowerBonus {
    public:
        static constexpr float kMark = 1.5f;

        static bool IsOurs(float a_value)
        {
            const float frac = a_value - std::floor(a_value);
            return std::fabs(frac - 0.5f) < 0.01f;
        }

        // a_want: partner near right now. a_current: the global's value.
        // Returns the value to write, or nothing.
        static std::optional<float> Tick(bool a_want, float a_current)
        {
            const bool ours = IsOurs(a_current);
            if (a_want) {
                // Not marked = first apply, or the follower script recounted.
                if (!ours) return a_current + kMark;
                return std::nullopt;
            }
            if (ours) {
                const float base = a_current - kMark;
                return base < 0.0f ? 0.0f : base;
            }
            return std::nullopt;   // the script's own count: never ours to change
        }
    };
}
