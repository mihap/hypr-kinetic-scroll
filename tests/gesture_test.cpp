// Scripted tests for the gesture state machine (gesture.hpp). No Hyprland
// needed:  make test
//
// A fake host models what the compositor would show the machine: pointer
// focus as small integer surface ids, a monotonic clock, the timer, and it
// records everything the machine asks it to do (synthetic scrolls, delivered
// axis_stop events, scheduled frames). Each scenario below is a sequence of
// libinput-like events and frames, and an assertion about the protocol the
// client ends up seeing.
#include "../gesture.hpp"
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace Gesture;

static int  g_failed = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failed;
}
static bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

// ---------------------------------------------------------------------------
// Fake host

struct CFakeHost : IHost {
    // Scripted compositor state.
    bool          on       = true;
    bool          captured = false;
    SParams       p;
    int           focus       = 1; // surface id under the pointer; 0 = none
    bool          hasWindow   = true;
    std::string   windowClass = "com.mitchellh.ghostty";
    double        frameMs     = 1000.0 / 120.0;
    double        factor      = 1.0;
    std::set<int> destroyed;
    double        clock = 10000.0;

    // What the host holds for the machine.
    int gestureSurface = 0; // captured target
    int emitSurface    = 0; // last synthetic scroll went here

    // Recorded effects.
    struct SEmit {
        double   v, h;
        uint32_t t;
        eSource  src;
        int      surface;
    };
    std::vector<SEmit>       emits;
    std::vector<uint8_t>     stops; // delivered axis masks, one entry per delivery
    std::vector<eSource>     stopSources;
    int                      undeliverable = 0;
    int                      timer         = 0; // currently armed ms, 0 = disarmed
    int                      frames        = 0; // scheduleFrame calls
    int                      captures      = 0;
    std::vector<std::string> logs;

    bool enabled() const override {
        return on;
    }
    bool inputCaptured() const override {
        return captured;
    }
    SParams params() const override {
        return p;
    }
    STarget captureTarget() override {
        ++captures;
        gestureSurface = focus;
        STarget t;
        t.hasSurface  = focus != 0;
        t.hasWindow   = hasWindow && focus != 0;
        t.windowClass = t.hasWindow ? windowClass : "";
        t.frameMs     = frameMs;
        return t;
    }
    eTarget targetState() const override {
        if (!gestureSurface)
            return eTarget::NONE;
        if (destroyed.count(gestureSurface))
            return eTarget::DESTROYED;
        if (focus != gestureSurface)
            return eTarget::CHANGED;
        return eTarget::SAME;
    }
    void releaseTarget(bool keepEmitSurface) override {
        gestureSurface = 0;
        if (!keepEmitSurface)
            emitSurface = 0;
    }
    double scrollFactor(eSource) const override {
        return factor;
    }
    void emitScroll(double v, double h, uint32_t t, eSource src) override {
        emitSurface = focus;
        emits.push_back({v, h, t, src, focus});
    }
    bool deliverStops(uint8_t axes, eSource src) override {
        const bool toGesture = gestureSurface && !destroyed.count(gestureSurface) && focus == gestureSurface;
        const bool toEmit    = emitSurface && !destroyed.count(emitSurface) && focus == emitSurface;
        if (!focus || (!toGesture && !toEmit)) {
            ++undeliverable;
            emitSurface = 0;
            return false;
        }
        stops.push_back(axes);
        stopSources.push_back(src);
        emitSurface = 0;
        return true;
    }
    void armTimer(int ms) override {
        timer = ms;
    }
    void scheduleFrame() override {
        ++frames;
    }
    double nowMs() const override {
        return clock;
    }
    void log(std::string_view line) override {
        logs.emplace_back(line);
    }

    double travelV() const {
        double s = 0.0;
        for (const auto& e : emits)
            s += e.v;
        return s;
    }
    double travelH() const {
        double s = 0.0;
        for (const auto& e : emits)
            s += e.h;
        return s;
    }
};

// ---------------------------------------------------------------------------
// Event helpers

static SAxisEvent delta(uint32_t t, double d, bool vertical = true) {
    SAxisEvent e;
    e.timeMs   = t;
    e.vertical = vertical;
    e.delta    = d;
    e.source   = eSource::FINGER;
    return e;
}

static SAxisEvent liftEvent(uint32_t t, bool vertical = true) {
    SAxisEvent e;
    e.timeMs   = t;
    e.vertical = vertical;
    e.source   = eSource::FINGER;
    return e;
}

