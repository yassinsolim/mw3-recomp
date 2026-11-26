// The rule of adaptive.h alone: no clock, no device, no log, so that it can
// be checked on windows nobody measured (tests/adaptive_test.cpp).
#include "adaptive.h"

#include <algorithm>
#include <cstdio>

void adaptive::Governor::Start(std::vector<uint32_t> steps, bool rate, bool shadows)
{
    steps.erase(std::remove(steps.begin(), steps.end(), 0u), steps.end());
    std::sort(steps.begin(), steps.end(), [](uint32_t a, uint32_t b) { return a > b; });
    steps.erase(std::unique(steps.begin(), steps.end()), steps.end());
    m_steps = std::move(steps);
    m_step = 0;
    m_rate = rate && m_steps.size() > 1;
    m_shadows = shadows;
    // Off until a window has shown the room for them: a device that has it
    // gets them half a minute into play, one that has not never pays for them.
    m_shadowsOn = false;
    m_missed = m_missedFrames = m_shadowsCalm = m_upCalm = 0;
    m_shadowsWait = m_upWait = 0;
    m_shadowsRetry = m_upRetry = kRetry;
    m_sinceShadows = m_sinceUp = kMostRetry;
    m_why.clear();
}

bool adaptive::Governor::HasRoom(const Measured& window, double periodMs, double gpuShare, double rendererShare) const
{
    // A time the device does not give is no objection: the change is then
    // tried, and taken back if the frames say so.
    return (window.gpuMs < 0 || window.gpuMs <= gpuShare * periodMs) &&
           (window.rendererMs < 0 || window.rendererMs <= rendererShare * periodMs);
}

void adaptive::Governor::StepTo(size_t step, std::string why)
{
    m_step = step;
    m_missed = m_missedFrames = m_shadowsCalm = m_upCalm = 0;
    m_why = std::move(why);
}

bool adaptive::Governor::GiveUpShadows()
{
    if (!m_shadows || !m_shadowsOn) return false;
    m_shadowsOn = false;
    m_shadowsWait = m_shadowsRetry;
    // Gone within a minute of coming on, they did not hold: the next wait is twice this one.
    if (m_sinceShadows < kRetry) m_shadowsRetry = std::min(m_shadowsRetry * 2, kMostRetry);
    m_shadowsCalm = 0;
    m_why = "the frames came late with them";
    return true;
}

bool adaptive::Governor::Feed(const Measured& window)
{
    const double target = double(Target());
    if (target <= 0 || window.seconds <= 0) return false;
    const double periodMs = 1000.0 / target;
    const double seconds = window.seconds;
    m_shadowsWait = std::max(0.0, m_shadowsWait - seconds);
    m_upWait = std::max(0.0, m_upWait - seconds);
    m_sinceShadows = std::min(m_sinceShadows + seconds, kMostRetry);
    m_sinceUp = std::min(m_sinceUp + seconds, kMostRetry);

    if (window.fps < kMiss * target)
    {
        m_missed += seconds;
        m_missedFrames += window.fps * seconds;
        m_shadowsCalm = m_upCalm = 0;
        if (m_shadowsOn && m_shadows && m_missed >= kShadowsOffSeconds)
        {
            GiveUpShadows();
            m_missed = m_missedFrames = 0;
            return true;
        }
        if (!m_rate || m_missed < kDownSeconds) return false;
        // Where the chip is what a frame waits for, the scene's pixels go
        // before the rate does; where it is not, they would buy nothing.
        const bool chipLate = window.gpuMs < 0 || window.gpuMs >= 0.85 * periodMs;
        if (chipLate && !window.sceneAtFloor) return false;
        const double sustained = m_missedFrames / m_missed;
        size_t pick = m_step;
        for (size_t step = m_steps.size() - 1; step > m_step; step--)
            if (double(m_steps[step]) * kGivenUp >= sustained) { pick = step; break; }
        if (pick == m_step)
        {
            // Too far above the next step to give it up for an even pace.
            m_missed = m_missedFrames = 0;
            return false;
        }
        if (m_sinceUp < kRetry) m_upRetry = std::min(m_upRetry * 2, kMostRetry);
        m_upWait = m_upRetry;
        char why[96];
        std::snprintf(why, sizeof why, "%.0f s at %.0f frames a second under %u", kDownSeconds, sustained,
                      m_steps[m_step]);
        StepTo(pick, why);
        return true;
    }
    m_missed = m_missedFrames = 0;

    const bool calm = window.fps >= 0.97 * target && window.sceneWhole;
    if (m_rate && m_step > 0 && calm && HasRoom(window, 1000.0 / double(m_steps[m_step - 1]), kUpShare, kUpShare))
    {
        // The faster rate before the shadows, which would take the room for
        // it: frames are what was given up last. Also while the step up
        // still waits its turn.
        m_shadowsCalm = 0;
        if (m_upWait > 0) return false;
        m_upCalm += seconds;
        if (m_upCalm < kUpCalm) return false;
        m_sinceUp = 0;
        char why[96];
        std::snprintf(why, sizeof why, "%.0f s with room for it", kUpCalm);
        StepTo(m_step - 1, why);
        return true;
    }
    m_upCalm = 0;
    if (m_shadows && !m_shadowsOn && m_shadowsWait <= 0 && calm &&
        HasRoom(window, periodMs, kShadowsGpu, kShadowsRenderer))
    {
        m_shadowsCalm += seconds;
        if (m_shadowsCalm < kShadowsCalm) return false;
        m_shadowsOn = true;
        m_sinceShadows = 0;
        m_shadowsCalm = 0;
        m_why = "the frames have room for them";
        return true;
    }
    m_shadowsCalm = 0;
    return false;
}
