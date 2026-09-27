#include "kinetic.hpp"
#include "globals.hpp"
#include "metrics.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string_view>

// ---------------------------------------------------------------------------
// Window policy (evaluated once per gesture)

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

bool KineticState::windowAllowed(const std::string& cls) const {
    if (const auto it = m_perAppRules.find(cls); it != m_perAppRules.end())
        return it->second; // explicit rule wins over everything, including the browser heuristic

    if (classInList(cls, *m_cfg.disabledClasses))
        return false;

    if (!m_defaultAppRule)
        return false;

    if (*m_cfg.disableInBrowser && classLooksLikeBrowser(cls))
        return false;

    return true;
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

// ---------------------------------------------------------------------------
// Construction, timer, touchpads

KineticState::KineticState() {
    m_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTimer, this);
    rescanTouchpads();
}

KineticState::~KineticState() {
    if (m_timer)
        wl_event_source_remove(m_timer);
}

// Listen on every touchpad for contact (motion / hold begin) and frame events.
// Called at construction and whenever the pointer list changed (hotplug).
void KineticState::rescanTouchpads() {
    if (!g_pInputManager)
        return;

    m_touchpadListeners.clear();
    m_pointersSeen = g_pInputManager->m_pointers.size();

    for (const auto& pointer : g_pInputManager->m_pointers) {
        if (!pointer || !pointer->m_isTouchpad)
            continue;

        m_touchpadListeners.emplace_back(pointer->m_pointerEvents.frame.listen([this] { onPointerFrame(); }));
        m_touchpadListeners.emplace_back(pointer->m_pointerEvents.motion.listen([this](const IPointer::SMotionEvent&) { onTouchpadContact(); }));
        m_touchpadListeners.emplace_back(pointer->m_pointerEvents.holdBegin.listen([this](const IPointer::SHoldBeginEvent&) { onTouchpadContact(); }));
    }
}

std::ostream* KineticState::log() {
    if (!m_g.debug)
        return nullptr;
    if (!m_log.is_open())
        m_log.open("/tmp/hypr-kinetic-scroll.log", std::ios::app);
    return m_log.is_open() ? &m_log : nullptr;
}

