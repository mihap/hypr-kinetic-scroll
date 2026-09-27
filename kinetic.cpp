#include "kinetic.hpp"
#include "globals.hpp"
#include "metrics.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <fstream>
#include <cstdint>
#include <cctype>
#include <sstream>
#include <string_view>

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

struct SScrollTargetKeys {
    uintptr_t windowKey  = 0;
    uintptr_t surfaceKey = 0;
};

bool KineticState::hasAppRule(const std::string& windowClass) const {
    return m_perAppRules.contains(windowClass);
}

bool KineticState::shouldProcessForWindow(const std::string& windowClass) const {
    const auto it = m_perAppRules.find(windowClass);
    if (it != m_perAppRules.end())
        return it->second;
    return m_defaultAppRule;
}

void KineticState::setAppRule(const std::string& appClass, bool enabled) {
    m_perAppRules[appClass] = enabled;
}

void KineticState::setDefaultAppRule(bool enabled) {
    m_defaultAppRule = enabled;
}

void KineticState::resetAppRules() {
    m_perAppRules.clear();
    m_defaultAppRule = true;
}

static SScrollTargetKeys currentScrollTargetKeys() {
    SScrollTargetKeys out;

    if (g_pInputManager) {
        const auto PWIN = g_pInputManager->m_lastMouseFocus.lock();
        out.windowKey   = PWIN ? reinterpret_cast<uintptr_t>(PWIN.get()) : 0;
    }

    if (g_pSeatManager) {
        const auto PSURF = g_pSeatManager->m_state.pointerFocus.lock();
        out.surfaceKey   = PSURF ? reinterpret_cast<uintptr_t>(PSURF.get()) : 0;
    }

    return out;
}

KineticState::KineticState() {
    auto* loop = g_pCompositor->m_wlEventLoop;
    m_stopTimer  = wl_event_loop_add_timer(loop, onStopTimer, this);
    m_decayTimer = wl_event_loop_add_timer(loop, onDecayTimer, this);
}

KineticState::~KineticState() {
    if (m_stopTimer)
        wl_event_source_remove(m_stopTimer);
    if (m_decayTimer)
        wl_event_source_remove(m_decayTimer);
}

