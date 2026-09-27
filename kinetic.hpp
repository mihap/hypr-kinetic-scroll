#pragma once
#include "physics.hpp"
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/macros.hpp>
#include <wayland-server-core.h>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

// Momentum ("kinetic") scrolling for touchpads, implemented in the compositor.
//
// Lifecycle of one gesture:
//   IDLE ──motion──▶ TRACKING ──finger lift──▶ DECAYING ──velocity floor──▶ IDLE
//                       │                          │
//                       └── pause/idle ──▶ IDLE    └── touch / new swipe / target change ──▶ IDLE
//
// IGNORED is TRACKING's twin for gestures the plugin leaves alone (window
// rule, no pointer focus): nothing is measured or emitted until they end.
//
// While DECAYING the plugin *owns* the gesture: the client never sees the real
// finger lift, receives one synthetic scroll per compositor frame with the
// same axis source as the finger phase, and gets an axis_stop from us when
// momentum ends. The stops we owe (per axis: every axis whose real stop we
// swallowed or on which we emitted) are tracked separately from velocity and
// survive a re-touch hand-off, so every opened axis sequence gets closed.
//
// Physics (velocity estimate, decay integral) lives in physics.hpp; this class
// is the adapter to Hyprland: events, focus, timers, frames, config.

// Why a gesture ended. Typed so that "does the client's gesture continue with
// real events?" is a property of the reason, not a string comparison.
enum class eStop : uint8_t {
    BELOW_THRESHOLD,  // lift with ~no velocity; the real stop passed through
    DECAY_DONE,       // momentum ran out
    NEW_GESTURE,      // fingers back and moving: the client's gesture continues
    TOUCHPAD_CONTACT, // fingers back (hold / motion): momentum cancelled
    GESTURE_IDLE,     // fingers rested on the pad; nothing to launch
    TARGET_CHANGED,   // pointer focus moved to another window/surface
    TARGET_DESTROYED, // the target window/surface went away
    MOUSE_BUTTON,
    ACTIVE_WINDOW,
    RULE_DISABLED,    // plugin disabled while a gesture was live
    CAPTURED,         // input capture took the seat's input
    UNLOAD,
};
const char* stopName(eStop s);
constexpr bool clientGestureContinues(eStop s) {
    return s == eStop::NEW_GESTURE;
}

class KineticState {
  public:
    KineticState();
    ~KineticState();

    // Returns true if the event must be cancelled (not delivered to the client).
    bool onAxis(IPointer::SAxisEvent& e);
    void onTouchpadContact();
    void onRenderPre(PHLMONITOR mon);
    void stopKinetic(eStop reason);

    void setAppRule(const std::string& appClass, bool enabled);
    void setDefaultAppRule(bool enabled);
    void resetAppRules();

    // Config handles, bound once at construction. Reading one is a pointer
    // dereference; Hyprland re-binds them itself on config reload.
    struct SConfig {
        CConfigValue<Config::INTEGER> enabled{"plugin:kinetic-scroll:enabled"};
        CConfigValue<Config::FLOAT>   decel{"plugin:kinetic-scroll:decel"};
        CConfigValue<Config::FLOAT>   minVelocity{"plugin:kinetic-scroll:min_velocity"};
        CConfigValue<Config::INTEGER> intervalMs{"plugin:kinetic-scroll:interval_ms"};
        CConfigValue<Config::FLOAT>   deltaMultiplier{"plugin:kinetic-scroll:delta_multiplier"};
        CConfigValue<Config::INTEGER> velocityWindowMs{"plugin:kinetic-scroll:velocity_window_ms"};
        CConfigValue<Config::INTEGER> liftTailGraceMs{"plugin:kinetic-scroll:lift_tail_grace_ms"};
        CConfigValue<Config::INTEGER> liftTailFadeMs{"plugin:kinetic-scroll:lift_tail_fade_ms"};
        CConfigValue<Config::INTEGER> disableInBrowser{"plugin:kinetic-scroll:disable_in_browser"};
        CConfigValue<Config::INTEGER> stopOnTargetChange{"plugin:kinetic-scroll:stop_on_target_change"};
        CConfigValue<Config::STRING>  disabledClasses{"plugin:kinetic-scroll:disabled_classes"};
        CConfigValue<Config::INTEGER> debug{"plugin:kinetic-scroll:debug"};
        CConfigValue<Config::INTEGER> stopOnClick{"plugin:kinetic-scroll:stop_on_click"};
        CConfigValue<Config::INTEGER> stopOnFocus{"plugin:kinetic-scroll:stop_on_focus"};
        CConfigValue<Config::STRING>  metricsFile{"plugin:kinetic-scroll:metrics_file"};
        CConfigValue<Config::FLOAT>   touchpadScrollFactor{"input:touchpad:scroll_factor"};
        CConfigValue<Config::FLOAT>   mouseScrollFactor{"input:scroll_factor"};
    };
    const SConfig& config() const {
        return m_cfg;
    }

