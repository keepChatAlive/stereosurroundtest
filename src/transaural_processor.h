#pragma once

#include "audio_frames.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace stereo_surround {

class GameSpeakerProcessor;

enum class TransauralProfile {
    natural,
    aggressive,
    game,
};

struct TransauralConfig {
    double sample_rate = 48'000.0;
    TransauralProfile profile = TransauralProfile::aggressive;
    // Total angle between the two physical speakers as seen by the listener.
    float speaker_span_degrees = 20.0F;
    // Radial head-centre-to-speaker distance. At close range, exact point
    // geometry changes inter-ear distance and level relative to a far field.
    float physical_speaker_distance_metres = 1.0F;
    float head_width_metres = 0.18F;
    // Five deliberately different parametric rear pinna-cue families, 0..4.
    unsigned rear_profile = 2;
    // 0 = delayed naive matrix, 1 = designed transaural response. Hyper mode
    // permits controlled extrapolation up to 1.5 for an intentionally obvious
    // effect.
    float strength = 1.25F;
    // 0 = pure regularized XTC side rendering, 1 = strong robust side anchor,
    // 1.5 = maximum same-side anchoring for an intentionally obvious image.
    float side_width = 1.5F;
};

// Portable fixed-listener 7.1-to-stereo speaker virtualizer. This backend has
// no dependency on Windows, WASAPI, or external DSP libraries. Its FIR
// bank is designed at construction time from an analytic two-speaker/two-ear
// model using a frequency-dependent Tikhonov-regularized inverse.
class TransauralDownmixer {
public:
    explicit TransauralDownmixer(const TransauralConfig& config);
    ~TransauralDownmixer();

    void reset();
    void setStrength(float strength) noexcept;
    void setSideWidth(float side_width) noexcept;
    [[nodiscard]] StereoFrame process(const SurroundFrame& input);

    [[nodiscard]] const TransauralConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] float strength() const noexcept { return strength_; }
    [[nodiscard]] float sideWidth() const noexcept { return side_width_; }
    [[nodiscard]] float preLimiterPeak() const noexcept;
    [[nodiscard]] float limiterGain() const noexcept;
    [[nodiscard]] float comparisonGain() const noexcept;
    [[nodiscard]] std::size_t latencySamples() const noexcept {
        return latency_samples_;
    }
    [[nodiscard]] std::size_t filterLength() const noexcept {
        return filter_length_;
    }

private:
    class DelayLine {
    public:
        void resize(std::size_t delay_samples);
        void reset();
        [[nodiscard]] float process(float input);

    private:
        std::vector<float> buffer_;
        std::size_t position_ = 0;
    };

    void designFilters();
    [[nodiscard]] StereoFrame delayedMatrix(const SurroundFrame& input);
    [[nodiscard]] StereoFrame limitStereoLinked(float left, float right);

    TransauralConfig config_{};
    std::unique_ptr<GameSpeakerProcessor> game_;
    float strength_ = 1.25F;
    float side_width_ = 1.5F;
    std::size_t filter_length_ = 0;
    std::size_t latency_samples_ = 0;
    std::size_t write_position_ = 0;
    std::array<std::vector<float>, 8> history_{};
    std::array<std::vector<float>, 8> left_filters_{};
    std::array<std::vector<float>, 8> right_filters_{};
    DelayLine matrix_left_delay_;
    DelayLine matrix_right_delay_;
    DelayLine lfe_delay_;
    DelayLine side_left_delay_;
    DelayLine side_right_delay_;
    float lfe_low_pass_state_ = 0.0F;
    float lfe_low_pass_coefficient_ = 0.0F;
    float limiter_gain_ = 1.0F;
    float pre_limiter_peak_ = 0.0F;
    float limiter_release_coefficient_ = 0.0F;
};

}  // namespace stereo_surround