bool KineticState::onAxis(IPointer::SAxisEvent& e) {
    static uint64_t s_lastNotifyMs = 0;

    // Metrics: attribute this call's cost to the gesture it belongs to, or to
    // the idle bucket (wheel events, ignored sources) at scope exit.
    struct SAxisScope {
        KineticState* self;
        int64_t       t0;
        int64_t       flush0;
        ~SAxisScope() {
            auto& m = Metrics::g_metrics;
            if (!m.enabled())
                return;
            // Exclude the JSON write of a gesture finalized inside this call.
            const double ns = static_cast<double>(Metrics::nowNs() - t0 - (m.flushNsTotal() - flush0));
            if (m.active() && (self->m_tracking || self->m_decaying))
                m.gesture().axisNs.add(ns);
            else
                m.global().idleAxisNs.add(ns);
        }
    } axisScope{this, Metrics::nowNs(), Metrics::g_metrics.flushNsTotal()};

    if (!getKineticConfigInt("enabled", 1))
        return false;

    const auto targetKeys = currentScrollTargetKeys();

    if (getKineticConfigInt("stop_on_target_change", 1) && m_scrollTargetWindowKey != 0) {
        const bool windowChanged = targetKeys.windowKey != 0 && targetKeys.windowKey != m_scrollTargetWindowKey;
        const bool surfaceChanged = targetKeys.surfaceKey != 0 && targetKeys.surfaceKey != m_scrollTargetSurfaceKey;
        if (windowChanged || surfaceChanged)
            stopKinetic("targetChanged");
    }

    const auto PWIN = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
    if (PWIN) {
        const bool hasRule = hasAppRule(PWIN->m_class);
        if (classInList(PWIN->m_class, getKineticConfigString("disabled_classes", ""))) {
            if (m_decaying)
                stopKinetic("disabledClasses");
            return false;
        }

        if (!shouldProcessForWindow(PWIN->m_class)) {
            if (m_decaying)
                stopKinetic("appRule");
            return false;
        }

        if (getKineticConfigInt("disable_in_browser", 1) && !hasRule && classLooksLikeBrowser(PWIN->m_class)) {
            if (m_decaying)
                stopKinetic("browserFocus");
            return false;
        }
    }

    // Only handle touchpad scrolling (some devices report as mouse with smooth deltas)
    const bool touchpadSource = (e.source == WL_POINTER_AXIS_SOURCE_FINGER || e.source == WL_POINTER_AXIS_SOURCE_CONTINUOUS);
    const bool smoothMouse    = (e.mouse && e.deltaDiscrete == 0);
    if (!touchpadSource && !smoothMouse)
        return false;

    if (e.delta == 0.0 && e.deltaDiscrete == 0) {
        if (!touchpadSource)
            return false;

        // libinput's axis_stop: the fingers left the pad. Launch right now instead of
        // waiting for the empty frame plus a timer; that wait was a visible hitch.
        if (m_tracking) {
            m_axisEventInFrame = true;
            if (Metrics::g_metrics.enabled())
                Metrics::g_metrics.gesture().tLift = Metrics::nowNs();
            computeLaunchVelocity(e.timeMs);
            beginDecay("axisStop");
        }

        // Gesture ownership: while momentum is live the client must not see the
        // finger lift. Delivered, it would end the gesture (GTK's scroll-end) or
        // start the client's own fling on top of ours (GTK4, Chromium). Every
        // stop of this lift is swallowed (libinput may send one per axis); we
        // send the stop ourselves when momentum ends. If momentum did not
        // launch (pause before lift, below threshold) the real stop passes.
        if (m_decaying) {
            if (Metrics::g_metrics.enabled())
                ++Metrics::g_metrics.gesture().stopsCancelled;
            return true;
        }
        return false;
    }

    // Touching the pad again grabs the content and cancels existing momentum.
    if (m_decaying) {
        if (getKineticConfigInt("debug", 0)) {
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << "[hypr-kinetic-scroll] onAxis: new gesture stops decay self=" << this << "\n";
        }
        stopKinetic("newGesture");
    }

    if (!m_tracking) {
        // Gesture start. Re-read the metrics sink here (once per gesture) so
        // the hot paths only test a cached bool.
        static auto PMETRICS = CConfigValue<Config::STRING>("plugin:kinetic-scroll:metrics_file");
        const std::string metricsPath = PMETRICS.good() ? std::string{*PMETRICS} : std::string{};
        Metrics::g_metrics.setEnabled(!metricsPath.empty(), metricsPath);
        if (Metrics::g_metrics.enabled())
            Metrics::g_metrics.gestureBegin(axisScope.t0, PWIN ? PWIN->m_class : std::string{});
    }

    m_tracking              = true;
    m_axisEventInFrame      = touchpadSource;
    m_source                = touchpadSource ? e.source : WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    m_scrollTargetWindowKey = targetKeys.windowKey;
    m_scrollTargetSurfaceKey = targetKeys.surfaceKey;

    // Record the raw delta with the time it covers. Velocity is derived at lift
    // from a short window of these, in units per ms, so the launch speed matches
    // the finger regardless of the touchpad's report rate.
    const bool vertical = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL;
    uint32_t   dt       = 0;
    if (!m_samples.empty() && m_samples.back().t == e.timeMs) {
        // Same frame, other axis: merge.
        auto& last = m_samples.back();
        if (vertical)
            last.dv += e.delta;
        else
            last.dh += e.delta;
        dt = last.dt;
    } else {
        SSample s;
        s.t = e.timeMs;
        if (m_lastEventMs > 0 && e.timeMs >= m_lastEventMs && e.timeMs - m_lastEventMs < 200)
            s.dt = e.timeMs - m_lastEventMs;
        if (vertical)
            s.dv = e.delta;
        else
            s.dh = e.delta;
        dt = s.dt;
        m_samples.push_back(s);
        while (m_samples.size() > 64)
            m_samples.pop_front();
    }

    m_lastEventMs = e.timeMs;

    if (Metrics::g_metrics.enabled() && Metrics::g_metrics.active())
        Metrics::g_metrics.gesture().samples.push_back({e.timeMs, vertical ? e.delta : 0.0, vertical ? 0.0 : e.delta});

    if (getKineticConfigInt("debug", 0)) {
        auto     now    = std::chrono::steady_clock::now();
        uint64_t nowMs  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (nowMs - s_lastNotifyMs > 200) {
            const char* axis = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? "v" : "h";
            std::string msg = "[hypr-kinetic-scroll] axis=" + std::string(axis) +
                              " delta=" + std::to_string(e.delta) +
                              " source=" + std::to_string((int)e.source) +
                              " mouse=" + std::to_string(e.mouse ? 1 : 0) +
                              " discrete=" + std::to_string(e.deltaDiscrete) +
                              " dt=" + std::to_string(dt) +
                              " samples=" + std::to_string(m_samples.size()) +
                              " self=" + std::to_string(reinterpret_cast<uintptr_t>(this));
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << msg << "\n";
            s_lastNotifyMs = nowMs;
        }
    }

    // Touchpad: idle safety only. Fingers resting on the pad for this long end the
    // gesture, so a later lift launches nothing. (The velocity window already
    // yields ~0 for a pause before lift; this covers a missing stop event.)
    // Smooth mouse: no stop event exists, so a short silence means "lifted".
    m_cancelOnStopTimer = touchpadSource;
    wl_event_source_timer_update(m_stopTimer, touchpadSource ? 200 : 50);
    // Ensure decay timer is off while actively tracking
    wl_event_source_timer_update(m_decayTimer, 0);
    return false;
}

