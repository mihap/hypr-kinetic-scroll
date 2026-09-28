#include "gesture.hpp"
#include "metrics.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace Gesture {

    const char* stopName(eStop s) {
        switch (s) {
            case eStop::BELOW_THRESHOLD: return "belowThreshold";
            case eStop::DECAY_DONE: return "decayDone";
            case eStop::NEW_GESTURE: return "newGesture";
            case eStop::TOUCHPAD_CONTACT: return "touchpadContact";
            case eStop::GESTURE_IDLE: return "gestureIdle";
            case eStop::TARGET_CHANGED: return "targetChanged";
            case eStop::TARGET_DESTROYED: return "targetDestroyed";
            case eStop::MOUSE_BUTTON: return "mouseButton";
            case eStop::ACTIVE_WINDOW: return "activeWindow";
            case eStop::RULE_DISABLED: return "ruleDisabled";
            case eStop::CAPTURED: return "captured";
            case eStop::UNLOAD: return "unload";
        }
        return "?";
    }

    static int stopCount(uint8_t axes) {
        return (axes & AXIS_V ? 1 : 0) + (axes & AXIS_H ? 1 : 0);
    }

    // -----------------------------------------------------------------------
    // Window policy

    static bool classLooksLikeBrowser(std::string cls) {
        for (auto& c : cls)
            c = std::tolower(static_cast<unsigned char>(c));

        return cls.find("firefox") != std::string::npos || cls.find("chrom") != std::string::npos || cls.find("brave") != std::string::npos ||
            cls.find("vivaldi") != std::string::npos || cls.find("opera") != std::string::npos || cls.find("librewolf") != std::string::npos ||
            cls.find("zen") != std::string::npos;
    }

    static bool classInList(std::string cls, std::string list) {
        for (auto& c : cls)
            c = std::tolower(static_cast<unsigned char>(c));

        for (auto& c : list) {
            if (c == ',')
                c = ' ';
            else
                c = std::tolower(static_cast<unsigned char>(c));
        }

        std::istringstream iss(list);
        for (std::string item; iss >> item;) {
            if (item == cls)
                return true;
        }

        return false;
    }

    void CPolicy::setAppRule(const std::string& cls, bool enabled) {
        m_perApp[cls] = enabled;
    }

    void CPolicy::setDefaultAppRule(bool enabled) {
        m_default = enabled;
    }

    void CPolicy::reset() {
        m_perApp.clear();
        m_default = true;
    }

    bool CPolicy::allows(const std::string& cls, const std::string& disabledClasses, bool disableInBrowser) const {
        if (const auto it = m_perApp.find(cls); it != m_perApp.end())
            return it->second; // explicit rule wins over everything, including the browser heuristic

        if (classInList(cls, disabledClasses))
            return false;

        if (!m_default)
            return false;

        if (disableInBrowser && classLooksLikeBrowser(cls))
            return false;

        return true;
    }

    // -----------------------------------------------------------------------
    // Construction, timer

    CMachine::CMachine(IHost& host) : m_host(host) {}

    CMachine::~CMachine() {
        // Unloading mid-momentum must not leave the client waiting for the lift
        // we swallowed. The host normally closes the gesture itself before it
        // tears down; this is the safety net.
        if (m_state != eState::IDLE)
            stop(eStop::UNLOAD);
    }

    void CMachine::onTimer() {
        // Disabled since the timer was armed: nothing is ours any more. Close
        // what we owned rather than launch or keep flinging.
        if (m_state != eState::IDLE && !m_host.enabled()) {
            stop(eStop::RULE_DISABLED);
            return;
        }
        switch (m_state) {
            case eState::IDLE: break;
            case eState::IGNORED:
                // Long enough without a stop event: whatever that was, it is over.
                resetGesture(false);
                break;
            case eState::TRACKING:
                if (m_timerLaunches) {
                    // Smooth mouse only: no stop event exists, silence is the lift.
                    // (Touchpad gestures launch solely on libinput's real stop.)
                    if (Metrics::g_metrics.enabled())
                        Metrics::g_metrics.gesture().tLift = Metrics::nowNs();
                    launch(m_est.lastMs(), "timeout");
                } else
                    stop(eStop::GESTURE_IDLE); // fingers resting on the pad
                break;
            case eState::DECAYING:
                // Watchdog: the render hook normally emits. No frame came (nothing
                // damaged, DPMS, monitor mismatch): keep the fling alive from here.
                step(false);
                break;
        }
    }

    // -----------------------------------------------------------------------
    // Gesture start

    bool CMachine::beginGesture(const SAxisEvent& e, bool touchpadSource) {
        m_g          = {};
        m_g.touchpad = touchpadSource;
        m_g.source   = touchpadSource ? e.source : eSource::CONTINUOUS;

        const SParams p = m_host.params();
        m_g.debug       = p.debug;

        const STarget target = m_host.captureTarget();

        // Nothing to own without a surface: momentum would land on whatever the
        // pointer enters next. A window rule can also exclude the gesture.
        if (!target.hasSurface || (target.hasWindow && !m_policy.allows(target.windowClass, p.disabledClasses, p.disableInBrowser))) {
            m_state = eState::IGNORED;
            m_host.armTimer(500);
            return false;
        }

        // Snapshot everything the momentum phase needs.
        m_g.frameMs            = target.frameMs;
        m_g.stopOnTargetChange = p.stopOnTargetChange;
        m_g.intervalMs         = std::max<int64_t>(1, p.intervalMs);
        m_g.windowMs           = std::max<int64_t>(8, p.velocityWindowMs);
        m_g.tailGraceMs        = std::max<int64_t>(0, p.liftTailGraceMs);
        m_g.tailFadeMs         = std::max<int64_t>(m_g.tailGraceMs + 1, p.liftTailFadeMs);
        m_g.launchMultiplier   = p.deltaMultiplier;
        m_g.decay              = Physics::SDecay::fromDecelPer16ms(p.decel);
        // min_velocity is in scroll units per 16 ms, comparable to real deltas. A
        // zero cutoff would let a zero-velocity fling run forever: floor it.
        m_g.minVelPerMs = std::max(1e-4, p.minVelocity / 16.0);

        m_est.reset();
        m_state = eState::TRACKING;

        // Metrics sink is re-read once per gesture (string copy); hot paths only
        // test the cached bool.
        Metrics::g_metrics.setEnabled(!p.metricsFile.empty(), p.metricsFile);
        if (Metrics::g_metrics.enabled())
            Metrics::g_metrics.gestureBegin(Metrics::nowNs(), target.windowClass);

        return true;
    }

    void CMachine::resetGesture(bool keepOwedStops) {
        m_state             = eState::IDLE;
        m_timerLaunches     = false;
        m_velocityV         = 0.0;
        m_velocityH         = 0.0;
        m_lastStepFromTimer = false;
        if (!keepOwedStops)
            m_owedStops = 0;
        m_est.reset();
        m_g = {};
        m_host.releaseTarget(keepOwedStops);
        m_host.armTimer(0);
    }

    // -----------------------------------------------------------------------
    // Axis events

    bool CMachine::onAxis(const SAxisEvent& e) {
        // Metrics: attribute this call's cost to the gesture or to the idle bucket.
        struct SAxisScope {
            CMachine* self;
            int64_t   t0;
            int64_t   flush0;
            ~SAxisScope() {
                auto& m = Metrics::g_metrics;
                if (!m.enabled() || t0 == 0)
                    return;
                const double ns = static_cast<double>(Metrics::nowNs() - t0 - (m.flushNsTotal() - flush0));
                if (m.active() && (self->m_state == eState::TRACKING || self->m_state == eState::DECAYING))
                    m.gesture().axisNs.add(ns);
                else
                    m.global().idleAxisNs.add(ns);
            }
        } axisScope{this, Metrics::g_metrics.enabled() ? Metrics::nowNs() : 0, Metrics::g_metrics.flushNsTotal()};

        // Disabled, or the seat's input is captured (input-capture protocol):
        // nothing is ours. Close whatever we owned; let everything pass.
        if (!m_host.enabled()) {
            if (m_state != eState::IDLE)
                stop(eStop::RULE_DISABLED);
            return false;
        }
        if (m_host.inputCaptured()) {
            if (m_state != eState::IDLE)
                stop(eStop::CAPTURED);
            return false;
        }

        // Touchpad scrolling, plus mice that report smooth-only deltas.
        const bool touchpadSource = e.source == eSource::FINGER || e.source == eSource::CONTINUOUS;
        const bool smoothMouse    = e.mouse && e.deltaDiscrete == 0;
        if (!touchpadSource && !smoothMouse)
            return false;

        const bool    isStop  = e.delta == 0.0 && e.deltaDiscrete == 0;
        const uint8_t axisBit = e.vertical ? AXIS_V : AXIS_H;

        switch (m_state) {
            case eState::IGNORED:
                if (isStop)
                    resetGesture(false);
                return false;

            case eState::DECAYING:
                if (isStop) {
                    // Gesture ownership: the client must not see the finger lift
                    // while momentum is live. Delivered, it would end the gesture
                    // (GTK's scroll-end) or start the client's own fling on top of
                    // ours (GTK4, Chromium). libinput sends one stop per axis; all
                    // are swallowed and owed back when momentum ends.
                    m_owedStops |= axisBit;
                    if (Metrics::g_metrics.enabled())
                        ++Metrics::g_metrics.gesture().stopsCancelled;
                    return true;
                }
                // Fingers back on the pad and moving: the new swipe takes over.
                // The client's gesture simply continues with these real events;
                // the stops we owe carry over to the new gesture.
                stop(eStop::NEW_GESTURE);
                [[fallthrough]];

            case eState::IDLE:
                if (isStop) {
                    // A stray stop while idle (e.g. the previous gesture handed
                    // off but never launched): close what is still owed.
                    sendOwedStops();
                    return false;
                }
                if (!beginGesture(e, touchpadSource))
                    return false;
                break;

            case eState::TRACKING: {
                // Real events go to whatever has pointer focus now. If that is no
                // longer our target, this gesture is over for us: a delta restarts
                // tracking against the new focus; a lift passes through untouched.
                const auto ts = m_host.targetState();
                if (ts == eTarget::CHANGED || ts == eTarget::DESTROYED) {
                    stop(ts == eTarget::CHANGED ? eStop::TARGET_CHANGED : eStop::TARGET_DESTROYED);
                    if (isStop || !beginGesture(e, touchpadSource))
                        return false;
                }
                break;
            }
        }

        // TRACKING.
        if (isStop) {
            if (!m_g.touchpad)
                return false;
            // libinput's axis_stop: fingers left the pad. Launch synchronously; any
            // wait here is a visible hitch. If nothing launches (pause before lift,
            // below threshold) the real stop passes through, and any stop still
            // owed from a hand-off is delivered alongside it.
            if (Metrics::g_metrics.enabled())
                Metrics::g_metrics.gesture().tLift = Metrics::nowNs();
            if (!launch(e.timeMs, "axisStop"))
                return false;
            m_owedStops |= axisBit;
            if (Metrics::g_metrics.enabled())
                ++Metrics::g_metrics.gesture().stopsCancelled;
            return true;
        }

        if (touchpadSource != m_g.touchpad)
            return false; // mixed devices mid-gesture: not ours

        m_est.add(e.timeMs, e.vertical, e.delta);
        if (Metrics::g_metrics.enabled() && Metrics::g_metrics.active())
            Metrics::g_metrics.addSample(e.timeMs, e.vertical ? e.delta : 0.0, e.vertical ? 0.0 : e.delta);

        // Touchpad: idle safety only. Fingers resting this long end the gesture, so
        // a later lift launches nothing (the velocity fade already yields ~0 for a
        // pause; this covers a missing stop event). Smooth mouse: no stop event
        // exists, so a short silence means "lifted".
        m_timerLaunches = !m_g.touchpad;
        m_host.armTimer(m_g.touchpad ? 200 : 50);
        return false;
    }

    void CMachine::onTouchpadContact() {
        if (m_state == eState::TRACKING || m_state == eState::DECAYING)
            stop(eStop::TOUCHPAD_CONTACT);
    }

    // -----------------------------------------------------------------------
    // Launch

    bool CMachine::launch(uint32_t liftMs, const char* how) {
        if (m_state != eState::TRACKING)
            return false;

        const auto l = m_est.launch(liftMs, m_g.windowMs, m_g.tailGraceMs, m_g.tailFadeMs, m_g.launchMultiplier);
        m_velocityV  = l.v;
        m_velocityH  = l.h;

        if (Metrics::g_metrics.enabled()) {
            auto& g   = Metrics::g_metrics.gesture();
            g.launchV = l.v;
            g.launchH = l.h;
            g.span    = l.spanMs;
            g.tail    = l.tailMs;
        }
        if (m_g.debug) {
            std::ostringstream o;
            o << "[hypr-kinetic-scroll] launch(" << how << ") v=" << l.v << "/ms h=" << l.h << "/ms span=" << l.spanMs << "ms tail=" << l.tailMs << " rest=" << l.rest
              << " samples=" << l.samples;
            m_host.log(o.str());
        }

        if (std::abs(m_velocityV) <= m_g.minVelPerMs && std::abs(m_velocityH) <= m_g.minVelPerMs) {
            stop(eStop::BELOW_THRESHOLD);
            return false;
        }

        m_g.scrollFactor    = m_host.scrollFactor(m_g.source);
        m_state             = eState::DECAYING;
        m_lastTickMs        = m_host.nowMs();
        m_lastStepFromTimer = false;

        if (Metrics::g_metrics.enabled()) {
            auto& g    = Metrics::g_metrics.gesture();
            g.tLaunch  = Metrics::nowNs();
            g.launched = true;
        }

        // Nothing is damaged after the fingers stop, so without this the first
        // momentum frame would wait for the watchdog: a visible hitch at lift.
        m_host.scheduleFrame();
        m_host.armTimer(m_g.intervalMs);
        return true;
    }

    // -----------------------------------------------------------------------
    // Momentum

    void CMachine::onFrame() {
        if (m_state != eState::DECAYING)
            return;
        step(true);
    }

    // One momentum step. Called once per compositor frame of the target monitor
    // (fromRender), or from the watchdog timer when no frame arrives.
    void CMachine::step(bool fromRender) {
        Metrics::CScope stepScope(Metrics::g_metrics.enabled() ? &Metrics::g_metrics.gesture().stepNs : nullptr);

        // Disabled mid-fling (kinetic-toggle, config reload): stop now, not at
        // the next real axis event, which may never come.
        if (!m_host.enabled()) {
            stepScope.retarget(nullptr);
            stop(eStop::RULE_DISABLED);
            return;
        }
        if (m_host.inputCaptured()) {
            stepScope.retarget(nullptr);
            stop(eStop::CAPTURED);
            return;
        }

        switch (m_host.targetState()) {
            case eTarget::DESTROYED:
                // The client we owe a stop is gone; nothing to deliver, nothing to scroll.
                stepScope.retarget(nullptr);
                stop(eStop::TARGET_DESTROYED);
                return;
            case eTarget::CHANGED:
                if (m_g.stopOnTargetChange) {
                    stepScope.retarget(nullptr);
                    stop(eStop::TARGET_CHANGED);
                    return;
                }
                break;
            case eTarget::SAME:
            case eTarget::NONE: break; // NONE cannot launch (beginGesture requires a surface)
        }

        const double now = m_host.nowMs();
        double       dt  = now - m_lastTickMs;

        // A render right after a *watchdog* emission carries nothing new. The
        // launch frame itself may arrive within a fraction of a frame and must
        // be used, or the watchdog would start the fling 16 ms late.
        if (fromRender && m_lastStepFromTimer && dt < 0.25 * m_g.frameMs)
            return;

        if (Metrics::g_metrics.enabled()) {
            auto& g = Metrics::g_metrics.gesture();
            if (fromRender) {
                ++g.stepsRender;
                g.renderDtMs.add(dt);
            } else
                ++g.stepsTimer;
        }

        m_lastTickMs        = now;
        m_lastStepFromTimer = !fromRender;
        if (dt <= 0.0)
            dt = fromRender ? m_g.frameMs : m_g.intervalMs;

        // Decay over the real elapsed time; emit at most 100 ms worth of travel so
        // a stalled event loop doesn't make the content jump.
        double deltaV = 0.0, deltaH = 0.0;
        m_g.decay.step(m_velocityV, m_velocityH, dt, 100.0, deltaV, deltaH);

        emitSyntheticScroll(deltaV, deltaH, static_cast<uint32_t>(static_cast<int64_t>(now)));

        if (m_g.debug) {
            std::ostringstream o;
            o << "[hypr-kinetic-scroll] step src=" << (fromRender ? "render" : "timer") << " dt=" << dt << " dV=" << deltaV << " v=" << m_velocityV;
            m_host.log(o.str());
        }

        if (std::abs(m_velocityV) <= m_g.minVelPerMs && std::abs(m_velocityH) <= m_g.minVelPerMs) {
            stepScope.retarget(nullptr); // gesture is finalized inside stop()
            stop(eStop::DECAY_DONE);
            return;
        }

        // After a real frame the watchdog only needs to catch a missing next frame.
        // If the watchdog itself fired, frames aren't coming: poll at interval_ms.
        const int watchdog = fromRender ? std::max<int>(m_g.intervalMs, static_cast<int>(std::ceil(2.5 * m_g.frameMs))) : m_g.intervalMs;
        m_host.armTimer(watchdog);
    }

    void CMachine::emitSyntheticScroll(double deltaV, double deltaH, uint32_t timeMs) {
        if (deltaV == 0.0 && deltaH == 0.0)
            return;

        if (Metrics::g_metrics.enabled()) {
            auto& g = Metrics::g_metrics.gesture();
            ++g.emits;
            g.travelV += std::abs(deltaV);
            g.travelH += std::abs(deltaH);
            if (g.tFirstEmit == 0)
                g.tFirstEmit = Metrics::nowNs();
        }

        // Same axis source as the finger phase, so the client sees one gesture.
        // Every axis we emit on is a sequence we must close.
        if (deltaV != 0.0)
            m_owedStops |= AXIS_V;
        if (deltaH != 0.0)
            m_owedStops |= AXIS_H;

        m_host.emitScroll(deltaV * m_g.scrollFactor, deltaH * m_g.scrollFactor, timeMs, m_g.source);
    }

    // The axis_stop events the client never got, one per owed axis. Delivered
    // by the host to the gesture's own surface, or a newer focus that momentum
    // was allowed to follow. If pointer focus has since moved elsewhere the
    // debt is void: that client already received pointer.leave, which ends the
    // gesture on its side.
    void CMachine::sendOwedStops() {
        if (!m_owedStops)
            return;

        const bool delivered = m_host.deliverStops(m_owedStops, m_g.source);
        if (delivered && Metrics::g_metrics.enabled())
            Metrics::g_metrics.gesture().stopsSent += stopCount(m_owedStops);
        m_owedStops = 0;
    }

    // -----------------------------------------------------------------------
    // End of gesture

    void CMachine::stop(eStop reason) {
        if (m_state == eState::IDLE)
            return;

        // Close the gesture for the client if we swallowed its finger lift, unless
        // the client's gesture simply continues with the real events that caused
        // this stop (fingers back down and moving: a re-touch, as on macOS). In
        // that case the debt carries over and is settled when that gesture ends.
        const bool carryOver = clientGestureContinues(reason);
        if (!carryOver)
            sendOwedStops();

        if (m_g.debug) {
            std::ostringstream o;
            o << "[hypr-kinetic-scroll] stop reason=" << stopName(reason) << " owedLeft=" << int(m_owedStops);
            m_host.log(o.str());
        }

        if (Metrics::g_metrics.enabled()) {
            auto& g      = Metrics::g_metrics.gesture();
            g.stopsOwed  = stopCount(m_owedStops); // carried over (hand-off) or undeliverable
            g.stopReason = stopName(reason);
        }
        Metrics::g_metrics.gestureEnd(Metrics::nowNs());
        resetGesture(carryOver);
    }
}
