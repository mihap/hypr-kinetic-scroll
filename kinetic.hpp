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
// IGNORED is TRACKING's twin for gestures over windows where the plugin is
// disabled: nothing is measured or emitted until that gesture ends.
//
// While DECAYING the plugin *owns* the gesture: the client never sees the real
// finger lift, receives one synthetic scroll per compositor frame with the
// same axis source as the finger phase, and gets an axis_stop from us when
// momentum ends. Which axes are owed a stop is tracked separately from which
// axes still have velocity.
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
    RULE_DISABLED,
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
    void onPointerFrame();
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
        CConfigValue<Config::INTEGER> liftTailCapMs{"plugin:kinetic-scroll:lift_tail_cap_ms"};
        CConfigValue<Config::INTEGER> disableInBrowser{"plugin:kinetic-scroll:disable_in_browser"};
        CConfigValue<Config::INTEGER> stopOnTargetChange{"plugin:kinetic-scroll:stop_on_target_change"};
        CConfigValue<Config::STRING>  disabledClasses{"plugin:kinetic-scroll:disabled_classes"};
        CConfigValue<Config::INTEGER> debug{"plugin:kinetic-scroll:debug"};
        CConfigValue<Config::INTEGER> stopOnClick{"plugin:kinetic-scroll:stop_on_click"};
        CConfigValue<Config::INTEGER> stopOnFocus{"plugin:kinetic-scroll:stop_on_focus"};
        CConfigValue<Config::STRING>  metricsFile{"plugin:kinetic-scroll:metrics_file"};
        CConfigValue<Config::FLOAT>   touchpadScrollFactor{"input:touchpad:scroll_factor"};
    };
    const SConfig& config() const {
        return m_cfg;
    }

  private:
    enum class eState : uint8_t {
        IDLE,
        IGNORED,  // a gesture is in progress over a window we leave alone
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
        double                 scrollFactor       = 1.0;
        wl_pointer_axis_source source             = WL_POINTER_AXIS_SOURCE_FINGER;
        bool                   touchpad           = true; // false: smooth mouse, no stop events
        bool                   stopOnTargetChange = true;
        bool                   debug              = false;
        Physics::SDecay        decay;
        double                 minVelPerMs      = 0.0;
        double                 launchMultiplier = 1.0;
        uint32_t               windowMs         = 64;
        uint32_t               tailCapMs        = 0; // 0 = auto
        int                    intervalMs       = 16;
    };

    static int onTimer(void* data);

    bool          beginGesture(const IPointer::SAxisEvent& e, bool touchpadSource);
    eTarget       targetState() const;
    bool          launch(uint32_t liftMs, const char* how);
    void          step(bool fromRender);
    void          emitSyntheticScroll(double deltaV, double deltaH, uint32_t timeMs);
    void          sendOwedStops();
    void          resetGesture();
    bool          touchpadsStale() const;
    void          rescanTouchpads();
    bool          windowAllowed(const std::string& cls) const;
    std::ostream* log();

    SConfig                    m_cfg;
    eState                     m_state = eState::IDLE;
    SGestureCtx                m_g;
    Physics::CVelocityEstimator m_est;

    bool                       m_axisInFrame   = false;
    bool                       m_timerLaunches = false; // TRACKING timer: launch (true) or abandon (false)

    // Momentum. Velocities in scroll units per ms.
    double                                m_velocityV = 0.0;
    double                                m_velocityH = 0.0;
    std::chrono::steady_clock::time_point m_lastTick;
    bool                                  m_lastStepFromTimer = false;

    // Protocol ownership: bit 0 = vertical, bit 1 = horizontal axis whose real
    // axis_stop we swallowed and therefore owe the client. The stops go to the
    // surface that last received our synthetic scroll (normally the gesture's
    // surface; with stop_on_target_change = 0 it may be a newer focus).
    uint8_t                m_owedStops = 0;
    WP<CWLSurfaceResource> m_lastEmitSurface;

    wl_event_source*                                    m_timer = nullptr;
    std::vector<WP<IPointer>>                           m_touchpads;
    std::vector<Hyprutils::Signal::CHyprSignalListener> m_touchpadListeners;

    std::unordered_map<std::string, bool> m_perAppRules;
    bool                                  m_defaultAppRule = true;

    std::ofstream m_log;
};