void KineticState::onPointerFrame() {
    if (m_axisEventInFrame) {
        m_axisEventInFrame = false;
        return;
    }

    if (!m_tracking)
        return;

    // Fallback only: the lift is normally handled synchronously from the
    // zero-delta axis event. If a device ends a sequence with an axis-less
    // frame and no stop event, launch after a short grace period.
    m_cancelOnStopTimer = false;
    wl_event_source_timer_update(m_stopTimer, 100);
}

void KineticState::onTouchpadContact() {
    if (m_tracking || m_decaying)
        stopKinetic("touchpadContact");
}

void KineticState::stopKinetic(const char* reason) {
    // Close the gesture for the client if we swallowed its finger lift. Not on
    // newGesture: the real axis event that triggered it is about to be
    // delivered and simply continues the same gesture, as a re-touch on macOS.
    if (m_clientInGesture && (!reason || std::string_view{reason} != "newGesture"))
        sendGestureStop();
    m_clientInGesture = false;
    m_activeV         = false;
    m_activeH         = false;
    m_source          = WL_POINTER_AXIS_SOURCE_FINGER;

    Metrics::g_metrics.gestureEnd(Metrics::nowNs(), reason);

    if (getKineticConfigInt("debug", 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] stopKinetic reason=" << (reason ? reason : "(null)") << " self=" << this << "\n";
    }
    m_velocityV            = 0.0;
    m_velocityH            = 0.0;
    m_tracking             = false;
    m_decaying             = false;
    m_axisEventInFrame     = false;
    m_cancelOnStopTimer    = false;
    m_lastEventMs          = 0;
    m_scrollTargetWindowKey = 0;
    m_scrollTargetSurfaceKey = 0;
    m_samples.clear();
    wl_event_source_timer_update(m_stopTimer, 0);
    wl_event_source_timer_update(m_decayTimer, 0);
}

