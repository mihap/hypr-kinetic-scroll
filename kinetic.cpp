#include "kinetic.hpp"
#include "globals.hpp"
#include "metrics.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/InputCapture.hpp>
#include <algorithm>
#include <chrono>

using Gesture::eSource;
using Gesture::eTarget;

static eSource toSource(wl_pointer_axis_source s) {
    switch (s) {
        case WL_POINTER_AXIS_SOURCE_WHEEL: return eSource::WHEEL;
        case WL_POINTER_AXIS_SOURCE_FINGER: return eSource::FINGER;
        case WL_POINTER_AXIS_SOURCE_CONTINUOUS: return eSource::CONTINUOUS;
        case WL_POINTER_AXIS_SOURCE_WHEEL_TILT: return eSource::WHEEL_TILT;
    }
    return eSource::WHEEL;
}

static wl_pointer_axis_source fromSource(eSource s) {
    switch (s) {
        case eSource::WHEEL: return WL_POINTER_AXIS_SOURCE_WHEEL;
        case eSource::FINGER: return WL_POINTER_AXIS_SOURCE_FINGER;
        case eSource::CONTINUOUS: return WL_POINTER_AXIS_SOURCE_CONTINUOUS;
        case eSource::WHEEL_TILT: return WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
    }
    return WL_POINTER_AXIS_SOURCE_FINGER;
}

static uint32_t steadyMs() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// Construction, timer, devices

KineticState::KineticState() : m_machine(*this) {
    m_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTimer, this);
    Metrics::g_metrics.setLoop(g_pCompositor->m_wlEventLoop);
    rescanDevices();
}

KineticState::~KineticState() {
    // Unloading mid-momentum must not leave the client waiting for the lift we
    // swallowed: close the gesture first, while the seat is still there.
    m_machine.stop(Gesture::eStop::UNLOAD);
    Metrics::g_metrics.flushNow();
    if (m_timer)
        wl_event_source_remove(m_timer);
}

