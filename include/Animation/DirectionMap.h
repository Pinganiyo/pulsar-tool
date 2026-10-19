#pragma once

// The "Direction" animation variable (movement direction relative to facing).
//
// Read from Starfield's own graph data (Starfield - Animations.ba2,
// AnimTextData/Tables/Graphs/Biped_ReadyLocomotion.agx): every human - player
// and NPC share Human_Root.agx - uses Direction as a FRACTION OF A TURN in
// [0, 1): ~0 / ~1 forward, 0.4-0.6 backwards, the side ranges in between. The
// locomotion blend nodes say "Direction Is Radians: False".
//
// - The partner clone gets the host PLAYER's Direction unchanged: already the
//   right unit.
// - NPCSync computed atan2(velocity) - yaw in RADIANS and wrote that straight
//   into Direction (original 0.3.0 code), so a sideways NPC (~1.57) sent a value
//   outside [0, 1) - the "stares one way and jogs" its comments describe.
//   RadiansToDirection() converts.
// Game-free (tests/test_direction_map.cpp).

#include <cmath>

namespace Pulsar::Animation {

    inline constexpr float kPi = 3.14159265358979f;

    inline float WrapPi(float a)
    {
        while (a > kPi) a -= 2.0f * kPi;
        while (a < -kPi) a += 2.0f * kPi;
        return a;
    }

    // Movement direction relative to facing, radians, Bethesda yaw convention
    // (heading = atan2(dx, dy): 0 = +Y, clockwise). 0 = forward, +pi/2 = right.
    inline float RelativeMoveAngle(float a_dx, float a_dy, float a_facingRotZ)
    {
        return WrapPi(std::atan2(a_dx, a_dy) - a_facingRotZ);
    }

    // Radians relative to facing -> the graph's Direction fraction in [0, 1).
    inline float RadiansToDirection(float a_rad)
    {
        float f = WrapPi(a_rad) / (2.0f * kPi);   // (-0.5, 0.5]
        if (f < 0.0f) f += 1.0f;
        if (f >= 1.0f) f -= 1.0f;
        return f;
    }

    // Is a graph Direction value moving (mostly) forward? Mirrors the graph's
    // own forward test: Direction >= 0.9 or <= 0.1.
    inline bool IsForwardDirection(float a_direction)
    {
        return a_direction >= 0.9f || a_direction <= 0.1f;
    }

    // Backwards per the graph: 0.4 <= Direction <= 0.6.
    inline bool IsBackwardDirection(float a_direction)
    {
        return a_direction >= 0.4f && a_direction <= 0.6f;
    }
}