// Velocity at lift, in scroll units per ms, from the deltas of the last
// velocity_window_ms before liftMs. Time the fingers spent still just before
// lifting counts against it, so "scroll, pause, lift" launches nothing.
void KineticState::computeLaunchVelocity(uint32_t liftMs) {
    const uint32_t window = std::max<int64_t>(8, getKineticConfigInt("velocity_window_ms", 64));
    const double   mult   = getKineticConfigFloat("delta_multiplier", 1.0);

    m_velocityV = 0.0;
    m_velocityH = 0.0;

    const uint32_t tail = liftMs >= m_lastEventMs ? liftMs - m_lastEventMs : 0;
    if (tail > window)
        return;

    double   sumV  = 0.0;
    double   sumH  = 0.0;
    uint32_t sumDt = 0;
    for (auto it = m_samples.rbegin(); it != m_samples.rend(); ++it) {
        if (it->t > liftMs)
            continue;
        if (liftMs - it->t > window)
            break;
        if (it->dt == 0)
            continue; // first event of a gesture: covers unknown time
        sumV += it->dv;
        sumH += it->dh;
        sumDt += it->dt;
    }

    const double span = static_cast<double>(sumDt + tail);
    if (sumDt == 0 || span <= 0.0)
        return;

    m_velocityV = sumV / span * mult;
    m_velocityH = sumH / span * mult;

    if (Metrics::g_metrics.enabled()) {
        auto& g   = Metrics::g_metrics.gesture();
        g.launchV = m_velocityV;
        g.launchH = m_velocityH;
        g.span    = span;
        g.tail    = tail;
    }

    if (getKineticConfigInt("debug", 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] launch v=" << m_velocityV << "/ms h=" << m_velocityH << "/ms span=" << span << "ms tail=" << tail
                << " samples=" << m_samples.size() << "\n";
    }
}

int KineticState::onStopTimer(void* data) {
    auto* self = static_cast<KineticState*>(data);

    if (getKineticConfigInt("debug", 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open()) {
            log << "[hypr-kinetic-scroll] stopTimer tracking=" << (self->m_tracking ? 1 : 0)
                << " v=" << self->m_velocityV << " h=" << self->m_velocityH << " self=" << self << "\n";
        }
    }

    if (!self->m_tracking)
        return 0;

    if (self->m_cancelOnStopTimer) {
        self->stopKinetic("gestureIdle");
        return 0;
    }

    if (Metrics::g_metrics.enabled())
        Metrics::g_metrics.gesture().tLift = Metrics::nowNs();
    self->computeLaunchVelocity(self->m_lastEventMs);
    self->beginDecay("fallbackTimeout");
    return 0;
}

void KineticState::beginDecay(const char* reason) {
    if (!m_tracking)
        return;

    if (getKineticConfigInt("debug", 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] beginDecay reason=" << reason << " self=" << this << "\n";
    }

    // min_velocity is in scroll units per 16 ms, the same scale as one real
    // touchpad frame, so the number is comparable to the deltas in the debug log.
    const double minVelocity = getKineticConfigFloat("min_velocity", 0.1);
    if (std::abs(m_velocityV) * 16.0 < minVelocity && std::abs(m_velocityH) * 16.0 < minVelocity) {
        stopKinetic("belowThreshold");
        return;
    }

    m_tracking = false;
    m_decaying = true;
    m_cancelOnStopTimer = false;
    m_lastTick = std::chrono::steady_clock::now();

    // Only touchpad gestures have a client-visible lift to own; smooth-mouse
    // sequences never produce a stop event.
    m_clientInGesture = m_source == WL_POINTER_AXIS_SOURCE_FINGER;
    m_activeV         = std::abs(m_velocityV) * 16.0 >= minVelocity;
    m_activeH         = std::abs(m_velocityH) * 16.0 >= minVelocity;

    if (Metrics::g_metrics.enabled()) {
        auto& g    = Metrics::g_metrics.gesture();
        g.tLaunch  = Metrics::nowNs();
        g.launched = true;
    }

    // Emit in lockstep with the monitor that shows the target window, and use
    // the same scroll factor Hyprland applies to that window's real touchpad
    // events (window rule scroll_touchpad overrides input:touchpad:scroll_factor).
    static auto PSCROLLFACTOR = CConfigValue<Hyprlang::FLOAT>("input:touchpad:scroll_factor");
    m_scrollFactor    = *PSCROLLFACTOR;
    m_targetMonitorId = MONITOR_INVALID;
    m_frameMs         = 1000.0 / 60.0;
    PHLMONITOR mon;
    if (const auto PWIN = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr) {
        mon = PWIN->m_monitor.lock();
        if (PWIN->isScrollTouchpadOverridden())
            m_scrollFactor = PWIN->getScrollTouchpad();
    }
    // No window monitor: MONITOR_INVALID makes onRenderPre accept any monitor's frame.
    if (mon) {
        m_targetMonitorId = mon->m_id;
        if (mon->m_refreshRate > 1.0f)
            m_frameMs = 1000.0 / mon->m_refreshRate;
    }

    wl_event_source_timer_update(m_stopTimer, 0);
    wl_event_source_timer_update(m_decayTimer, std::max<int64_t>(1, getKineticConfigInt("interval_ms", 16)));
}

