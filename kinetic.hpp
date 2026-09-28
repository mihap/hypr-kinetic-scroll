#pragma once
#include "gesture.hpp"
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/macros.hpp>
#include <wayland-server-core.h>
#include <fstream>
#include <string>
#include <vector>

// The Hyprland side of hypr-kinetic-scroll: Gesture::IHost for the state
// machine in gesture.hpp. Owns everything compositor-specific: the event-loop
// timer, the pointer devices we listen on, the gesture's target references
// (window, surface, monitor), the seat calls, config handles, the debug log.
// No decisions are made here; see gesture.cpp for those.

class KineticState : private Gesture::IHost {
  public:
    KineticState();
    ~KineticState() override;

    // Returns true if the event must be cancelled (not delivered to the client).
    bool onAxis(const IPointer::SAxisEvent& e);
    void onTouchpadContact();
    void onRenderPre(PHLMONITOR mon);
    void stopKinetic(Gesture::eStop reason);

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
    // Gesture::IHost
    bool             enabled() const override;
    bool             inputCaptured() const override;
    Gesture::SParams params() const override;
    Gesture::STarget captureTarget() override;
    Gesture::eTarget targetState() const override;
    void             releaseTarget(bool keepEmitSurface) override;
    double           scrollFactor(Gesture::eSource source) const override;
    void             emitScroll(double v, double h, uint32_t timeMs, Gesture::eSource source) override;
    bool             deliverStops(uint8_t axes, Gesture::eSource source) override;
    void             armTimer(int ms) override;
    void             scheduleFrame() override;
    double           nowMs() const override;
    void             log(std::string_view line) override;

    static int onTimer(void* data);
    bool       devicesStale() const;
    void       rescanDevices();

    // The current gesture's target, captured at its start. Weak references:
    // expiry means the target was destroyed.
    struct STargetRefs {
        PHLWINDOWREF           window;
        WP<CWLSurfaceResource> surface;
        bool                   hadWindow  = false;
        bool                   hadSurface = false;
        PHLMONITORREF          monitor;
    };

    SConfig                m_cfg;
    STargetRefs            m_target;
    WP<CWLSurfaceResource> m_lastEmitSurface; // received the last synthetic scroll

    wl_event_source*                                    m_timer = nullptr;
    std::vector<WP<IPointer>>                           m_devices; // every pointer device we listen on
    std::vector<Hyprutils::Signal::CHyprSignalListener> m_deviceListeners;
    WP<IPointer>                                        m_lastAxisDevice; // device of the most recent axis event

    std::ofstream m_log;

    // Last: it holds a reference to *this and calls back into it.
    Gesture::CMachine m_machine;
};
