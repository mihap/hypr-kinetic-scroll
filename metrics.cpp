#include "metrics.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>

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

    void CCollector::gestureEnd(int64_t now, const char* reason) {
        if (!m_active)
            return;
        m_active = false;
        if (!m_enabled || m_path.empty())
            return;

        const int64_t flushStart = nowNs();

        auto& g      = m_gesture;
        g.tEnd       = now;
        g.stopReason = reason ? reason : "";
        if (g.launched)
            ++m_global.launches;

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
        o << ",";
        stat(o, "axis_ns", g.axisNs);
        o << ",";
        stat(o, "step_ns", g.stepNs);
        o << ",";
        stat(o, "render_dt_ms", g.renderDtMs);
        o << ",\"global\":{";
        stat(o, "idle_axis_ns", m_global.idleAxisNs);
        o << ",\"idle_render\":" << m_global.idleRender << ",\"gestures\":" << m_global.gestures << ",\"launches\":" << m_global.launches << "}";
        o << ",\"samples\":[";
        for (size_t i = 0; i < g.samples.size(); ++i) {
            if (i)
                o << ",";
            o << "[" << g.samples[i].t << "," << g.samples[i].dv << "," << g.samples[i].dh << "]";
        }
        o << "]}\n";

        std::ofstream f(m_path, std::ios::app);
        if (f.is_open())
            f << o.str();

        m_flushNs += nowNs() - flushStart;
    }
}
