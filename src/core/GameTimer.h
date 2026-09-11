#pragma once

#include <chrono>

namespace vv::core {

class GameTimer final
{
public:
    GameTimer();

    void reset();

    void setPaused(bool paused);
    bool paused() const { return m_paused; }

    // Advances internal time. Returns delta seconds (0 when paused).
    float tickSeconds();

    double totalSeconds() const { return m_totalSeconds; }

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point m_last{};
    double m_totalSeconds = 0.0;
    bool m_paused = false;
};

} // namespace vv::core

