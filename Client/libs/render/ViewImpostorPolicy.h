#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// Compare whole-frame cadence with and without GPU image captures. A cheap scene can be CPU-bound:
// saving triangles alone is insufficient evidence to keep an extra render path enabled.
class ViewImpostorPolicy
{
  public:
    enum class Stage
    {
        Baseline,
        Trial,
        Enabled,
        Geometry
    };

    bool Observe(double now, bool stable)
    {
        if (!std::isfinite(now) || now < 0)
        {
            Reset(0);
            m_started = false;
            return false;
        }
        const double delta = now - m_previous;
        m_previous = now;
        if (!stable || !m_started || delta <= 0 || delta >= 0.5)
        {
            Reset(now);
            m_started = stable;
            return false;
        }
        if (m_stage == Stage::Enabled)
            return true;
        if (m_stage == Stage::Geometry)
        {
            if (now - m_phaseStarted >= 10)
                Reset(now);
            return false;
        }
        // Discard upload/pipeline warm-up at the beginning of each phase.
        if (now - m_phaseStarted >= 0.05)
        {
            m_samples[m_cursor++ % m_samples.size()] = delta;
            m_count = std::min(m_count + 1, m_samples.size());
        }
        if (now - m_phaseStarted >= 0.75 && m_count >= 32)
        {
            auto sorted = m_samples;
            std::sort(sorted.begin(), sorted.begin() + m_count);
            const double median = sorted[m_count / 2];
            if (m_stage == Stage::Baseline)
            {
                m_baseline = median;
                m_stage = Stage::Trial;
                m_trialUsed = false;
            }
            else
                m_stage = m_trialUsed && median < m_baseline * 0.98 ? Stage::Enabled : Stage::Geometry;
            m_phaseStarted = now;
            m_count = m_cursor = 0;
        }
        return m_stage == Stage::Trial || m_stage == Stage::Enabled;
    }

    void RecordUse()
    {
        m_trialUsed = true;
    }
    Stage State() const
    {
        return m_stage;
    }
    const char* Name() const
    {
        switch (m_stage)
        {
        case Stage::Baseline:
            return "calibrating";
        case Stage::Trial:
            return "trial";
        case Stage::Enabled:
            return "enabled";
        case Stage::Geometry:
            return "geometry";
        }
        return "geometry";
    }

  private:
    void Reset(double now)
    {
        m_stage = Stage::Baseline;
        m_phaseStarted = now;
        m_previous = now;
        m_count = m_cursor = 0;
        m_trialUsed = false;
    }
    Stage m_stage = Stage::Baseline;
    std::array<double, 512> m_samples{};
    std::size_t m_count = 0, m_cursor = 0;
    double m_phaseStarted = 0, m_previous = 0, m_baseline = 0;
    bool m_started = false, m_trialUsed = false;
};
