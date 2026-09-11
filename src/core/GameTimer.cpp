#include "core/GameTimer.h"

namespace vv::core {

GameTimer::GameTimer() { reset(); }

void GameTimer::reset() {
  m_last = Clock::now();
  m_totalSeconds = 0.0;
  m_paused = false;
}

void GameTimer::setPaused(bool paused) {
  if (m_paused == paused) {
    return;
  }
  m_paused = paused;
  m_last = Clock::now();
}

float GameTimer::tickSeconds() {
  const Clock::time_point now = Clock::now();
  const std::chrono::duration<double> delta = now - m_last;
  m_last = now;

  if (m_paused) {
    return 0.0f;
  }

  const double dt = delta.count();
  m_totalSeconds += dt;
  return static_cast<float>(dt);
}

} // namespace vv::core