int KineticState::onDecayTimer(void* data) {
    auto* self = static_cast<KineticState*>(data);
    if (!self->m_decaying)
        return 0;
    // Watchdog: the per-frame render hook normally emits. If no frame came
    // (nothing damaged, monitor mismatch, DPMS), keep the fling alive from here.
    self->step(false);
    return 0;
}

void KineticState::onRenderPre(PHLMONITOR mon) {
    if (!m_decaying || !mon) {
        if (Metrics::g_metrics.enabled())
            ++Metrics::g_metrics.global().idleRender;
        return;
    }
    if (m_targetMonitorId != MONITOR_INVALID && mon->m_id != m_targetMonitorId)
        return;
    step(true);
}

bool KineticState::targetStillValid() {
    const auto targetKeys = currentScrollTargetKeys();

    if (getKineticConfigInt("stop_on_target_change", 1) && m_scrollTargetWindowKey != 0) {
        const bool windowChanged  = targetKeys.windowKey != 0 && targetKeys.windowKey != m_scrollTargetWindowKey;
        const bool surfaceChanged = targetKeys.surfaceKey != 0 && targetKeys.surfaceKey != m_scrollTargetSurfaceKey;
        if (windowChanged || surfaceChanged) {
            stopKinetic("targetChangedDecay");
            return false;
        }
    }

    const auto PWIN = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
    if (PWIN) {
        const bool hasRule = hasAppRule(PWIN->m_class);
        if (classInList(PWIN->m_class, getKineticConfigString("disabled_classes", ""))) {
            stopKinetic("disabledClassesDecay");
            return false;
        }

        if (!shouldProcessForWindow(PWIN->m_class)) {
            stopKinetic("appRuleDecay");
            return false;
        }

        if (getKineticConfigInt("disable_in_browser", 1) && !hasRule && classLooksLikeBrowser(PWIN->m_class)) {
            stopKinetic("browserDecay");
            return false;
        }
    }

    return true;
}