int KineticState::onTimer(void* data) {
    auto* self = static_cast<KineticState*>(data);
    switch (self->m_state) {
        case eState::IDLE: break;
        case eState::IGNORED:
            // Long enough without a stop event: whatever that was, it is over.
            self->resetGesture();
            break;
        case eState::TRACKING:
            if (self->m_timerLaunches) {
                // Smooth mouse (no stop event) or a device that ended the
                // sequence with an axis-less frame: silence means lifted.
                if (Metrics::g_metrics.enabled())
                    Metrics::g_metrics.gesture().tLift = Metrics::nowNs();
                self->computeLaunchVelocity(self->m_lastEventMs);
                self->launch("timeout");
            } else
                self->stopKinetic("gestureIdle"); // fingers resting on the pad
            break;
        case eState::DECAYING:
            // Watchdog: the render hook normally emits. No frame came (nothing
            // damaged, DPMS, monitor mismatch): keep the fling alive from here.
            self->step(false);
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Gesture start

bool KineticState::beginGesture(const IPointer::SAxisEvent& e, bool touchpadSource) {
    if (g_pInputManager && g_pInputManager->m_pointers.size() != m_pointersSeen)
        rescanTouchpads();

    m_g = {};
    m_g.touchpad = touchpadSource;
    m_g.source   = touchpadSource ? e.source : WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    m_g.debug    = *m_cfg.debug != 0;

    const auto PWIN = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
    if (PWIN && !windowAllowed(PWIN->m_class)) {
        m_state = eState::IGNORED;
        wl_event_source_timer_update(m_timer, 500);
        return false;
    }

    // Snapshot everything the momentum phase needs.
    m_g.scrollFactor = *m_cfg.touchpadScrollFactor;
    if (PWIN) {
        m_g.window  = PWIN;
        m_g.monitor = PWIN->m_monitor;
        if (PWIN->isScrollTouchpadOverridden())
            m_g.scrollFactor = PWIN->getScrollTouchpad();
        if (const auto MON = PWIN->m_monitor.lock(); MON && MON->m_refreshRate > 1.0f)
            m_g.frameMs = 1000.0 / MON->m_refreshRate;
    }
    if (g_pSeatManager)
        m_g.surface = g_pSeatManager->m_state.pointerFocus;

    m_g.stopOnTargetChange = *m_cfg.stopOnTargetChange != 0;
    m_g.intervalMs         = std::max<int64_t>(1, *m_cfg.intervalMs);
    m_g.windowMs           = std::max<int64_t>(8, *m_cfg.velocityWindowMs);
    m_g.launchMultiplier   = *m_cfg.deltaMultiplier;
    // decel is the velocity multiplier per 16 ms (0.967 ~ macOS). Convert to a
    // continuous rate so the integral is exact for any frame time.
    const double decel = std::clamp<double>(*m_cfg.decel, 0.5, 0.9999);
    m_g.lambda         = -std::log(decel) / 16.0;
    // min_velocity is in scroll units per 16 ms, comparable to real deltas.
    m_g.minVelPerMs = *m_cfg.minVelocity / 16.0;

    m_sampleHead  = 0;
    m_sampleCount = 0;
    m_lastEventMs = 0;
    m_state       = eState::TRACKING;

    // Metrics sink is re-read once per gesture (string copy); hot paths only
    // test the cached bool.
    {
        const std::string metricsPath = *m_cfg.metricsFile;
        Metrics::g_metrics.setEnabled(!metricsPath.empty(), metricsPath);
    }
    if (Metrics::g_metrics.enabled())
        Metrics::g_metrics.gestureBegin(Metrics::nowNs(), PWIN ? PWIN->m_class : std::string{});

    return true;
}

void KineticState::resetGesture() {
    m_state           = eState::IDLE;
    m_sampleHead      = 0;
    m_sampleCount     = 0;
    m_lastEventMs     = 0;
    m_axisInFrame     = false;
    m_timerLaunches   = false;
    m_velocityV       = 0.0;
    m_velocityH       = 0.0;
    m_activeV         = false;
    m_activeH         = false;
    m_clientInGesture = false;
    m_g               = {};
    wl_event_source_timer_update(m_timer, 0);
}

// ---------------------------------------------------------------------------
// Axis events

bool KineticState::onAxis(IPointer::SAxisEvent& e) {
    // Metrics: attribute this call's cost to the gesture or to the idle bucket.
    struct SAxisScope {
        KineticState* self;
        int64_t       t0;
        int64_t       flush0;
        ~SAxisScope() {
            auto& m = Metrics::g_metrics;
            if (!m.enabled())
                return;
            const double ns = static_cast<double>(Metrics::nowNs() - t0 - (m.flushNsTotal() - flush0));
            if (m.active() && (self->m_state == eState::TRACKING || self->m_state == eState::DECAYING))
                m.gesture().axisNs.add(ns);
            else
                m.global().idleAxisNs.add(ns);
        }
    } axisScope{this, Metrics::nowNs(), Metrics::g_metrics.flushNsTotal()};

    if (!*m_cfg.enabled)
        return false;

    // Touchpad scrolling, plus mice that report smooth-only deltas.
    const bool touchpadSource = e.source == WL_POINTER_AXIS_SOURCE_FINGER || e.source == WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    const bool smoothMouse    = e.mouse && e.deltaDiscrete == 0;
    if (!touchpadSource && !smoothMouse)
        return false;

    const bool isStop = e.delta == 0.0 && e.deltaDiscrete == 0;

    switch (m_state) {
        case eState::IGNORED:
            if (isStop)
                resetGesture();
            return false;

        case eState::DECAYING:
            if (isStop) {
                // Gesture ownership: the client must not see the finger lift
                // while momentum is live. Delivered, it would end the gesture
                // (GTK's scroll-end) or start the client's own fling on top of
                // ours (GTK4, Chromium). libinput sends one stop per axis; all
                // are swallowed. We send the stop when momentum ends.
                if (Metrics::g_metrics.enabled())
                    ++Metrics::g_metrics.gesture().stopsCancelled;
                return true;
            }
            // Fingers back on the pad and moving: the new swipe takes over.
            // The client's gesture simply continues with these real events.
            stopKinetic("newGesture");
            [[fallthrough]];

        case eState::IDLE:
            if (isStop)
                return false;
            if (!beginGesture(e, touchpadSource))
                return false;
            break;

        case eState::TRACKING: break;
    }

    // TRACKING.
    if (isStop) {
        if (!m_g.touchpad)
            return false;
        // libinput's axis_stop: fingers left the pad. Launch synchronously; any
        // wait here is a visible hitch. If nothing launches (pause before lift,
        // below threshold) the real stop passes through.
        m_axisInFrame = true;
        if (Metrics::g_metrics.enabled())
            Metrics::g_metrics.gesture().tLift = Metrics::nowNs();
        computeLaunchVelocity(e.timeMs);
        if (!launch("axisStop"))
            return false;
        if (Metrics::g_metrics.enabled())
            ++Metrics::g_metrics.gesture().stopsCancelled;
        return true;
    }

    if (touchpadSource != m_g.touchpad)
        return false; // mixed devices mid-gesture: not ours

    m_axisInFrame = m_g.touchpad;
    recordSample(e);

    // Touchpad: idle safety only. Fingers resting this long end the gesture, so
    // a later lift launches nothing (the velocity window already yields ~0 for
    // a pause; this covers a missing stop event). Smooth mouse: no stop event
    // exists, so a short silence means "lifted".
    m_timerLaunches = !m_g.touchpad;
    wl_event_source_timer_update(m_timer, m_g.touchpad ? 200 : 50);
    return false;
}

void KineticState::recordSample(const IPointer::SAxisEvent& e) {
    const bool vertical = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL;

    SSample* last = m_sampleCount ? &m_samples[(m_sampleHead + SAMPLE_CAP - 1) % SAMPLE_CAP] : nullptr;
    if (last && last->t == e.timeMs) {
        // Same frame, other axis: merge.
        if (vertical)
            last->dv += e.delta;
        else
            last->dh += e.delta;
    } else {
        SSample s;
        s.t = e.timeMs;
        if (m_lastEventMs > 0 && e.timeMs >= m_lastEventMs && e.timeMs - m_lastEventMs < 200)
            s.dt = e.timeMs - m_lastEventMs;
        if (vertical)
            s.dv = e.delta;
        else
            s.dh = e.delta;
        m_samples[m_sampleHead] = s;
        m_sampleHead            = (m_sampleHead + 1) % SAMPLE_CAP;
        if (m_sampleCount < SAMPLE_CAP)
            ++m_sampleCount;
    }

    m_lastEventMs = e.timeMs;

    if (Metrics::g_metrics.enabled() && Metrics::g_metrics.active())
        Metrics::g_metrics.gesture().samples.push_back({e.timeMs, vertical ? e.delta : 0.0, vertical ? 0.0 : e.delta});
}

void KineticState::onPointerFrame() {
    if (m_axisInFrame) {
        m_axisInFrame = false;
        return;
    }

    if (m_state != eState::TRACKING || !m_g.touchpad)
        return;

    // Fallback only: the lift is normally handled synchronously from the
    // zero-delta axis event. If a device ends a sequence with an axis-less
    // frame and no stop event, launch after a short grace period.
    m_timerLaunches = true;
    wl_event_source_timer_update(m_timer, 100);
}

void KineticState::onTouchpadContact() {
    if (m_state == eState::TRACKING || m_state == eState::DECAYING)
        stopKinetic("touchpadContact");
}

// ---------------------------------------------------------------------------
// Launch

// Velocity at lift, in scroll units per ms, from the deltas of the last
// windowMs before liftMs. Time the fingers spent still just before lifting
// counts against it, so "scroll, pause, lift" launches nothing.
void KineticState::computeLaunchVelocity(uint32_t liftMs) {
    m_velocityV = 0.0;
    m_velocityH = 0.0;

    const uint32_t tail = liftMs >= m_lastEventMs ? liftMs - m_lastEventMs : 0;
    if (tail > m_g.windowMs)
        return;

    double   sumV  = 0.0;
    double   sumH  = 0.0;
    uint32_t sumDt = 0;
    for (size_t i = 0; i < m_sampleCount; ++i) {
        const auto& s = m_samples[(m_sampleHead + SAMPLE_CAP - 1 - i) % SAMPLE_CAP];
        if (s.t > liftMs)
            continue;
        if (liftMs - s.t > m_g.windowMs)
            break;
        if (s.dt == 0)
            continue; // first event of a gesture: covers unknown time
        sumV += s.dv;
        sumH += s.dh;
        sumDt += s.dt;
    }

    const double span = static_cast<double>(sumDt + tail);
    if (sumDt == 0 || span <= 0.0)
        return;

    m_velocityV = sumV / span * m_g.launchMultiplier;
    m_velocityH = sumH / span * m_g.launchMultiplier;

    if (Metrics::g_metrics.enabled()) {
        auto& g   = Metrics::g_metrics.gesture();
        g.launchV = m_velocityV;
        g.launchH = m_velocityH;
        g.span    = span;
        g.tail    = tail;
    }

    if (auto* l = log())
        *l << "[hypr-kinetic-scroll] launch v=" << m_velocityV << "/ms h=" << m_velocityH << "/ms span=" << span << "ms tail=" << tail << " samples=" << m_sampleCount
           << "\n";
}

bool KineticState::launch(const char* reason) {
    if (m_state != eState::TRACKING)
        return false;

    m_activeV = std::abs(m_velocityV) >= m_g.minVelPerMs;
    m_activeH = std::abs(m_velocityH) >= m_g.minVelPerMs;
    if (!m_activeV && !m_activeH) {
        stopKinetic("belowThreshold");
        return false;
    }

    m_state           = eState::DECAYING;
    m_lastTick        = std::chrono::steady_clock::now();
    m_clientInGesture = m_g.touchpad; // smooth-mouse sequences have no client-visible lift to own

    if (Metrics::g_metrics.enabled()) {
        auto& g    = Metrics::g_metrics.gesture();
        g.tLaunch  = Metrics::nowNs();
        g.launched = true;
    }

    if (auto* l = log())
        *l << "[hypr-kinetic-scroll] beginDecay reason=" << reason << "\n";

    // Nothing is damaged after the fingers stop, so without this the first
    // momentum frame would wait for the watchdog: a visible hitch at lift.
    if (const auto MON = m_g.monitor.lock())
        MON->scheduleFrame();

    wl_event_source_timer_update(m_timer, m_g.intervalMs);
    return true;
}

// ---------------------------------------------------------------------------
// Momentum

void KineticState::onRenderPre(PHLMONITOR mon) {
    if (m_state != eState::DECAYING || !mon) {
        if (Metrics::g_metrics.enabled())
            ++Metrics::g_metrics.global().idleRender;
        return;
    }
    if (!m_g.monitor.expired() && m_g.monitor != mon)
        return;
    step(true);
}

bool KineticState::targetStillValid() {
    if (!m_g.stopOnTargetChange)
        return true;

    if (g_pInputManager && !m_g.window.expired() && g_pInputManager->m_lastMouseFocus != m_g.window) {
        stopKinetic("targetChangedDecay");
        return false;
    }
    if (g_pSeatManager && !m_g.surface.expired() && g_pSeatManager->m_state.pointerFocus != m_g.surface) {
        stopKinetic("targetChangedDecay");
        return false;
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

    const auto now = std::chrono::steady_clock::now();
    double     dt  = std::chrono::duration<double, std::milli>(now - m_lastTick).count();

    // A render right after a watchdog emission: nothing meaningful elapsed.
    if (fromRender && dt < 0.25 * m_g.frameMs)
        return;

    if (Metrics::g_metrics.enabled()) {
        auto& g = Metrics::g_metrics.gesture();
        if (fromRender) {
            ++g.stepsRender;
            g.renderDtMs.add(dt);
        } else
            ++g.stepsTimer;
    }

    m_lastTick = now;
    if (dt <= 0.0)
        dt = fromRender ? m_g.frameMs : m_g.intervalMs;
    if (dt > 100.0)
        dt = 100.0; // lag or suspend: don't jump the content

    // v(t) = v0 e^(-lambda t); emit the exact integral over the elapsed wall
    // time, so timer jitter changes neither distance nor perceived speed.
    const double k      = std::exp(-m_g.lambda * dt);
    const double travel = (1.0 - k) / m_g.lambda;

    const double deltaV = m_velocityV * travel;
    const double deltaH = m_velocityH * travel;
    m_velocityV *= k;
    m_velocityH *= k;

    emitSyntheticScroll(deltaV, deltaH);

    if (auto* l = log())
        *l << "[hypr-kinetic-scroll] step src=" << (fromRender ? "render" : "timer") << " dt=" << dt << " dV=" << deltaV << " v=" << m_velocityV << "\n";

    m_activeV = std::abs(m_velocityV) >= m_g.minVelPerMs;
    m_activeH = std::abs(m_velocityH) >= m_g.minVelPerMs;
    if (!m_activeV && !m_activeH) {
        stepScope.retarget(nullptr); // gesture is finalized inside stopKinetic
        stopKinetic("decayDone");
        return;
    }

    // After a real frame the watchdog only needs to catch a missing next frame.
    // If the watchdog itself fired, frames aren't coming: poll at interval_ms.
    const int watchdog = fromRender ? std::max<int>(m_g.intervalMs, static_cast<int>(std::ceil(2.5 * m_g.frameMs))) : m_g.intervalMs;
    wl_event_source_timer_update(m_timer, watchdog);
}

void KineticState::emitSyntheticScroll(double deltaV, double deltaH) {
    if (!g_pSeatManager || (deltaV == 0.0 && deltaH == 0.0))
        return;

    const auto     now    = std::chrono::steady_clock::now();
    const uint32_t timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    if (Metrics::g_metrics.enabled()) {
        auto& g = Metrics::g_metrics.gesture();
        ++g.emits;
        g.travelV += std::abs(deltaV);
        g.travelH += std::abs(deltaH);
        if (g.tFirstEmit == 0)
            g.tFirstEmit = Metrics::nowNs();
    }

    // Same axis source as the finger phase, so the client sees one gesture.
    if (deltaV != 0.0)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, deltaV * m_g.scrollFactor, 0, 0, m_g.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (deltaH != 0.0)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, deltaH * m_g.scrollFactor, 0, 0, m_g.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);

    g_pSeatManager->sendPointerFrame();
}

// The axis_stop the client never got: one per axis that was scrolling, to the
// surface that owns the gesture. If pointer focus moved elsewhere the old
// client is left as it would be after any focus change mid-gesture.
void KineticState::sendGestureStop() {
    if (!g_pSeatManager || m_g.surface.expired() || g_pSeatManager->m_state.pointerFocus != m_g.surface)
        return;

    const auto     now    = std::chrono::steady_clock::now();
    const uint32_t timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // sendPointerAxis with value 0 and a non-wheel source emits axis + axis_stop.
    if (m_activeV)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, 0.0, 0, 0, m_g.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (m_activeH)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0.0, 0, 0, m_g.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (m_activeV || m_activeH)
        g_pSeatManager->sendPointerFrame();

    if (Metrics::g_metrics.enabled())
        Metrics::g_metrics.gesture().stopSent = true;
}

// ---------------------------------------------------------------------------
// End of gesture

void KineticState::stopKinetic(const char* reason) {
    if (m_state == eState::IDLE)
        return;

    // Close the gesture for the client if we swallowed its finger lift. Not on
    // newGesture: the real axis event that triggered it is about to be
    // delivered and simply continues the same gesture, as a re-touch on macOS.
    if (m_clientInGesture && (!reason || std::string_view{reason} != "newGesture"))
        sendGestureStop();

    if (auto* l = log())
        *l << "[hypr-kinetic-scroll] stopKinetic reason=" << (reason ? reason : "(null)") << "\n";

    Metrics::g_metrics.gestureEnd(Metrics::nowNs(), reason);
    resetGesture();
}