static SAxisEvent mouseSmooth(uint32_t t, double d) {
    SAxisEvent e;
    e.timeMs = t;
    e.delta  = d;
    e.mouse  = true;
    return e;
}

// A two-finger flick at 1 unit/ms: n reports of 7 units every 7 ms starting
// at t0 (plus an optional horizontal component). Returns the last report time.
static uint32_t flick(CMachine& m, uint32_t t0, int n = 11, double dh = 0.0) {
    uint32_t t = t0;
    for (int i = 0; i < n; ++i) {
        t = t0 + 7 * i;
        m.onAxis(delta(t, 7.0));
        if (dh != 0.0)
            m.onAxis(delta(t, dh, false));
    }
    return t;
}

// Render frames at the host's frame interval until the machine is idle.
static int runFrames(CMachine& m, CFakeHost& h, int maxFrames = 5000) {
    int n = 0;
    while (!m.idle() && n < maxFrames) {
        h.clock += h.frameMs;
        m.onFrame();
        ++n;
    }
    return n;
}

static bool allSource(const CFakeHost& h, eSource s) {
    for (const auto& e : h.emits)
        if (e.src != s)
            return false;
    return !h.emits.empty();
}

static double lambdaFor(double decel) {
    return Physics::SDecay::fromDecelPer16ms(decel).lambda;
}

// ---------------------------------------------------------------------------

