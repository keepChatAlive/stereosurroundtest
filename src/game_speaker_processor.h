#pragma once

#include "audio_frames.h"

#include <array>
#include <cstddef>
#include <vector>

namespace stereo_surround {

// Independent, portable fixed-listener backend. No OS or room-data
// measured HRTF dependency. Construction designs filters; playback allocates
// nothing. All setters/process calls belong to one audio thread.
struct GameSpeakerConfig {
    double sample_rate = 48'000.0;
    float speaker_span_degrees = 20.0F;
    float speaker_distance_metres = 1.0F;
    float head_width_metres = 0.18F;
    unsigned rear_profile = 2;
    float mix = 1.0F;       // 0..1, never dry/wet extrapolation
    float expansion = 0.0F; // 0..1.5, 0 = reference, 1.5 = deliberately unnatural
};

class GameSpeakerProcessor {
public:
    explicit GameSpeakerProcessor(const GameSpeakerConfig& config);
    void reset() noexcept;
    void setMix(float mix) noexcept;
    void setExpansion(float expansion) noexcept;
    [[nodiscard]] StereoFrame process(const SurroundFrame& input) noexcept;
    [[nodiscard]] std::size_t latencySamples() const noexcept { return delay_; }
    [[nodiscard]] std::size_t filterLength() const noexcept { return length_; }
    [[nodiscard]] float fixedGain() const noexcept { return gain_; }
    [[nodiscard]] float strictPeakGain() const noexcept { return strict_gain_; }
    [[nodiscard]] float preLimiterPeak() const noexcept { return pre_peak_; }
    [[nodiscard]] float limiterGain() const noexcept { return limiter_; }
    [[nodiscard]] double referenceFitError() const noexcept { return fit_error_; }
    [[nodiscard]] double maximumFilterGain() const noexcept { return max_filter_gain_; }

private:
    using Pair = std::array<float, 2>;
    using Bank = std::array<std::vector<Pair>, 8>;
    void design();
    GameSpeakerConfig config_;
    Bank reference_, expanded_;
    std::array<std::vector<std::array<float, 4>>, 8> packed_;
    std::array<std::vector<float>, 8> history_;
    std::vector<SurroundFrame> dry_delay_;
    std::size_t length_ = 0, delay_ = 0, position_ = 0, dry_position_ = 0;
    float mix_ = 1.0F, mix_target_ = 1.0F;
    float expansion_ = 0.0F, expansion_target_ = 0.0F;
    float ramp_step_ = 0.0F, release_ = 0.0F;
    float gain_ = 1.0F, strict_gain_ = 1.0F;
    float pre_peak_ = 0.0F, limiter_ = 1.0F;
    double fit_error_ = 0.0, max_filter_gain_ = 0.0;
};

} // namespace stereo_surround
