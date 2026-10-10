#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::runtime {
// Wall time only paces ticks; every consumed simulation step has the same dt.
class FixedStepClock {
  public:
    explicit FixedStepClock(double step = 1.0 / 120.0, double maxFrame = .1, unsigned maxSteps = 12)
        : step_(step > 0 ? step : 1.0 / 120.0), maxFrame_(std::max(0.0, maxFrame)),
          maxSteps_(std::max(1u, maxSteps)) {}
    void beginFrame(double seconds, bool active = true) {
        steps_ = 0;
        if (!active) {
            accumulator_ = 0;
            return;
        }
        if (!std::isfinite(seconds) || seconds < 0)
            seconds = 0;
        const double accepted = std::min(seconds, maxFrame_);
        discarded_ += seconds - accepted;
        accumulator_ += accepted;
        // A local session resynchronizes after suspension rather than inventing ticks.
        const double limit = step_ * maxSteps_;
        if (accumulator_ > limit) {
            discarded_ += accumulator_ - limit;
            accumulator_ = limit;
        }
    }
    bool consumeStep() {
        if (steps_ >= maxSteps_ || accumulator_ + step_ * 1e-9 < step_)
            return false;
        accumulator_ = std::max(0.0, accumulator_ - step_);
        ++steps_;
        ++tick_;
        return true;
    }
    double alpha() const {
        return std::clamp(accumulator_ / step_, 0.0, 1.0);
    }
    double stepSeconds() const {
        return step_;
    }
    double simulationSeconds() const {
        return tick_ * step_;
    }
    std::uint64_t tick() const {
        return tick_;
    }
    double discardedSeconds() const {
        return discarded_;
    }
    void reset(std::uint64_t tick = 0) {
        tick_ = tick;
        accumulator_ = 0;
        steps_ = 0;
        discarded_ = 0;
    }

  private:
    double step_, maxFrame_, accumulator_ = 0, discarded_ = 0;
    unsigned maxSteps_, steps_ = 0;
    std::uint64_t tick_ = 0;
};
} // namespace engine::runtime
