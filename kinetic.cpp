#include "kinetic.hpp"
#include "globals.hpp"
#include "metrics.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/InputCapture.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

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

static bool inputCaptured() {
    return PROTO::inputCapture && PROTO::inputCapture->isCaptured();
}

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
// Construction, timer, devices

KineticState::KineticState() {
    m_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTimer, this);
    Metrics::g_metrics.setLoop(g_pCompositor->m_wlEventLoop);
    rescanDevices();
}

KineticState::~KineticState() {
    // Unloading mid-momentum must not leave the client waiting for the lift we
    // swallowed: close the gesture first, while the seat is still there.
    if (m_state != eState::IDLE)
        stopKinetic(eStop::UNLOAD);
    Metrics::g_metrics.flushNow();
    if (m_timer)
        wl_event_source_remove(m_timer);
}

// True if a pointer device appeared, vanished, or was replaced since the last
// scan. Compares device identity (weak pointers), not the device count.
bool KineticState::devicesStale() const {
    if (!g_pInputManager)
        return false;
    for (const auto& wp : m_devices)
        if (wp.expired())
            return true;
    size_t seen = 0;
    for (const auto& pointer : g_pInputManager->m_pointers) {
        if (!pointer)
            continue;
        ++seen;
        if (std::ranges::none_of(m_devices, [&](const auto& wp) { return wp == pointer; }))
            return true;
    }
    return seen != m_devices.size();
}

