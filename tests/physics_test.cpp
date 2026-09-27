// Deterministic tests for physics.hpp. No Hyprland needed:
//   g++ -std=c++2b -I.. physics_test.cpp -o physics_test && ./physics_test
#include "../physics.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int  g_failed = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failed;
}
static bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

using namespace Physics;

// Finger moving at 1 unit/ms, reported every 7 ms for 70 ms, lifted 7 ms after
// the last report (one missed report, inside the auto cap).
static CVelocityEstimator constantFlick(uint32_t t0) {
    CVelocityEstimator est;
    for (int i = 0; i < 11; ++i)
        est.add(t0 + 7 * i, true, 7.0);
    return est;
}

int main() {
    // 1. Launch velocity tracks the finger for a clean lift.
    {
        auto est = constantFlick(1000);
        auto l   = est.launch(1000 + 70 + 7, 64, 0, 1.0);
        check(l.tailMs == 7 && l.capMs == 10, "auto cap = 1.5 report intervals (7 ms -> 10 ms)");
        check(l.v > 0.85 && l.v <= 1.0, "clean lift launches near finger speed");
        check(l.h == 0.0, "no horizontal component");
    }

    // 2. A long tail (fingers resting) launches nothing.
    {
        auto est = constantFlick(1000);
        auto l   = est.launch(1000 + 70 + 300, 64, 0, 1.0);
        check(l.v == 0.0 && l.tailMs == 300, "pause before lift -> no launch");
    }

    // 3. Tail inside the grace zone is free; beyond it the launch fades.
    {
        auto est  = constantFlick(1000);
        auto l14  = est.launch(1000 + 70 + 14, 64, 0, 1.0);
        auto l20  = est.launch(1000 + 70 + 20, 64, 0, 1.0);
        auto l40  = est.launch(1000 + 70 + 40, 64, 0, 1.0);
        auto l64  = est.launch(1000 + 70 + 64, 64, 0, 1.0);
        // Both inside the grace zone: full launch (the tiny difference is the
        // window covering a different set of reports).
        check(l14.v > 0.8 && l20.v > 0.8 && near(l14.v, l20.v, 0.05), "tail 14 and 20 ms (<= 2 caps) both launch fully");
        check(l40.v < l20.v && l40.v > 0.0, "tail 40 ms launches slower");
        check(near(l64.v, 0.0, 1e-9), "tail at window edge fades to zero");
    }

    // 4. Same-frame axis reports merge into one sample.
    {
        CVelocityEstimator est;
        est.add(10, true, 1.0);
        est.add(10, false, 2.0);
        est.add(17, true, 1.0);
        check(est.count() == 2, "same-timestamp reports merge");
    }

    // 5. Timestamp rollover: identical result whether or not the gesture
    //    straddles 2^32 ms.
    {
        auto a  = constantFlick(1000);
        auto b  = constantFlick(0xFFFFFFFFu - 40); // lift lands after the wrap
        auto la = a.launch(1000 + 77, 64, 0, 1.0);
        auto lb = b.launch(0xFFFFFFFFu - 40 + 77, 64, 0, 1.0);
        check(near(la.v, lb.v, 1e-12) && la.tailMs == lb.tailMs && la.samples == lb.samples, "launch is rollover-safe");
    }

    // 6. Decay: total travel of a fling equals v0 / lambda regardless of frame size.
    {
        const auto d  = SDecay::fromDecelPer16ms(0.967);
        double     v  = 2.0, h = 0.0, total = 0.0, dv, dh;
        for (int i = 0; i < 2000; ++i) {
            d.step(v, h, 8.333, 100.0, dv, dh);
            total += dv;
        }
        check(near(total, 2.0 / d.lambda, 1e-6), "integrated travel = v0 / lambda");
        double v2 = 2.0, h2 = 0.0, total2 = 0.0;
        for (int i = 0; i < 1000; ++i) {
            d.step(v2, h2, 16.666, 100.0, dv, dh);
            total2 += dv;
        }
        check(near(total, total2, 1e-6), "travel independent of frame time");
    }

    // 7. Stall: velocity decays over the real time, displacement is bounded.
    {
        const auto d = SDecay::fromDecelPer16ms(0.967);
        double     v = 2.0, h = 0.0, dv, dh;
        d.step(v, h, 5000.0, 100.0, dv, dh);
        const double maxTravel = 2.0 * (1.0 - std::exp(-d.lambda * 100.0)) / d.lambda;
        check(v < 1e-3 * 2.0, "5 s stall leaves <0.1% of the velocity");
        check(near(dv, maxTravel, 1e-9), "5 s stall emits at most 100 ms of travel");
    }

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
