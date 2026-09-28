#pragma once
// The gesture state machine of hypr-kinetic-scroll, free of the compositor.
//
// Everything that decides *what happens* lives here: which axis events are
// ours, when momentum launches, what is emitted per frame, which axis_stop
// events the client is owed and when they are settled, how a gesture ends.
// Everything that touches Hyprland (pointer focus, surfaces, monitors, the
// seat, the event loop, config storage) is behind IHost, so the same machine
// runs under the real compositor (kinetic.cpp) and under a scripted host in
// tests (tests/gesture_test.cpp).
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
// Physics (velocity estimate, decay integral) lives in physics.hpp.

#include "physics.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Gesture {

    // Why a gesture ended. Typed so that "does the client's gesture continue
    // with real events?" is a property of the reason, not a string comparison.
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
    const char*    stopName(eStop s);
    constexpr bool clientGestureContinues(eStop s) {
        return s == eStop::NEW_GESTURE;
    }

    // wl_pointer axis source, without the Wayland header.
    enum class eSource : uint8_t { WHEEL, FINGER, CONTINUOUS, WHEEL_TILT };

    // What became of the gesture's target since it was captured.
    enum class eTarget : uint8_t { SAME, CHANGED, DESTROYED, NONE };

    // Axis bits: owed stops, IHost::deliverStops.
    constexpr uint8_t AXIS_V = 1;
    constexpr uint8_t AXIS_H = 2;

    struct SAxisEvent {
        uint32_t timeMs        = 0;
        bool     vertical      = true;
        double   delta         = 0.0; // delta == 0 && deltaDiscrete == 0 is an axis_stop
        int32_t  deltaDiscrete = 0;
        eSource  source        = eSource::WHEEL;
        bool     mouse         = false; // reported by a mouse device
    };

    // Configuration, read from the host once per gesture. Raw values: the
    // machine sanitizes them.
    struct SParams {
        double      decel              = 0.967; // velocity multiplier per 16 ms
        double      minVelocity        = 0.5;   // scroll units per 16 ms
        double      deltaMultiplier    = 1.0;
        int64_t     velocityWindowMs   = 32;
        int64_t     liftTailGraceMs    = 28;
        int64_t     liftTailFadeMs     = 80;
        int64_t     intervalMs         = 16;
        bool        stopOnTargetChange = true;
        bool        disableInBrowser   = true;
        std::string disabledClasses;
        bool        debug = false;
        std::string metricsFile;
    };

    // The pointer focus at gesture start, as far as the machine needs it.
    struct STarget {
        bool        hasSurface = false;
        bool        hasWindow  = false;
        std::string windowClass;                // empty without a window
        double      frameMs = 1000.0 / 60.0;    // of the monitor showing the window
    };

    // Everything the machine needs from the compositor.
    class IHost {
      public:
        virtual ~IHost() = default;

        virtual bool    enabled() const       = 0;
        virtual bool    inputCaptured() const = 0;
        virtual SParams params() const        = 0;

        // Snapshot the current pointer focus as the target of a new gesture.
        // The host keeps the references for targetState() and deliverStops().
        virtual STarget captureTarget()       = 0;
        virtual eTarget targetState() const   = 0;
        // Drop the target references. The surface that last received a
        // synthetic scroll is kept while owed stops carry over to the next
        // gesture, so they can still be delivered there.
        virtual void releaseTarget(bool keepEmitSurface) = 0;

        // Scroll factor for the gesture's source, resolved at launch.
        virtual double scrollFactor(eSource source) const = 0;

        // One synthetic scroll, already scaled, at least one axis non-zero,
        // followed by a pointer frame. The host remembers the receiving surface.
        virtual void emitScroll(double v, double h, uint32_t timeMs, eSource source) = 0;

        // The axis_stop events owed on `axes`, to the gesture's surface or the
        // surface that last received a synthetic scroll. False when neither
        // has pointer focus any more: that client already got pointer.leave.
        virtual bool deliverStops(uint8_t axes, eSource source) = 0;

        virtual void   armTimer(int ms)            = 0; // 0 disarms
        virtual void   scheduleFrame()             = 0; // on the target monitor
        virtual double nowMs() const               = 0; // monotonic
        virtual void   log(std::string_view line)  = 0; // debug only
    };

    // Per-app policy: explicit rules, the disabled-class list, the browser
    // heuristic, and the default. Evaluated once per gesture.
    class CPolicy {
      public:
        void setAppRule(const std::string& cls, bool enabled);
        void setDefaultAppRule(bool enabled);
        void reset();
        bool allows(const std::string& cls, const std::string& disabledClasses, bool disableInBrowser) const;

      private:
        std::unordered_map<std::string, bool> m_perApp;
        bool                                  m_default = true;
    };

    class CMachine {
      public:
        enum class eState : uint8_t {
            IDLE,
            IGNORED,  // a gesture is in progress that we leave alone
            TRACKING, // fingers down, collecting samples
            DECAYING, // momentum running
        };

        explicit CMachine(IHost& host);
        ~CMachine(); // a live gesture is closed with eStop::UNLOAD

        // True if the event must be swallowed (not delivered to the client).
        bool onAxis(const SAxisEvent& e);
        void onTouchpadContact();
        void onFrame(); // a frame of the target monitor is about to render
        void onTimer();
        void stop(eStop reason);

        CPolicy& policy() {
            return m_policy;
        }
        eState state() const {
            return m_state;
        }
        bool idle() const {
            return m_state == eState::IDLE;
        }
        bool decaying() const {
            return m_state == eState::DECAYING;
        }
        uint8_t owedStops() const {
            return m_owedStops;
        }
        double velocityV() const {
            return m_velocityV;
        }
        double velocityH() const {
            return m_velocityH;
        }

      private:
        // Everything about the current gesture that is fixed at its start.
        struct SGesture {
            double          frameMs            = 1000.0 / 60.0;
            double          scrollFactor       = 1.0; // resolved at launch (device known by then)
            eSource         source             = eSource::FINGER;
            bool            touchpad           = true; // false: smooth mouse, no stop events
            bool            stopOnTargetChange = true;
            bool            debug              = false;
            Physics::SDecay decay;
            double          minVelPerMs      = 0.0;
            double          launchMultiplier = 1.0;
            uint32_t        windowMs         = 32;
            uint32_t        tailGraceMs      = 28;
            uint32_t        tailFadeMs       = 80;
            int             intervalMs       = 16;
        };

        bool beginGesture(const SAxisEvent& e, bool touchpadSource);
        bool launch(uint32_t liftMs, const char* how);
        void step(bool fromRender);
        void emitSyntheticScroll(double deltaV, double deltaH, uint32_t timeMs);
        void sendOwedStops();
        void resetGesture(bool keepOwedStops);

        IHost&                      m_host;
        CPolicy                     m_policy;
        eState                      m_state = eState::IDLE;
        SGesture                    m_g;
        Physics::CVelocityEstimator m_est;

        bool m_timerLaunches = false; // TRACKING timer: launch (true) or abandon (false)

        // Momentum. Velocities in scroll units per ms.
        double m_velocityV         = 0.0;
        double m_velocityH         = 0.0;
        double m_lastTickMs        = 0.0;
        bool   m_lastStepFromTimer = false;

        // Protocol ownership: AXIS_V / AXIS_H bits of the axes on which the
        // client has an open scroll sequence that we must close: its real
        // axis_stop was swallowed, or we emitted on it.
        uint8_t m_owedStops = 0;
    };
}