int main() {
    // 1. Flick, lift, fling to the end: the lift is swallowed, the launch frame
    //    is used, one scroll per frame, and exactly one axis_stop is delivered.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        check(m.state() == CMachine::eState::TRACKING && h.timer == 200 && h.captures == 1, "deltas start tracking; idle timer armed at 200 ms");

        const bool swallowed = m.onAxis(liftEvent(last + 7));
        check(swallowed && m.decaying(), "lift within the grace launches synchronously and is swallowed");
        check(h.frames == 1 && h.timer == 16 && h.stops.empty(), "launch schedules a frame and arms the watchdog; no stop yet");
        check(near(m.velocityV(), 1.0, 1e-9), "launch velocity is the finger's speed");

        h.clock += 2.0; // the launch frame arrives within a fraction of a frame
        m.onFrame();
        check(h.emits.size() == 1 && near(h.emits[0].v, 2.0, 0.01), "the launch frame is used, 2 ms of travel");

        const int n = runFrames(m, h);
        check(m.idle() && n > 100, "momentum ends on the velocity floor");
        const double total = 1.0 / lambdaFor(0.967);
        check(h.travelV() > total - 16.0 && h.travelV() < total, "emitted travel is v0/lambda minus the sub-floor remainder");
        check(allSource(h, eSource::FINGER), "synthetic scrolls carry the finger source");
        check(h.stops.size() == 1 && h.stops[0] == AXIS_V && h.stopSources[0] == eSource::FINGER, "exactly one vertical axis_stop, finger source");
        check(h.timer == 0 && m.owedStops() == 0, "timer disarmed, nothing owed");
    }

    // 2. Scroll, pause, lift: nothing launches and the real stop passes through.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        const bool swallowed = m.onAxis(liftEvent(last + 300));
        check(!swallowed && m.idle(), "lift after a pause passes through, machine idle");
        check(h.emits.empty() && h.stops.empty() && h.undeliverable == 0, "no scroll emitted, no stop sent");
    }

    // 3. Fingers resting on the pad: the tracking timer ends the gesture, the
    //    later lift passes through.
    {
        CFakeHost h;
        CMachine  m(h);
        flick(m, 1000);
        m.onTimer();
        check(m.idle() && h.timer == 0, "tracking timer (fingers resting) ends the gesture");
        check(!m.onAxis(liftEvent(1500)) && h.stops.empty(), "the later lift passes through, nothing owed");
    }

    // 4. Touching the pad during momentum stops it and settles the debt.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        h.clock += 8;
        m.onFrame();
        h.clock += 8;
        m.onFrame();
        const auto emitted = h.emits.size();
        m.onTouchpadContact();
        check(m.idle() && h.stops.size() == 1 && h.stops[0] == AXIS_V, "touchpad contact stops momentum and delivers the stop");
        h.clock += 8;
        m.onFrame();
        check(h.emits.size() == emitted, "no scroll after the stop");
    }

    // 5. Re-touch hand-off: a new swipe during momentum takes over. The old
    //    debt carries into the new gesture and is settled once, at its end.
    {
        CFakeHost h;
        CMachine  m(h);
        auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        for (int i = 0; i < 20; ++i) {
            h.clock += 8;
            m.onFrame();
        }
        check(m.decaying(), "still decaying after 20 frames");

        const bool passed = !m.onAxis(delta(1500, 7.0));
        check(passed && m.state() == CMachine::eState::TRACKING, "a delta during momentum passes through and starts tracking");
        check(h.stops.empty() && m.owedStops() == AXIS_V && h.captures == 2, "the owed stop carries over, nothing delivered yet");

        last = flick(m, 1500);
        check(m.onAxis(liftEvent(last + 7)) && m.decaying(), "the second flick launches");
        runFrames(m, h);
        check(m.idle() && h.stops.size() == 1 && h.stops[0] == AXIS_V, "one stop at the very end, not two");
    }

    // 6. Hand-off, then the second gesture lifts after a pause: the real stop
    //    passes through and the carried stop is delivered alongside it.
    {
        CFakeHost h;
        CMachine  m(h);
        auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        h.clock += 8;
        m.onFrame();
        m.onAxis(delta(1200, 7.0));
        last = flick(m, 1200, 5);
        const bool swallowed = m.onAxis(liftEvent(last + 300));
        check(!swallowed && m.idle(), "lift after a pause passes through");
        check(h.stops.size() == 1 && h.stops[0] == AXIS_V, "the carried stop is delivered with it");
    }

    // 7. Two axes: libinput sends one stop per axis. The second arrives while
    //    decaying and is swallowed too; both are settled in one delivery.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000, 11, 3.0);
        check(m.onAxis(liftEvent(last + 7, true)), "vertical stop launches and is swallowed");
        check(m.onAxis(liftEvent(last + 7, false)) && m.decaying(), "horizontal stop during momentum is swallowed");
        check(m.owedStops() == (AXIS_V | AXIS_H), "both axes owed");
        runFrames(m, h);
        bool both = !h.emits.empty();
        for (const auto& e : h.emits)
            both = both && e.v != 0.0 && e.h != 0.0;
        check(both, "every frame carries both axes");
        check(near(h.travelH() / h.travelV(), 3.0 / 7.0, 1e-6), "axes keep their ratio");
        check(h.stops.size() == 1 && h.stops[0] == (AXIS_V | AXIS_H), "one delivery closing both axes");
    }

    // 8. Target destroyed during momentum: nothing to scroll, nothing to deliver.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        h.clock += 8;
        m.onFrame();
        h.destroyed.insert(1);
        h.clock += 8;
        m.onFrame();
        check(m.idle() && h.stops.empty() && h.undeliverable == 1, "destroyed target ends momentum; the stop is void");
        check(h.emits.size() == 1, "no scroll after the destruction");
    }

    // 9. Focus moves during momentum (stop_on_target_change = 1): momentum
    //    ends; the stop cannot go to a surface that lost pointer focus.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        h.clock += 8;
        m.onFrame();
        h.focus = 2;
        h.clock += 8;
        m.onFrame();
        check(m.idle() && h.stops.empty() && h.undeliverable == 1, "focus change ends momentum; that client got pointer.leave instead");
    }

    // 10. stop_on_target_change = 0: momentum follows the focus; the stop goes
    //     to the surface that last received a synthetic scroll.
    {
        CFakeHost h;
        h.p.stopOnTargetChange = false;
        CMachine   m(h);
        const auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        h.clock += 8;
        m.onFrame();
        h.focus = 2;
        runFrames(m, h);
        check(m.idle() && h.emits.back().surface == 2, "momentum continued onto the new focus");
        check(h.stops.size() == 1 && h.stops[0] == AXIS_V && h.undeliverable == 0, "the stop went to the surface that received the scrolls");
    }

    // 11. Focus moves while tracking: a delta re-targets, a lift passes through.
    {
        CFakeHost h;
        CMachine  m(h);
        flick(m, 1000, 5);
        h.focus = 2;
        check(!m.onAxis(delta(1100, 7.0)) && m.state() == CMachine::eState::TRACKING && h.captures == 2 && h.gestureSurface == 2,
              "a delta on a new focus restarts tracking against it");
        const auto last = flick(m, 1100, 6);
        check(m.onAxis(liftEvent(last + 7)) && m.decaying(), "the gesture launches on the new target");
        runFrames(m, h);
        check(h.emits.front().surface == 2 && h.stops.size() == 1, "scrolls and the stop went to the new target");
    }
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        h.focus = 2;
        check(!m.onAxis(liftEvent(last + 7)) && m.idle() && h.stops.empty() && h.undeliverable == 0, "a lift after a focus change passes through untouched");
    }

    // 12. Ignored gestures: browser class, disabled class, no surface.
    {
        CFakeHost h;
        h.windowClass = "firefox";
        CMachine m(h);
        check(!m.onAxis(delta(1000, 7.0)) && m.state() == CMachine::eState::IGNORED && h.timer == 500, "browser gesture is ignored");
        const auto last = flick(m, 1000);
        check(!m.onAxis(liftEvent(last + 7)) && m.idle() && h.emits.empty() && h.stops.empty(), "its lift passes through; nothing emitted");

        m.policy().setAppRule("firefox", true);
        flick(m, 2000);
        check(m.state() == CMachine::eState::TRACKING, "an explicit rule overrides the browser heuristic");
    }
    {
        CFakeHost h;
        h.windowClass       = "org.telegram.desktop";
        h.p.disabledClasses = "org.telegram.desktop, steam";
        CMachine m(h);
        m.onAxis(delta(1000, 7.0));
        check(m.state() == CMachine::eState::IGNORED, "disabled_classes ignores the gesture");
        m.onTimer();
        check(m.idle() && h.timer == 0, "the ignored gesture times out to idle");
    }
    {
        CFakeHost h;
        h.focus = 0;
        CMachine m(h);
        m.onAxis(delta(1000, 7.0));
        check(m.state() == CMachine::eState::IGNORED, "no pointer-focus surface: ignored");
    }

    // 13. Disabled mid-momentum: the gesture is closed, events pass through.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        h.clock += 8;
        m.onFrame();
        h.on = false;
        check(!m.onAxis(delta(1200, 7.0)) && m.idle(), "disabling closes the gesture on the next event");
        check(h.stops.size() == 1 && h.stops[0] == AXIS_V, "the owed stop is delivered");
        check(!m.onAxis(delta(1210, 7.0)) && m.idle(), "while disabled nothing is tracked");
    }

    // 14. Input capture takes the seat: momentum ends on the next step.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        h.captured = true;
        h.clock += 8;
        m.onFrame();
        check(m.idle() && h.stops.size() == 1, "capture ends momentum and closes the gesture");
    }

    // 15. Unload closes a live gesture: explicitly, and via the destructor.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        m.stop(eStop::UNLOAD);
        check(m.idle() && h.stops.size() == 1 && h.timer == 0, "explicit unload delivers the stop");
    }
    {
        CFakeHost h;
        {
            CMachine   m(h);
            const auto last = flick(m, 1000);
            m.onAxis(liftEvent(last + 7));
        }
        check(h.stops.size() == 1, "destroying a decaying machine delivers the stop");
    }

    // 16. Watchdog: no frames arrive; the timer keeps the fling alive. A render
    //     right after a watchdog step carries nothing new; a later one does.
    {
        CFakeHost h;
        CMachine  m(h);
        const auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        h.clock += 16;
        m.onTimer();
        check(h.emits.size() == 1 && near(h.emits[0].v, 15.75, 0.05) && h.timer == 16, "watchdog step emits 16 ms of travel and re-arms at interval_ms");
        h.clock += 1;
        m.onFrame();
        check(h.emits.size() == 1, "a render 1 ms after a watchdog step is skipped");
        h.clock += 8;
        m.onFrame();
        check(h.emits.size() == 2 && h.timer == 21, "a render 9 ms later steps; the watchdog waits 2.5 frames");
    }

    // 17. Smooth-scrolling mouse: no stop event exists, 50 ms of silence
    //     launches with the continuous source. Discrete wheel: not ours.
    {
        CFakeHost h;
        CMachine  m(h);
        for (int i = 0; i < 6; ++i)
            m.onAxis(mouseSmooth(1000 + 8 * i, 8.0));
        check(m.state() == CMachine::eState::TRACKING && h.timer == 50, "smooth mouse deltas track with a 50 ms silence timer");
        m.onTimer();
        check(m.decaying() && near(m.velocityV(), 1.0, 1e-9), "silence launches at the mouse's speed");
        runFrames(m, h);
        check(allSource(h, eSource::CONTINUOUS), "synthetic scrolls carry the continuous source");
        check(h.stops.size() == 1 && h.stopSources[0] == eSource::CONTINUOUS, "the sequence is closed with the same source");
    }
    {
        CFakeHost  h;
        CMachine   m(h);
        SAxisEvent e;
        e.timeMs        = 1000;
        e.delta         = 15.0;
        e.deltaDiscrete = 120;
        e.mouse         = true;
        check(!m.onAxis(e) && m.idle(), "a discrete wheel click is left alone");
    }

    // 18. Mixed devices mid-gesture are not ours.
    {
        CFakeHost h;
        CMachine  m(h);
        flick(m, 1000, 5);
        check(!m.onAxis(mouseSmooth(1030, 8.0)) && m.state() == CMachine::eState::TRACKING, "a mouse delta during a touchpad gesture passes and is not sampled");
        m.onAxis(liftEvent(1035));
        check(m.decaying() && near(m.velocityV(), 1.0, 1e-9), "launch velocity comes from the touchpad reports only");
    }

    // 19. Scroll factor and delta multiplier scale what the client sees.
    {
        CFakeHost h;
        h.factor            = 0.2;
        h.p.deltaMultiplier = 2.0;
        CMachine   m(h);
        const auto last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        check(near(m.velocityV(), 2.0, 1e-9), "delta_multiplier scales the launch velocity");
        runFrames(m, h);
        const double total = 2.0 / lambdaFor(0.967);
        check(h.travelV() > 0.2 * (total - 16.0) && h.travelV() < 0.2 * total, "emitted deltas carry the window's scroll factor");
    }

    // 20. Parameter sanitizing: a zero velocity floor still rejects a
    //     zero-velocity launch instead of flinging forever.
    {
        CFakeHost h;
        h.p.minVelocity = 0.0;
        CMachine m(h);
        for (int i = 0; i < 11; ++i)
            m.onAxis(delta(1000 + 7 * i, 1e-7));
        check(!m.onAxis(liftEvent(1077)) && m.idle() && h.emits.empty(), "a (near) zero velocity launches nothing even with min_velocity = 0");
    }

    // 21. Frame time does not change the distance travelled.
    {
        double travel[2];
        for (int i = 0; i < 2; ++i) {
            CFakeHost h;
            h.frameMs = i ? 1000.0 / 60.0 : 1000.0 / 120.0;
            CMachine   m(h);
            const auto last = flick(m, 1000);
            m.onAxis(liftEvent(last + 7));
            runFrames(m, h);
            travel[i] = h.travelV();
        }
        check(near(travel[0], travel[1], 1.0), "60 Hz and 120 Hz travel the same distance");
    }

    // 22. Policy on its own.
    {
        CPolicy p;
        check(!p.allows("Brave-browser", "", true) && p.allows("Brave-browser", "", false), "browser heuristic, case-insensitive, switchable");
        check(!p.allows("steam", "org.telegram.desktop, Steam", true) && p.allows("steam", "org.telegram.desktop", true), "disabled_classes, case-insensitive, comma or space separated");
        p.setDefaultAppRule(false);
        check(!p.allows("com.mitchellh.ghostty", "", true), "default off disables everything");
        p.setAppRule("com.mitchellh.ghostty", true);
        check(p.allows("com.mitchellh.ghostty", "", true), "explicit enable is the allow-list");
        p.setAppRule("steam", true);
        check(p.allows("steam", "steam", true), "explicit rule beats the disabled list");
        p.reset();
        check(p.allows("steam", "", true) && !p.allows("firefox", "", true), "reset restores the defaults");
    }

    // 23. Debug log is only produced when asked for.
    {
        CFakeHost h;
        CMachine  m(h);
        auto      last = flick(m, 1000);
        m.onAxis(liftEvent(last + 7));
        runFrames(m, h);
        check(h.logs.empty(), "no log lines without debug");
        h.p.debug = true;
        last      = flick(m, 3000);
        m.onAxis(liftEvent(last + 7));
        runFrames(m, h);
        check(h.logs.size() > 2 && h.logs.front().find("launch(axisStop)") != std::string::npos && h.logs.back().find("stop reason=decayDone") != std::string::npos,
              "debug logs launch, steps and the stop reason");
    }

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
