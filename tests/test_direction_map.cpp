// The "Direction" animation variable (include/Animation/DirectionMap.h).

#include "TestHarness.h"

#include "Animation/DirectionMap.h"

#include <cmath>

using namespace Pulsar::Animation;

namespace {
    bool Near(float a, float b) { return std::fabs(WrapPi(a - b)) < 0.001f; }
    bool NearF(float a, float b) { return std::fabs(a - b) < 0.001f; }
}

TEST(DirectionMap, RelativeMoveAngleUsesBethesdaYaw)
{
    // Facing north (rotZ 0): moving +Y is forward, +X is right, -Y is back.
    CHECK(Near(RelativeMoveAngle(0, 1, 0), 0));
    CHECK(Near(RelativeMoveAngle(1, 0, 0), kPi / 2));
    CHECK(Near(RelativeMoveAngle(-1, 0, 0), -kPi / 2));
    CHECK(Near(RelativeMoveAngle(0, -1, 0), kPi));
    CHECK(Near(RelativeMoveAngle(1, 0, kPi / 2), 0));   // facing east, moving east
}

TEST(DirectionMap, RadiansBecomeTheGraphsFraction)
{
    CHECK(NearF(RadiansToDirection(0.0f), 0.0f));
    CHECK(NearF(RadiansToDirection(kPi / 2), 0.25f));
    CHECK(NearF(RadiansToDirection(kPi), 0.5f));
    CHECK(NearF(RadiansToDirection(-kPi / 2), 0.75f));
    CHECK(NearF(RadiansToDirection(-0.1f), 1.0f - 0.1f / (2 * kPi)));
    for (float a = -12.0f; a <= 12.0f; a += 0.29f) {
        const float d = RadiansToDirection(a);
        CHECK(d >= 0.0f && d < 1.0f);
    }
}

TEST(DirectionMap, GraphForwardAndBackwardBands)
{
    // The graph's own tests (Biped_ReadyLocomotion.agx).
    CHECK(IsForwardDirection(RadiansToDirection(0.0f)));
    CHECK(IsForwardDirection(RadiansToDirection(-0.2f)));   // slightly left of forward
    CHECK(IsBackwardDirection(RadiansToDirection(kPi)));
    CHECK(!IsForwardDirection(RadiansToDirection(kPi / 2)));
    CHECK(!IsBackwardDirection(RadiansToDirection(kPi / 2)));
    // The old bug: radians written raw. A sideways NPC sent 1.57, which is
    // neither a valid fraction nor "sideways" for the graph.
    CHECK(!(1.57f >= 0.0f && 1.57f < 1.0f));
}

TEST(DirectionMap, WrapStaysInRange)
{
    for (float a = -20.0f; a <= 20.0f; a += 0.37f) {
        const float w = WrapPi(a);
        CHECK(w <= kPi && w >= -kPi);
        CHECK(Near(w, a));
    }
}