int KineticState::onTimer(void* data) {
    static_cast<KineticState*>(data)->m_machine.onTimer();
    return 0;
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

// ---------------------------------------------------------------------------
// Events in

bool KineticState::onAxis(const IPointer::SAxisEvent& e) {
    Gesture::SAxisEvent ev;
    ev.timeMs        = e.timeMs;
    ev.vertical      = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL;
    ev.delta         = e.delta;
    ev.deltaDiscrete = e.deltaDiscrete;
    ev.source        = toSource(e.source);
    ev.mouse         = e.mouse;
    return m_machine.onAxis(ev);
}

void KineticState::onTouchpadContact() {
    m_machine.onTouchpadContact();
}

void KineticState::onRenderPre(PHLMONITOR mon) {
    if (!m_machine.decaying() || !mon) {
        if (Metrics::g_metrics.enabled())
            ++Metrics::g_metrics.global().idleRender;
        return;
    }
    if (!m_target.monitor.expired() && m_target.monitor != mon)
        return;
    m_machine.onFrame();
}

void KineticState::stopKinetic(Gesture::eStop reason) {
    m_machine.stop(reason);
}

void KineticState::setAppRule(const std::string& appClass, bool enabled) {
    m_machine.policy().setAppRule(appClass, enabled);
}

void KineticState::setDefaultAppRule(bool enabled) {
    m_machine.policy().setDefaultAppRule(enabled);
}

void KineticState::resetAppRules() {
    m_machine.policy().reset();
}

// ---------------------------------------------------------------------------
// Gesture::IHost

bool KineticState::enabled() const {
    return *m_cfg.enabled != 0;
}

bool KineticState::inputCaptured() const {
    return PROTO::inputCapture && PROTO::inputCapture->isCaptured();
}

Gesture::SParams KineticState::params() const {
    Gesture::SParams p;
    p.decel              = *m_cfg.decel;
    p.minVelocity        = *m_cfg.minVelocity;
    p.deltaMultiplier    = *m_cfg.deltaMultiplier;
    p.velocityWindowMs   = *m_cfg.velocityWindowMs;
    p.liftTailGraceMs    = *m_cfg.liftTailGraceMs;
    p.liftTailFadeMs     = *m_cfg.liftTailFadeMs;
    p.intervalMs         = *m_cfg.intervalMs;
    p.stopOnTargetChange = *m_cfg.stopOnTargetChange != 0;
    p.disableInBrowser   = *m_cfg.disableInBrowser != 0;
    p.disabledClasses    = *m_cfg.disabledClasses;
    p.debug              = *m_cfg.debug != 0;
    p.metricsFile        = *m_cfg.metricsFile;
    return p;
}

Gesture::STarget KineticState::captureTarget() {
    if (devicesStale())
        rescanDevices();

    m_target = {};
    Gesture::STarget t;

    const auto PWIN  = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
    const auto PSURF = g_pSeatManager ? g_pSeatManager->m_state.pointerFocus.lock() : nullptr;

    if (PSURF) {
        m_target.surface    = PSURF;
        m_target.hadSurface = true;
        t.hasSurface        = true;
    }
    if (PWIN) {
        m_target.window    = PWIN;
        m_target.hadWindow = true;
        m_target.monitor   = PWIN->m_monitor;
        t.hasWindow        = true;
        t.windowClass      = PWIN->m_class;
        if (const auto MON = PWIN->m_monitor.lock(); MON && MON->m_refreshRate > 1.0f)
            t.frameMs = 1000.0 / MON->m_refreshRate;
    }
    return t;
}

eTarget KineticState::targetState() const {
    if (!m_target.hadWindow && !m_target.hadSurface)
        return eTarget::NONE;

    if (m_target.hadWindow) {
        if (m_target.window.expired())
            return eTarget::DESTROYED;
        if (g_pInputManager && g_pInputManager->m_lastMouseFocus != m_target.window)
            return eTarget::CHANGED;
    }
    if (m_target.hadSurface) {
        if (m_target.surface.expired())
            return eTarget::DESTROYED;
        if (g_pSeatManager && g_pSeatManager->m_state.pointerFocus != m_target.surface)
            return eTarget::CHANGED;
    }
    return eTarget::SAME;
}

void KineticState::releaseTarget(bool keepEmitSurface) {
    m_target = {};
    if (!keepEmitSurface)
        m_lastEmitSurface.reset();
}

// Mirror of Hyprland's factor selection in CInputManager::onMouseWheel:
// touchpad factor for finger scrolls (or whenever it is <= 0), else the mouse
// factor; a per-device factor overrides that; a window rule overrides both.
double KineticState::scrollFactor(eSource source) const {
    const float touchpadFactor   = *m_cfg.touchpadScrollFactor;
    const bool  isTouchpadScroll = touchpadFactor <= 0.f || source == eSource::FINGER;
    double      factor           = isTouchpadScroll ? touchpadFactor : *m_cfg.mouseScrollFactor;

    if (const auto DEV = m_lastAxisDevice.lock(); DEV && DEV->m_scrollFactor.has_value())
        factor = *DEV->m_scrollFactor;

    if (const auto PWIN = m_target.window.lock()) {
        if (!isTouchpadScroll && PWIN->isScrollMouseOverridden())
            factor = PWIN->getScrollMouse();
        else if (isTouchpadScroll && PWIN->isScrollTouchpadOverridden())
            factor = PWIN->getScrollTouchpad();
    }
    return factor;
}

void KineticState::emitScroll(double v, double h, uint32_t timeMs, eSource source) {
    if (!g_pSeatManager)
        return;

    m_lastEmitSurface = g_pSeatManager->m_state.pointerFocus;

    const auto SRC = fromSource(source);
    if (v != 0.0)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, v, 0, 0, SRC, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (h != 0.0)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, h, 0, 0, SRC, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);

    g_pSeatManager->sendPointerFrame();
}

// The owed axis_stop events go to the surface that last received our
// synthetic scroll: the gesture's own surface, or a newer focus that momentum
// was allowed to follow. If pointer focus has since moved elsewhere the stop
// cannot be delivered through the seat; that client already received
// pointer.leave, which ends the gesture on its side.
bool KineticState::deliverStops(uint8_t axes, eSource source) {
    if (!axes || !g_pSeatManager)
        return false;

    const auto& focus = g_pSeatManager->m_state.pointerFocus;
    if (!focus) {
        m_lastEmitSurface.reset();
        return false;
    }
    const bool isGestureSurface = m_target.hadSurface && !m_target.surface.expired() && focus == m_target.surface;
    const bool isEmitSurface    = !m_lastEmitSurface.expired() && focus == m_lastEmitSurface;
    if (!isGestureSurface && !isEmitSurface) {
        m_lastEmitSurface.reset();
        return false;
    }

    const uint32_t timeMs = steadyMs();
    const auto     SRC    = fromSource(source);

    // sendPointerAxis with value 0 and a non-wheel source emits axis + axis_stop.
    if (axes & Gesture::AXIS_V)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, 0.0, 0, 0, SRC, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (axes & Gesture::AXIS_H)
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0.0, 0, 0, SRC, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    g_pSeatManager->sendPointerFrame();

    m_lastEmitSurface.reset();
    return true;
}

void KineticState::armTimer(int ms) {
    wl_event_source_timer_update(m_timer, ms);
}

void KineticState::scheduleFrame() {
    if (const auto MON = m_target.monitor.lock())
        MON->scheduleFrame();
}

double KineticState::nowMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void KineticState::log(std::string_view line) {
    if (!m_log.is_open())
        m_log.open("/tmp/hypr-kinetic-scroll.log", std::ios::app);
    if (m_log.is_open())
        m_log << line << '\n';
}