// One momentum step. Called once per compositor frame of the target monitor
// (fromRender), or from the watchdog timer when no frame arrives.
void KineticState::step(bool fromRender) {
    Metrics::CScope stepScope(Metrics::g_metrics.enabled() ? &Metrics::g_metrics.gesture().stepNs : nullptr);

    if (!targetStillValid()) {
        stepScope.retarget(nullptr); // gesture already finalized
        return;
    }

    // Time-based decay: v(t) = v0 * e^(-lambda t). The emitted delta is the exact
    // integral over the elapsed wall time, so timer jitter changes neither the
    // total distance nor the perceived speed.
    const int  interval = std::max<int64_t>(1, getKineticConfigInt("interval_ms", 16));
    const auto now      = std::chrono::steady_clock::now();
    double     dt       = std::chrono::duration<double, std::milli>(now - m_lastTick).count();

    // A render right after a watchdog emission: nothing meaningful elapsed.
    if (fromRender && dt < 0.25 * m_frameMs)
        return;

    if (Metrics::g_metrics.enabled()) {
        auto& g = Metrics::g_metrics.gesture();
        if (fromRender) {
            ++g.stepsRender;
            g.renderDtMs.add(dt);
        } else {
            ++g.stepsTimer;
        }
    }

    m_lastTick = now;
    if (dt <= 0.0)
        dt = fromRender ? m_frameMs : interval;
    if (dt > 100.0)
        dt = 100.0; // lag or suspend: don't jump the content

    // decel is the velocity multiplier per 16 ms. 0.967 is close to the macOS
    // "normal" rate of 0.998 per ms; the original 0.92 dies in half a second.
    const double decel  = std::clamp(getKineticConfigFloat("decel", 0.967), 0.5, 0.9999);
    const double lambda = -std::log(decel) / 16.0;
    const double k      = std::exp(-lambda * dt);
    const double travel = (1.0 - k) / lambda;

    const double deltaV = m_velocityV * travel;
    const double deltaH = m_velocityH * travel;
    m_velocityV *= k;
    m_velocityH *= k;

    emitSyntheticScroll(deltaV, deltaH);

    if (getKineticConfigInt("debug", 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] step src=" << (fromRender ? "render" : "timer") << " dt=" << dt << " dV=" << deltaV << " v=" << m_velocityV << "\n";
    }

    const double minVelocity = getKineticConfigFloat("min_velocity", 0.1);
    const bool   activeV     = std::abs(m_velocityV) * 16.0 >= minVelocity;
    const bool   activeH     = std::abs(m_velocityH) * 16.0 >= minVelocity;
    if (!activeV && !activeH) {
        stepScope.retarget(nullptr); // gesture is finalized inside stopKinetic
        stopKinetic("decayDone");
        return;
    }

    // After a real frame, the watchdog only needs to catch a missing next frame.
    // If the watchdog itself fired, frames aren't coming: poll at interval_ms.
    const int watchdog = fromRender ? std::max<int>(interval, static_cast<int>(std::ceil(2.5 * m_frameMs))) : interval;
    wl_event_source_timer_update(m_decayTimer, watchdog);
}

void KineticState::emitSyntheticScroll(double deltaV, double deltaH) {
    auto         now          = std::chrono::steady_clock::now();
    uint32_t     timeMs       = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const double scrollFactor = m_scrollFactor;

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
    if (deltaV != 0.0) {
        g_pSeatManager->sendPointerAxis(
            timeMs,
            WL_POINTER_AXIS_VERTICAL_SCROLL,
            deltaV * scrollFactor,
            0, // discrete
            0, // v120
            m_source,
            WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    if (deltaH != 0.0) {
        g_pSeatManager->sendPointerAxis(
            timeMs,
            WL_POINTER_AXIS_HORIZONTAL_SCROLL,
            deltaH * scrollFactor,
            0,
            0,
            m_source,
            WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    g_pSeatManager->sendPointerFrame();
}

// The axis_stop the client never got: one per axis that was scrolling, to the
// surface that owns the gesture. If pointer focus moved elsewhere the old
// client is left as it would be after any focus change mid-gesture.
void KineticState::sendGestureStop() {
    if (!g_pSeatManager)
        return;
    const auto PSURF = g_pSeatManager->m_state.pointerFocus.lock();
    if (!PSURF || reinterpret_cast<uintptr_t>(PSURF.get()) != m_scrollTargetSurfaceKey)
        return;

    const auto     now    = std::chrono::steady_clock::now();
    const uint32_t timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // sendPointerAxis with value 0 and a non-wheel source emits axis + axis_stop.
    if (m_activeV)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, 0.0, 0, 0, m_source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (m_activeH)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0.0, 0, 0, m_source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (m_activeV || m_activeH)
        g_pSeatManager->sendPointerFrame();

    if (Metrics::g_metrics.enabled())
        Metrics::g_metrics.gesture().stopSent = true;
}
