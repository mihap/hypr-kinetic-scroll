#pragma once
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/macros.hpp>
#include <wayland-server-core.h>
#include <chrono>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <string>

class KineticState {
  public:
    KineticState();
    ~KineticState();

    void onAxis(IPointer::SAxisEvent& e);
    void onPointerFrame();
    void onTouchpadContact();
    void onRenderPre(PHLMONITOR mon);
    void stopKinetic(const char* reason = nullptr);
    void setAppRule(const std::string& appClass, bool enabled);
    void setDefaultAppRule(bool enabled);
    void resetAppRules();

  private:
    struct SSample {
        uint32_t t  = 0; // libinput timestamp (ms) of the event
        uint32_t dt = 0; // ms since previous event in this gesture, 0 if unknown
        double   dv = 0.0;
        double   dh = 0.0;
    };

    static int onStopTimer(void* data);
    static int onDecayTimer(void* data);
    void       computeLaunchVelocity(uint32_t liftMs);
    void       beginDecay(const char* reason);
    void       step(bool fromRender);
    bool       targetStillValid();
    void       emitSyntheticScroll(double deltaV, double deltaH);
    bool       hasAppRule(const std::string& windowClass) const;
    bool       shouldProcessForWindow(const std::string& windowClass) const;

    // While tracking: unused. While decaying: velocity in scroll units per ms.
    double    m_velocityV             = 0.0;
    double    m_velocityH             = 0.0;
    uint32_t  m_lastEventMs           = 0;
    std::deque<SSample>                   m_samples;
    std::chrono::steady_clock::time_point m_lastTick;
    MONITORID                             m_targetMonitorId = MONITOR_INVALID;
    double                                m_frameMs         = 1000.0 / 60.0;
    double                                m_scrollFactor    = 1.0;
    bool      m_tracking              = false;
    bool      m_decaying              = false;
    bool      m_axisEventInFrame      = false;
    bool      m_cancelOnStopTimer     = false;
    uintptr_t m_scrollTargetWindowKey = 0;
    uintptr_t m_scrollTargetSurfaceKey = 0;

    wl_event_source* m_stopTimer  = nullptr;
    wl_event_source* m_decayTimer = nullptr;

    std::unordered_map<std::string, bool> m_perAppRules;
    bool                                  m_defaultAppRule = true;
};
