#pragma once
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/macros.hpp>
#include <wayland-server-core.h>
#include <array>
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
// momentum ends.
class KineticState {
  public:
    KineticState();
    ~KineticState();

    // Returns true if the event must be cancelled (not delivered to the client).
    bool onAxis(IPointer::SAxisEvent& e);
    void onPointerFrame();
    void onTouchpadContact();
    void onRenderPre(PHLMONITOR mon);
    void stopKinetic(const char* reason);

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

    struct SSample {
        uint32_t t  = 0; // libinput timestamp (ms)
        uint32_t dt = 0; // ms since previous sample of this gesture, 0 if unknown
        double   dv = 0.0;
        double   dh = 0.0;
    };

    // Everything about the current gesture that is fixed at its start.
    struct SGestureCtx {
        PHLWINDOWREF           window;
        WP<CWLSurfaceResource> surface;
        PHLMONITORREF          monitor;
        double                 frameMs            = 1000.0 / 60.0;
        double                 scrollFactor       = 1.0;
        wl_pointer_axis_source source             = WL_POINTER_AXIS_SOURCE_FINGER;
        bool                   touchpad           = true; // false: smooth mouse, no stop events
        bool                   stopOnTargetChange = true;
        bool                   debug              = false;
        double                 lambda             = 0.0; // decay rate per ms
        double                 minVelPerMs        = 0.0;
        double                 launchMultiplier   = 1.0;
        uint32_t               windowMs           = 64;
        int                    tailCapMs          = 0; // 0 = auto (from the pad's report interval)
        int                    intervalMs         = 16;
    };

    static int onTimer(void* data);

    bool       beginGesture(const IPointer::SAxisEvent& e, bool touchpadSource);
    void       recordSample(const IPointer::SAxisEvent& e);
    void       computeLaunchVelocity(uint32_t liftMs);
    bool       launch(const char* reason);
    void       step(bool fromRender);
    bool       targetStillValid();
    void       emitSyntheticScroll(double deltaV, double deltaH, uint32_t timeMs);
    void       sendGestureStop();
    void       rescanTouchpads();
    void       resetGesture();
    bool       windowAllowed(const std::string& cls) const;
    std::ostream* log();

    SConfig                               m_cfg;
    eState                                m_state = eState::IDLE;
    SGestureCtx                           m_g;

    // Sample ring for velocity at lift.
    static constexpr size_t               SAMPLE_CAP = 64;
    std::array<SSample, SAMPLE_CAP>       m_samples{};
    size_t                                m_sampleHead    = 0; // next write slot
    size_t                                m_sampleCount   = 0;
    uint32_t                              m_lastEventMs   = 0;
    bool                                  m_axisInFrame   = false;
    bool                                  m_timerLaunches = false; // TRACKING timer: launch (true) or abandon (false)

    // Momentum. Velocities in scroll units per ms.
    double                                m_velocityV       = 0.0;
    double                                m_velocityH       = 0.0;
    bool                                  m_activeV         = false;
    bool                                  m_activeH         = false;
    bool                                  m_clientInGesture = false;
    std::chrono::steady_clock::time_point m_lastTick;

    wl_event_source*                      m_timer = nullptr;

    std::vector<Hyprutils::Signal::CHyprSignalListener> m_touchpadListeners;
    size_t                                              m_pointersSeen = 0;

    std::unordered_map<std::string, bool> m_perAppRules;
    bool                                  m_defaultAppRule = true;

    std::ofstream                         m_log;
};