// Every pointer device: remember which one scrolled last (the bus axis event
// carries no device; this listener runs after Hyprland's dispatch of the same
// event, so it is current from the second report of a gesture on). Touchpads
// additionally: contact (motion / hold begin) cancels momentum.
void KineticState::rescanDevices() {
    if (!g_pInputManager)
        return;

    m_deviceListeners.clear();
    m_devices.clear();

    for (const auto& pointer : g_pInputManager->m_pointers) {
        if (!pointer)
            continue;

        m_devices.emplace_back(pointer);
        WP<IPointer> wp = pointer;
        m_deviceListeners.emplace_back(pointer->m_pointerEvents.axis.listen([this, wp](const IPointer::SAxisEvent&) { m_lastAxisDevice = wp; }));

        if (!pointer->m_isTouchpad)
            continue;
        m_deviceListeners.emplace_back(pointer->m_pointerEvents.motion.listen([this](const IPointer::SMotionEvent&) { onTouchpadContact(); }));
        m_deviceListeners.emplace_back(pointer->m_pointerEvents.holdBegin.listen([this](const IPointer::SHoldBeginEvent&) { onTouchpadContact(); }));
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
            self->resetGesture(false);
            break;
        case eState::TRACKING:
            if (self->m_timerLaunches) {
                // Smooth mouse only: no stop event exists, silence is the lift.
                // (Touchpad gestures launch solely on libinput's real stop.)
                if (Metrics::g_metrics.enabled())
                    Metrics::g_metrics.gesture().tLift = Metrics::nowNs();
                self->launch(self->m_est.lastMs(), "timeout");
            } else
                self->stopKinetic(eStop::GESTURE_IDLE); // fingers resting on the pad
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
// Target identity

KineticState::eTarget KineticState::targetState() const {
    if (!m_g.hadWindow && !m_g.hadSurface)
        return eTarget::NONE;

    if (m_g.hadWindow) {
        if (m_g.window.expired())
            return eTarget::DESTROYED;
        if (g_pInputManager && g_pInputManager->m_lastMouseFocus != m_g.window)
            return eTarget::CHANGED;
    }
    if (m_g.hadSurface) {
        if (m_g.surface.expired())
            return eTarget::DESTROYED;
        if (g_pSeatManager && g_pSeatManager->m_state.pointerFocus != m_g.surface)
            return eTarget::CHANGED;
    }
    return eTarget::SAME;
}

// ---------------------------------------------------------------------------
// Gesture start

bool KineticState::beginGesture(const IPointer::SAxisEvent& e, bool touchpadSource) {
    if (devicesStale())
        rescanDevices();

    m_g          = {};
    m_g.touchpad = touchpadSource;
    m_g.source   = touchpadSource ? e.source : WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    m_g.debug    = *m_cfg.debug != 0;

    const auto PWIN  = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
    const auto PSURF = g_pSeatManager ? g_pSeatManager->m_state.pointerFocus.lock() : nullptr;

    // Nothing to own without a surface: momentum would land on whatever the
    // pointer enters next. A window rule can also exclude the gesture.
    if (!PSURF || (PWIN && !windowAllowed(PWIN->m_class))) {
        m_state = eState::IGNORED;
        wl_event_source_timer_update(m_timer, 500);
        return false;
    }

    // Snapshot everything the momentum phase needs.
    m_g.surface    = PSURF;
    m_g.hadSurface = true;
    if (PWIN) {
        m_g.window    = PWIN;
        m_g.hadWindow = true;
        m_g.monitor   = PWIN->m_monitor;
        if (const auto MON = PWIN->m_monitor.lock(); MON && MON->m_refreshRate > 1.0f)
            m_g.frameMs = 1000.0 / MON->m_refreshRate;
    }

    m_g.stopOnTargetChange = *m_cfg.stopOnTargetChange != 0;
    m_g.intervalMs         = std::max<int64_t>(1, *m_cfg.intervalMs);
    m_g.windowMs           = std::max<int64_t>(8, *m_cfg.velocityWindowMs);
    m_g.tailGraceMs        = std::max<int64_t>(0, *m_cfg.liftTailGraceMs);
    m_g.tailFadeMs         = std::max<int64_t>(m_g.tailGraceMs + 1, *m_cfg.liftTailFadeMs);
    m_g.launchMultiplier   = *m_cfg.deltaMultiplier;
    m_g.decay              = Physics::SDecay::fromDecelPer16ms(*m_cfg.decel);
    // min_velocity is in scroll units per 16 ms, comparable to real deltas. A
    // zero cutoff would let a zero-velocity fling run forever: floor it.
    m_g.minVelPerMs = std::max(1e-4, static_cast<double>(*m_cfg.minVelocity) / 16.0);

    m_est.reset();
    m_state = eState::TRACKING;

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

void KineticState::resetGesture(bool keepOwedStops) {
    m_state             = eState::IDLE;
    m_timerLaunches     = false;
    m_velocityV         = 0.0;
    m_velocityH         = 0.0;
    m_lastStepFromTimer = false;
    if (!keepOwedStops) {
        m_owedStops = 0;
        m_lastEmitSurface.reset();
    }
    m_est.reset();
    m_g = {};
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
    if (!*m_cfg.enabled) {
        if (m_state != eState::IDLE)
            stopKinetic(eStop::RULE_DISABLED);
        return false;
    }
    if (inputCaptured()) {
        if (m_state != eState::IDLE)
            stopKinetic(eStop::CAPTURED);
        return false;
    }

    // Touchpad scrolling, plus mice that report smooth-only deltas.
    const bool touchpadSource = e.source == WL_POINTER_AXIS_SOURCE_FINGER || e.source == WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    const bool smoothMouse    = e.mouse && e.deltaDiscrete == 0;
    if (!touchpadSource && !smoothMouse)
        return false;

    const bool    isStop  = e.delta == 0.0 && e.deltaDiscrete == 0;
    const uint8_t axisBit = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 1 : 2;

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
            stopKinetic(eStop::NEW_GESTURE);
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
            const auto ts = targetState();
            if (ts == eTarget::CHANGED || ts == eTarget::DESTROYED) {
                stopKinetic(ts == eTarget::CHANGED ? eStop::TARGET_CHANGED : eStop::TARGET_DESTROYED);
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

    m_est.add(e.timeMs, e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL, e.delta);
    if (Metrics::g_metrics.enabled() && Metrics::g_metrics.active())
        Metrics::g_metrics.addSample(e.timeMs, e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? e.delta : 0.0, e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 0.0 : e.delta);

    // Touchpad: idle safety only. Fingers resting this long end the gesture, so
    // a later lift launches nothing (the velocity fade already yields ~0 for a
    // pause; this covers a missing stop event). Smooth mouse: no stop event
    // exists, so a short silence means "lifted".
    m_timerLaunches = !m_g.touchpad;
    wl_event_source_timer_update(m_timer, m_g.touchpad ? 200 : 50);
    return false;
}

void KineticState::onTouchpadContact() {
    if (m_state == eState::TRACKING || m_state == eState::DECAYING)
        stopKinetic(eStop::TOUCHPAD_CONTACT);
}

// ---------------------------------------------------------------------------
// Launch

// Mirror of Hyprland's factor selection in CInputManager::onMouseWheel:
// touchpad factor for finger scrolls (or whenever it is <= 0), else the mouse
// factor; a per-device factor overrides that; a window rule overrides both.
double KineticState::resolveScrollFactor() const {
    const float touchpadFactor  = *m_cfg.touchpadScrollFactor;
    const bool  isTouchpadScroll = touchpadFactor <= 0.f || m_g.source == WL_POINTER_AXIS_SOURCE_FINGER;
    double      factor           = isTouchpadScroll ? touchpadFactor : *m_cfg.mouseScrollFactor;

    if (const auto DEV = m_lastAxisDevice.lock(); DEV && DEV->m_scrollFactor.has_value())
        factor = *DEV->m_scrollFactor;

    if (const auto PWIN = m_g.window.lock()) {
        if (!isTouchpadScroll && PWIN->isScrollMouseOverridden())
            factor = PWIN->getScrollMouse();
        else if (isTouchpadScroll && PWIN->isScrollTouchpadOverridden())
            factor = PWIN->getScrollTouchpad();
    }
    return factor;
}

bool KineticState::launch(uint32_t liftMs, const char* how) {
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
    if (auto* lg = log())
        *lg << "[hypr-kinetic-scroll] launch(" << how << ") v=" << l.v << "/ms h=" << l.h << "/ms span=" << l.spanMs << "ms tail=" << l.tailMs << " rest=" << l.rest
            << " samples=" << l.samples << "\n";

    if (std::abs(m_velocityV) <= m_g.minVelPerMs && std::abs(m_velocityH) <= m_g.minVelPerMs) {
        stopKinetic(eStop::BELOW_THRESHOLD);
        return false;
    }

    m_g.scrollFactor    = resolveScrollFactor();
    m_state             = eState::DECAYING;
    m_lastTick          = std::chrono::steady_clock::now();
    m_lastStepFromTimer = false;

    if (Metrics::g_metrics.enabled()) {
        auto& g    = Metrics::g_metrics.gesture();
        g.tLaunch  = Metrics::nowNs();
        g.launched = true;
    }

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

// One momentum step. Called once per compositor frame of the target monitor
// (fromRender), or from the watchdog timer when no frame arrives.
void KineticState::step(bool fromRender) {
    Metrics::CScope stepScope(Metrics::g_metrics.enabled() ? &Metrics::g_metrics.gesture().stepNs : nullptr);

    if (inputCaptured()) {
        stepScope.retarget(nullptr);
        stopKinetic(eStop::CAPTURED);
        return;
    }

    switch (targetState()) {
        case eTarget::DESTROYED:
            // The client we owe a stop is gone; nothing to deliver, nothing to scroll.
            stepScope.retarget(nullptr);
            stopKinetic(eStop::TARGET_DESTROYED);
            return;
        case eTarget::CHANGED:
            if (m_g.stopOnTargetChange) {
                stepScope.retarget(nullptr);
                stopKinetic(eStop::TARGET_CHANGED);
                return;
            }
            break;
        case eTarget::SAME:
        case eTarget::NONE: break; // NONE cannot launch (beginGesture requires a surface)
    }

    const auto now = std::chrono::steady_clock::now();
    double     dt  = std::chrono::duration<double, std::milli>(now - m_lastTick).count();

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

    m_lastTick          = now;
    m_lastStepFromTimer = !fromRender;
    if (dt <= 0.0)
        dt = fromRender ? m_g.frameMs : m_g.intervalMs;

    // Decay over the real elapsed time; emit at most 100 ms worth of travel so
    // a stalled event loop doesn't make the content jump.
    double deltaV = 0.0, deltaH = 0.0;
    m_g.decay.step(m_velocityV, m_velocityH, dt, 100.0, deltaV, deltaH);

    emitSyntheticScroll(deltaV, deltaH, std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

    if (auto* l = log())
        *l << "[hypr-kinetic-scroll] step src=" << (fromRender ? "render" : "timer") << " dt=" << dt << " dV=" << deltaV << " v=" << m_velocityV << "\n";

    if (std::abs(m_velocityV) <= m_g.minVelPerMs && std::abs(m_velocityH) <= m_g.minVelPerMs) {
        stepScope.retarget(nullptr); // gesture is finalized inside stopKinetic
        stopKinetic(eStop::DECAY_DONE);
        return;
    }

    // After a real frame the watchdog only needs to catch a missing next frame.
    // If the watchdog itself fired, frames aren't coming: poll at interval_ms.
    const int watchdog = fromRender ? std::max<int>(m_g.intervalMs, static_cast<int>(std::ceil(2.5 * m_g.frameMs))) : m_g.intervalMs;
    wl_event_source_timer_update(m_timer, watchdog);
}

void KineticState::emitSyntheticScroll(double deltaV, double deltaH, uint32_t timeMs) {
    if (!g_pSeatManager || (deltaV == 0.0 && deltaH == 0.0))
        return;

    if (Metrics::g_metrics.enabled()) {
        auto& g = Metrics::g_metrics.gesture();
        ++g.emits;
        g.travelV += std::abs(deltaV);
        g.travelH += std::abs(deltaH);
        if (g.tFirstEmit == 0)
            g.tFirstEmit = Metrics::nowNs();
    }

    m_lastEmitSurface = g_pSeatManager->m_state.pointerFocus;

    // Same axis source as the finger phase, so the client sees one gesture.
    // Every axis we emit on is a sequence we must close.
    if (deltaV != 0.0) {
        m_owedStops |= 1;
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, deltaV * m_g.scrollFactor, 0, 0, m_g.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }
    if (deltaH != 0.0) {
        m_owedStops |= 2;
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, deltaH * m_g.scrollFactor, 0, 0, m_g.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    g_pSeatManager->sendPointerFrame();
}

// The axis_stop events the client never got, one per owed axis. They go to
// the surface that last received our synthetic scroll: the gesture's own
// surface, or a newer focus that momentum was allowed to follow. If pointer
// focus has since moved elsewhere the stop cannot be delivered through the
// seat; that client already received pointer.leave, which ends the gesture on
// its side.
void KineticState::sendOwedStops() {
    if (!m_owedStops || !g_pSeatManager)
        return;
    const auto& focus = g_pSeatManager->m_state.pointerFocus;
    if (!focus) {
        m_owedStops = 0;
        return;
    }
    const bool isGestureSurface = m_g.hadSurface && !m_g.surface.expired() && focus == m_g.surface;
    const bool isEmitSurface    = !m_lastEmitSurface.expired() && focus == m_lastEmitSurface;
    if (!isGestureSurface && !isEmitSurface) {
        m_owedStops = 0; // undeliverable: that client got pointer.leave
        return;
    }

    const auto     now    = std::chrono::steady_clock::now();
    const uint32_t timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // sendPointerAxis with value 0 and a non-wheel source emits axis + axis_stop.
    if (m_owedStops & 1)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, 0.0, 0, 0, m_g.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (m_owedStops & 2)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0.0, 0, 0, m_g.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    g_pSeatManager->sendPointerFrame();

    if (Metrics::g_metrics.enabled())
        Metrics::g_metrics.gesture().stopsSent += (m_owedStops & 1 ? 1 : 0) + (m_owedStops & 2 ? 1 : 0);
    m_owedStops = 0;
    m_lastEmitSurface.reset();
}

// ---------------------------------------------------------------------------
// End of gesture

void KineticState::stopKinetic(eStop reason) {
    if (m_state == eState::IDLE)
        return;

    // Close the gesture for the client if we swallowed its finger lift, unless
    // the client's gesture simply continues with the real events that caused
    // this stop (fingers back down and moving: a re-touch, as on macOS). In
    // that case the debt carries over and is settled when that gesture ends.
    const bool carryOver = clientGestureContinues(reason);
    if (!carryOver)
        sendOwedStops();

    if (auto* l = log())
        *l << "[hypr-kinetic-scroll] stop reason=" << stopName(reason) << " owedLeft=" << int(m_owedStops) << "\n";

    if (Metrics::g_metrics.enabled()) {
        auto& g      = Metrics::g_metrics.gesture();
        g.stopsOwed  = (m_owedStops & 1 ? 1 : 0) + (m_owedStops & 2 ? 1 : 0); // carried over (hand-off) or undeliverable
        g.stopReason = stopName(reason);
    }
    Metrics::g_metrics.gestureEnd(Metrics::nowNs());
    resetGesture(carryOver);
}
