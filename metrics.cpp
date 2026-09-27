#include "metrics.hpp"
#include <wayland-server-core.h>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Metrics {

    CCollector g_metrics;

    static void stat(std::ostringstream& o, const char* name, const SStat& s) {
        o << "\"" << name << "\":{\"n\":" << s.n << ",\"mean\":" << s.mean() << ",\"stddev\":" << s.stddev() << ",\"min\":" << s.min << ",\"max\":" << s.max << "}";
    }

    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        return out;
    }

    static std::string serialize(const SGesture& g, const SGlobal& gl) {
        const auto ms = [](int64_t a, int64_t b) { return (a && b) ? static_cast<double>(b - a) / 1e6 : -1.0; };

        std::ostringstream o;
        o << std::setprecision(6);
        o << "{\"launched\":" << (g.launched ? "true" : "false");
        o << ",\"class\":\"" << escape(g.windowClass) << "\"";
        o << ",\"stop\":\"" << escape(g.stopReason) << "\"";
        o << ",\"lift_to_launch_ms\":" << ms(g.tLift, g.tLaunch);
        o << ",\"lift_to_first_emit_ms\":" << ms(g.tLift, g.tFirstEmit);
        o << ",\"fling_ms\":" << ms(g.tLaunch, g.tEnd);
        o << ",\"gesture_ms\":" << ms(g.tFirstAxis, g.tEnd);
        o << ",\"launch_v\":" << g.launchV << ",\"launch_h\":" << g.launchH;
        o << ",\"span_ms\":" << g.span << ",\"tail_ms\":" << g.tail;
        o << ",\"travel_v\":" << g.travelV << ",\"travel_h\":" << g.travelH;
        o << ",\"steps_render\":" << g.stepsRender << ",\"steps_timer\":" << g.stepsTimer << ",\"emits\":" << g.emits;
        o << ",\"stops_cancelled\":" << g.stopsCancelled << ",\"stops_sent\":" << g.stopsSent << ",\"stops_owed\":" << g.stopsOwed;
        o << ",\"stop_sent\":" << (g.stopsSent > 0 ? "true" : "false"); // kept for older analyzers
        o << ",";
        stat(o, "axis_ns", g.axisNs);
        o << ",";
        stat(o, "step_ns", g.stepNs);
        o << ",";
        stat(o, "render_dt_ms", g.renderDtMs);
        o << ",\"global\":{";
        stat(o, "idle_axis_ns", gl.idleAxisNs);
        o << ",\"idle_render\":" << gl.idleRender << ",\"gestures\":" << gl.gestures << ",\"launches\":" << gl.launches << ",\"samples_dropped\":" << gl.samplesDropped
          << "}";
        o << ",\"samples\":[";
        for (size_t i = 0; i < g.samples.size(); ++i) {
            if (i)
                o << ",";
            o << "[" << g.samples[i].t << "," << g.samples[i].dv << "," << g.samples[i].dh << "]";
        }
        o << "]}\n";
        return o.str();
    }

    void CCollector::gestureEnd(int64_t now) {
        if (!m_active)
            return;
        m_active = false;
        if (!m_enabled || m_path.empty())
            return;

        const int64_t t0 = nowNs();

        m_gesture.tEnd = now;
        if (m_gesture.launched)
            ++m_global.launches;

        // Only a move into the queue happens here; formatting and the write
        // run from the idle callback, outside input/render dispatch.
        m_pending.emplace_back(std::move(m_gesture));
        m_gesture = {};
        if (m_loop && !m_idle)
            m_idle = wl_event_loop_add_idle(m_loop, onIdle, this);

        m_flushNs += nowNs() - t0;
    }

    void CCollector::onIdle(void* data) {
        auto* self   = static_cast<CCollector*>(data);
        self->m_idle = nullptr; // idle sources are one-shot and freed by the loop
        self->writePending();
    }

    void CCollector::writePending() {
        if (m_pending.empty())
            return;
        {
            std::ofstream f(m_path, std::ios::app);
            if (f.is_open())
                for (const auto& g : m_pending)
                    f << serialize(g, m_global);
        } // close inside: nothing of the write escapes this function
        m_pending.clear();
    }

    void CCollector::flushNow() {
        if (m_idle) {
            wl_event_source_remove(m_idle);
            m_idle = nullptr;
        }
        writePending();
    }
}
