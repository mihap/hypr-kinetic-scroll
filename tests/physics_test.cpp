// Deterministic tests for physics.hpp. No Hyprland needed:  make test
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

// Defaults used by the plugin.
constexpr uint32_t WINDOW = 32, GRACE = 28, FADE = 80;

// Finger moving at 1 unit/ms, reported every 7 ms for 70 ms.
static CVelocityEstimator constantFlick(uint32_t t0) {
    CVelocityEstimator est;
    for (int i = 0; i < 11; ++i)
        est.add(t0 + 7 * i, true, 7.0);
    return est;
}

// Finger accelerating 2 -> 12 units/ms over 10 reports (a real fast flick profile,
// longer than the window so the early ramp must be excluded).
static CVelocityEstimator acceleratingFlick(uint32_t t0) {
    CVelocityEstimator est;
    const double per[] = {14, 21, 28, 35, 42, 49, 57, 66, 75, 84};
    for (int i = 0; i < 10; ++i)
        est.add(t0 + 7 * i, true, per[i]);
    return est;
}

int main() {
    // 1. Constant speed: launch equals the finger speed, tail inside grace is free.
    {
        auto est = constantFlick(1000);
        for (uint32_t tail : {7u, 14u, 21u, 28u}) {
            auto l = est.launch(1000 + 70 + tail, WINDOW, GRACE, FADE, 1.0);
            check(near(l.v, 1.0, 1e-9) && l.tailMs == tail, "constant flick launches at finger speed (tail inside grace)");
        }
        check(est.launch(1077, WINDOW, GRACE, FADE, 1.0).h == 0.0, "no horizontal component");
    }

    // 2. Accelerating flick: launch reflects the end speed, not the mean of the ramp.
    {
        auto   est  = acceleratingFlick(1000);
        auto   l    = est.launch(1000 + 63 + 21, WINDOW, GRACE, FADE, 1.0);
        double mean = (21 + 28 + 35 + 42 + 49 + 57 + 66 + 75 + 84) / 63.0; // all timed reports
        double end  = (49 + 57 + 66 + 75 + 84) / 35.0;                     // reports within 32 ms of the last one
        check(near(l.v, end, 1e-9) && l.v > mean * 1.2, "accelerating flick launches at end speed, not the ramp mean");
    }

    // 3. Hesitation: beyond the grace the launch fades, and a pause launches nothing.
    {
        auto est = constantFlick(1000);
        auto l54 = est.launch(1070 + 54, WINDOW, GRACE, FADE, 1.0); // halfway through the fade
        auto l80 = est.launch(1070 + 80, WINDOW, GRACE, FADE, 1.0);
        auto l300 = est.launch(1070 + 300, WINDOW, GRACE, FADE, 1.0);
        check(near(l54.v, 0.5, 1e-9), "tail halfway through the fade launches at half speed");
        check(l80.v == 0.0 && l300.v == 0.0, "tail at/after the fade launches nothing");
    }

    // 4. Sparse device: reports further apart than the window still yield a velocity.
    {
        CVelocityEstimator est;
        est.add(0, true, 10.0);
        est.add(50, true, 50.0);
        est.add(100, true, 50.0);
        auto l = est.launch(110, WINDOW, GRACE, FADE, 1.0);
        check(near(l.v, 1.0, 1e-9) && l.samples == 2, "at least two timed reports are used");
    }

    // 5. Same-frame axis reports merge into one sample.
    {
        CVelocityEstimator est;
        est.add(10, true, 1.0);
        est.add(10, false, 2.0);
        est.add(17, true, 1.0);
        check(est.count() == 2, "same-timestamp reports merge");
    }

    // 6. Timestamp rollover: identical result whether or not the gesture straddles 2^32 ms.
    {
        auto a  = acceleratingFlick(1000);
        auto b  = acceleratingFlick(0xFFFFFFFFu - 20); // lift lands after the wrap
        auto la = a.launch(1000 + 63 + 21, WINDOW, GRACE, FADE, 1.0);
        auto lb = b.launch(0xFFFFFFFFu - 20 + 63 + 21, WINDOW, GRACE, FADE, 1.0);
        check(near(la.v, lb.v, 1e-12) && la.tailMs == lb.tailMs && la.samples == lb.samples, "launch is rollover-safe");
    }

    // 7. Decay: total travel of a fling equals v0 / lambda regardless of frame size.
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

    // 8. Stall: velocity decays over the real time, displacement is bounded.
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
