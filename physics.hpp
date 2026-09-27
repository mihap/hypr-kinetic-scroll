#pragma once
// Compositor-independent physics for hypr-kinetic-scroll: the launch-velocity
// estimator and the momentum decay integrator. No Hyprland, timers, focus or
// configuration here, so it can be replayed deterministically (see tests/).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Physics {

    // Signed difference of two wrapping 32-bit millisecond timestamps
    // (libinput / wl_pointer time). Correct across rollover for |diff| < 2^31.
    inline int32_t msDiff(uint32_t later, uint32_t earlier) {
        return static_cast<int32_t>(later - earlier);
    }

    struct SLaunch {
        double   v       = 0.0; // scroll units per ms
        double   h       = 0.0;
        double   spanMs  = 0.0; // ms of samples used (+ counted tail)
        uint32_t tailMs  = 0;   // last motion report -> lift
        uint32_t capMs   = 0;   // tail cap applied
        uint32_t samples = 0;   // samples inside the window
    };

    // Ring of the most recent motion reports of one gesture. Velocity at lift is
    // the sum of deltas over the last windowMs, divided by the time they cover.
    //
    // The tail (last report -> lift) is capped: a lift takes a few report
    // intervals during which contact fades and no motion is reported; counting
    // all of it launches every fast flick slower than the finger moved. Up to
    // two caps of tail costs nothing; beyond that the fingers were hesitating
    // and the launch fades linearly to zero at the window edge.
    class CVelocityEstimator {
      public:
        static constexpr size_t CAP = 64;

        void reset() {
            m_head    = 0;
            m_count   = 0;
            m_hasLast = false;
            m_lastMs  = 0;
        }

        // One axis delta at libinput time t. Same-t reports (other axis of the
        // same frame) merge into one sample.
        void add(uint32_t t, bool vertical, double delta) {
            if (m_count && m_hasLast && t == m_lastMs) {
                auto& last = m_s[(m_head + CAP - 1) % CAP];
                (vertical ? last.dv : last.dh) += delta;
                return;
            }
            SSample s;
            s.t = t;
            if (m_hasLast) {
                const int32_t d = msDiff(t, m_lastMs);
                if (d > 0 && d < 200)
                    s.dt = static_cast<uint32_t>(d);
            }
            (vertical ? s.dv : s.dh) = delta;
            m_s[m_head]            = s;
            m_head                 = (m_head + 1) % CAP;
            if (m_count < CAP)
                ++m_count;
            m_lastMs  = t;
            m_hasLast = true;
        }

        size_t count() const {
            return m_count;
        }
        bool hasSamples() const {
            return m_hasLast;
        }
        uint32_t lastMs() const {
            return m_lastMs;
        }

        // tailCapMs == 0 selects the automatic cap: 1.5 report intervals,
        // bounded to [8, 24] ms.
        SLaunch launch(uint32_t liftMs, uint32_t windowMs, uint32_t tailCapMs, double multiplier) const {
            SLaunch out;
            if (!m_hasLast)
                return out;

            const int32_t tailSigned = msDiff(liftMs, m_lastMs);
            const uint32_t tail      = tailSigned > 0 ? static_cast<uint32_t>(tailSigned) : 0;
            out.tailMs               = tail;
            if (tail > windowMs)
                return out;

            double   sumV = 0.0, sumH = 0.0;
            uint32_t sumDt = 0, lastDt = 0;
            for (size_t i = 0; i < m_count; ++i) {
                const auto&   s   = m_s[(m_head + CAP - 1 - i) % CAP];
                const int32_t age = msDiff(liftMs, s.t);
                if (age < 0)
                    continue;
                if (static_cast<uint32_t>(age) > windowMs)
                    break;
                ++out.samples;
                if (s.dt == 0)
                    continue; // first report of a gesture: covers unknown time
                if (lastDt == 0)
                    lastDt = s.dt;
                sumV += s.dv;
                sumH += s.dh;
                sumDt += s.dt;
            }
            if (sumDt == 0)
                return out;

            const uint32_t cap         = tailCapMs > 0 ? tailCapMs : std::clamp<uint32_t>(lastDt + lastDt / 2, 8, 24);
            const uint32_t tailCounted = std::min(tail, cap);
            const uint32_t grace       = 2 * cap;
            const double   rest        = (tail <= grace || windowMs <= grace) ? 1.0 : 1.0 - static_cast<double>(tail - grace) / static_cast<double>(windowMs - grace);
            const double   span        = static_cast<double>(sumDt + tailCounted);

            out.capMs  = cap;
            out.spanMs = span;
            out.v      = sumV / span * rest * multiplier;
            out.h      = sumH / span * rest * multiplier;
            return out;
        }

      private:
        struct SSample {
            uint32_t t  = 0;
            uint32_t dt = 0; // ms since the previous sample, 0 if unknown
            double   dv = 0.0;
            double   dh = 0.0;
        };
        std::array<SSample, CAP> m_s{};
        size_t                   m_head    = 0;
        size_t                   m_count   = 0;
        bool                     m_hasLast = false;
        uint32_t                 m_lastMs  = 0;
    };

    // Exponential decay v(t) = v0 e^(-lambda t), lambda per ms.
    struct SDecay {
        double lambda = 0.0;

        // decel is the velocity multiplier per 16 ms (0.967 ~ macOS "normal").
        static SDecay fromDecelPer16ms(double decel) {
            return {-std::log(std::clamp(decel, 0.5, 0.9999)) / 16.0};
        }

        // Advance velocity over dtRealMs of wall time. The displacement returned
        // covers only the first min(dtRealMs, dtTravelMaxMs): after a stall the
        // fling decays over the real elapsed time but the content does not jump
        // by the whole distance it would have travelled.
        void step(double& v, double& h, double dtRealMs, double dtTravelMaxMs, double& outV, double& outH) const {
            const double dtTravel = std::min(dtRealMs, dtTravelMaxMs);
            const double travel   = (1.0 - std::exp(-lambda * dtTravel)) / lambda;
            const double k        = std::exp(-lambda * std::min(dtRealMs, 60000.0));
            outV                  = v * travel;
            outH                  = h * travel;
            v *= k;
            h *= k;
        }
    };
}
