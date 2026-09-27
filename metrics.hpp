#pragma once
// Measurement instrumentation for hypr-kinetic-scroll.
//
// Records, per gesture, the cost of the plugin's hot paths (ns per axis event,
// ns per momentum step) and the behaviour that determines feel (lift-to-first-
// emit latency, per-frame cadence jitter, launch velocity, duration, travel).
// One JSON line per gesture is appended to plugin:kinetic-scroll:metrics_file.
// Empty path (default) disables everything except a cached bool test per hook.
//
// Serialization and file I/O never run inside input or render callbacks:
// finished gestures are queued and written from an idle callback of the
// compositor's event loop.
//
// Keep this file identical across builds that are being compared.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct wl_event_loop;
struct wl_event_source;

namespace Metrics {

    inline int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Running statistics without storing samples.
    struct SStat {
        uint64_t n     = 0;
        double   sum   = 0.0;
        double   sumsq = 0.0;
        double   min   = 0.0;
        double   max   = 0.0;

        void add(double v) {
            if (n == 0) {
                min = v;
                max = v;
            } else {
                min = std::min(min, v);
                max = std::max(max, v);
            }
            ++n;
            sum += v;
            sumsq += v * v;
        }
        double mean() const {
            return n ? sum / static_cast<double>(n) : 0.0;
        }
        double stddev() const {
            if (n < 2)
                return 0.0;
            const double m = mean();
            return std::sqrt(std::max(0.0, sumsq / static_cast<double>(n) - m * m));
        }
    };

    struct SRawSample {
        uint32_t t  = 0; // libinput ms
        double   dv = 0.0;
        double   dh = 0.0;
    };

    // Everything measured for one gesture: from the first tracked axis event to
    // the stop, whether or not momentum was launched.
    struct SGesture {
        int64_t                 tFirstAxis = 0; // steady ns
        int64_t                 tLift      = 0; // steady ns, when the stop event was processed
        int64_t                 tLaunch    = 0; // steady ns, decay committed
        int64_t                 tFirstEmit = 0; // steady ns, first synthetic axis sent
        int64_t                 tEnd       = 0; // steady ns, stop
        bool                    launched   = false;

        SStat                   axisNs;     // cost of onAxis per event (touchpad events of this gesture)
        SStat                   stepNs;     // cost of step() per momentum frame
        SStat                   renderDtMs; // wall-time gap between render-driven steps (cadence)
        uint64_t                stepsRender = 0;
        uint64_t                stepsTimer  = 0;
        uint64_t                emits       = 0;
        uint64_t                stopsCancelled = 0; // real axis_stop events swallowed
        uint32_t                stopsSent      = 0; // synthetic axis_stop events actually delivered
        uint32_t                stopsOwed      = 0; // still owed when the gesture ended (undeliverable)

        double                  launchV = 0.0, launchH = 0.0; // units per ms
        double                  span    = 0.0;                // ms of samples used
        uint32_t                tail    = 0;                  // ms between last motion and lift
        double                  travelV = 0.0, travelH = 0.0; // raw units emitted (before scroll factor)

        std::string             windowClass;
        std::string             stopReason;
        std::vector<SRawSample> samples; // raw input, for offline replay (bounded)
    };

    // Cumulative counters outside gestures. Written with every gesture line so
    // the analyzer can diff them.
    struct SGlobal {
        SStat    idleAxisNs;      // onAxis calls that did not belong to a gesture (wheel, ignored, etc.)
        uint64_t idleRender  = 0; // onRenderPre calls while not decaying
        uint64_t gestures    = 0;
        uint64_t launches    = 0;
        uint64_t samplesDropped = 0; // raw samples beyond the per-gesture bound
    };

    class CCollector {
      public:
        static constexpr size_t MAX_SAMPLES = 1024;

        // Cheap; safe to call from hot paths.
        bool enabled() const {
            return m_enabled;
        }
        void setEnabled(bool on, std::string path) {
            m_enabled = on;
            m_path    = std::move(path);
        }
        void setLoop(wl_event_loop* loop) {
            m_loop = loop;
        }

        SGesture& gesture() {
            return m_gesture;
        }
        SGlobal& global() {
            return m_global;
        }

        void gestureBegin(int64_t now, const std::string& cls) {
            m_gesture             = {};
            m_gesture.tFirstAxis  = now;
            m_gesture.windowClass = cls;
            m_active              = true;
            ++m_global.gestures;
        }
        bool active() const {
            return m_active;
        }
        void addSample(uint32_t t, double dv, double dh) {
            if (m_gesture.samples.size() < MAX_SAMPLES)
                m_gesture.samples.push_back({t, dv, dh});
            else
                ++m_global.samplesDropped;
        }

        // Finalize: queue the record for writing from an idle callback.
        void gestureEnd(int64_t now);

        // Write everything queued, synchronously (plugin unload).
        void flushNow();

        // Cumulative ns spent inside gestureEnd (copy + enqueue). Scopes subtract
        // the delta so the metrics' own bookkeeping never shows up as plugin cost.
        int64_t flushNsTotal() const {
            return m_flushNs;
        }

      private:
        static void onIdle(void* data);
        void        writePending();

        bool                 m_enabled = false;
        bool                 m_active  = false;
        int64_t              m_flushNs = 0;
        std::string          m_path;
        SGesture             m_gesture;
        SGlobal              m_global;
        wl_event_loop*       m_loop = nullptr;
        wl_event_source*     m_idle = nullptr;
        std::deque<SGesture> m_pending;
    };

    // Defined in metrics.cpp. Not `inline`: see the note in globals.hpp.
    extern CCollector g_metrics;

    // RAII: adds elapsed ns (minus any metrics bookkeeping that happened inside
    // the scope) to a stat chosen at destruction time.
    class CScope {
      public:
        explicit CScope(SStat* stat) : m_stat(stat), m_t0(stat ? nowNs() : 0), m_flush0(g_metrics.flushNsTotal()) {}
        void retarget(SStat* stat) {
            m_stat = stat;
        }
        ~CScope() {
            if (m_stat)
                m_stat->add(static_cast<double>(nowNs() - m_t0 - (g_metrics.flushNsTotal() - m_flush0)));
        }
        int64_t start() const {
            return m_t0;
        }

      private:
        SStat*  m_stat;
        int64_t m_t0;
        int64_t m_flush0;
    };
}