  private:
    enum class eState : uint8_t {
        IDLE,
        IGNORED,  // a gesture is in progress that we leave alone
        TRACKING, // fingers down, collecting samples
        DECAYING, // momentum running
    };

    enum class eTarget : uint8_t { SAME, CHANGED, DESTROYED, NONE };

    // Everything about the current gesture that is fixed at its start.
    struct SGestureCtx {
        PHLWINDOWREF           window;
        WP<CWLSurfaceResource> surface;
        bool                   hadWindow  = false;
        bool                   hadSurface = false;
        PHLMONITORREF          monitor;
        double                 frameMs            = 1000.0 / 60.0;
        double                 scrollFactor       = 1.0; // resolved at launch (device known by then)
        wl_pointer_axis_source source             = WL_POINTER_AXIS_SOURCE_FINGER;
        bool                   touchpad           = true; // false: smooth mouse, no stop events
        bool                   stopOnTargetChange = true;
        bool                   debug              = false;
        Physics::SDecay        decay;
        double                 minVelPerMs      = 0.0;
        double                 launchMultiplier = 1.0;
        uint32_t               windowMs         = 32;
        uint32_t               tailGraceMs      = 28;
        uint32_t               tailFadeMs       = 80;
        int                    intervalMs       = 16;
    };

    static int onTimer(void* data);

    bool          beginGesture(const IPointer::SAxisEvent& e, bool touchpadSource);
    eTarget       targetState() const;
    bool          launch(uint32_t liftMs, const char* how);
    double        resolveScrollFactor() const;
    void          step(bool fromRender);
    void          emitSyntheticScroll(double deltaV, double deltaH, uint32_t timeMs);
    void          sendOwedStops();
    void          resetGesture(bool keepOwedStops);
    bool          devicesStale() const;
    void          rescanDevices();
    bool          windowAllowed(const std::string& cls) const;
    std::ostream* log();

    SConfig                     m_cfg;
    eState                      m_state = eState::IDLE;
    SGestureCtx                 m_g;
    Physics::CVelocityEstimator m_est;

    bool m_timerLaunches = false; // TRACKING timer: launch (true) or abandon (false)

    // Momentum. Velocities in scroll units per ms.
    double                                m_velocityV = 0.0;
    double                                m_velocityH = 0.0;
    std::chrono::steady_clock::time_point m_lastTick;
    bool                                  m_lastStepFromTimer = false;

    // Protocol ownership: bit 0 = vertical, bit 1 = horizontal axis on which
    // the client has an open scroll sequence that we must close: its real
    // axis_stop was swallowed, or we emitted on it. Delivered to the surface
    // that last received our synthetic scroll (normally the gesture's surface).
    uint8_t                m_owedStops = 0;
    WP<CWLSurfaceResource> m_lastEmitSurface;

    wl_event_source*                                    m_timer = nullptr;
    std::vector<WP<IPointer>>                           m_devices; // every pointer device we listen on
    std::vector<Hyprutils::Signal::CHyprSignalListener> m_deviceListeners;
    WP<IPointer>                                        m_lastAxisDevice; // device of the most recent axis event

    std::unordered_map<std::string, bool> m_perAppRules;
    bool                                  m_defaultAppRule = true;

    std::ofstream m_log;
};
