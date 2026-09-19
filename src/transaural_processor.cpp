#include "transaural_processor.h"
#include "game_speaker_processor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

namespace stereo_surround {
namespace {

using Complex = std::complex<double>;

constexpr double kPi = std::numbers::pi_v<double>;
constexpr double kSpeedOfSound = 343.0;
constexpr std::array<bool, 8> kDirectionalChannel = {
    true, true, true, false, true, true, true, true};

[[nodiscard]] double degreesToRadians(double degrees) {
    return degrees * kPi / 180.0;
}

[[nodiscard]] double gaussianOctaves(double frequency, double centre,
                                     double width_octaves) {
    if (frequency <= 0.0 || centre <= 0.0) {
        return 0.0;
    }
    const double distance = std::log2(frequency / centre) / width_octaves;
    return std::exp(-0.5 * distance * distance);
}

[[nodiscard]] double woodworthDelay(double angle_radians,
                                    double head_radius) {
    const double sign = angle_radians < 0.0 ? -1.0 : 1.0;
    const double angle = std::clamp(std::abs(angle_radians), 0.0, kPi);
    const double distance =
        angle <= kPi / 2.0
            ? head_radius * (angle + std::sin(angle))
            : head_radius * (kPi - angle + std::sin(angle));
    return sign * distance / kSpeedOfSound;
}

[[nodiscard]] double rearCueDecibels(double frequency, unsigned profile,
                                     bool same_side, bool hyper) {
    constexpr std::array<double, 5> kPeak = {850.0, 950.0, 1'050.0, 1'160.0,
                                              1'280.0};
    constexpr std::array<double, 5> kNotchOne = {4'450.0, 4'950.0, 5'500.0,
                                                  6'100.0, 6'750.0};
    constexpr std::array<double, 5> kNotchTwo = {7'100.0, 7'750.0, 8'450.0,
                                                  9'150.0, 9'900.0};
    const std::size_t index = std::min<std::size_t>(profile, kPeak.size() - 1U);
    const double ear_shift = same_side ? 0.96 : 1.04;
    const double emphasis = hyper ? 1.45 : 0.85;
    return emphasis *
           (3.8 * gaussianOctaves(frequency, kPeak[index], 0.58) -
            7.0 * gaussianOctaves(frequency,
                                  kNotchOne[index] * ear_shift, 0.20) -
            4.5 * gaussianOctaves(frequency,
                                  kNotchTwo[index] / ear_shift, 0.22));
}

[[nodiscard]] Complex earResponse(double angle_degrees, double frequency,
                                  bool left_ear, double head_radius,
                                  TransauralProfile profile,
                                  unsigned rear_profile,
                                  bool include_virtual_cues) {
    const bool hyper = profile == TransauralProfile::aggressive;
    const double angle = degreesToRadians(angle_degrees);
    const double lateral = std::sin(angle);
    const double ild_corner = 720.0;
    const double frequency_weight =
        frequency * frequency /
        (frequency * frequency + ild_corner * ild_corner);
    // The reproduction matrix describes real physical speakers and must never
    // inherit Hyper's intentionally exaggerated virtual cue. Conflating these
    // made the inverse expect more natural speaker separation than actually
    // exists and allowed uncancelled crossfeed to pull side images inward.
    const double maximum_ild_db =
        include_virtual_cues ? (hyper ? 24.0 : 15.0) : 13.0;
    const double ild_db = maximum_ild_db * frequency_weight * lateral;
    double left_gain = std::pow(10.0, -ild_db / 40.0);
    double right_gain = std::pow(10.0, ild_db / 40.0);
    const double energy_normalizer =
        std::sqrt((left_gain * left_gain + right_gain * right_gain) / 2.0);
    left_gain /= energy_normalizer;
    right_gain /= energy_normalizer;

    double itd = woodworthDelay(angle, head_radius);
    if (hyper && include_virtual_cues) {
        itd *= 1.16;
    }
    const double ear_delay = left_ear ? 0.5 * itd : -0.5 * itd;
    double magnitude = left_ear ? left_gain : right_gain;

    if (include_virtual_cues) {
        const double absolute_angle = std::abs(angle_degrees);
        const double ear_side = left_ear ? -1.0 : 1.0;
        const bool same_side = angle_degrees * ear_side >= 0.0;
        double cue_db = 0.0;
        if (absolute_angle >= 112.0) {
            cue_db += rearCueDecibels(frequency, rear_profile, same_side, hyper);
        } else if (absolute_angle >= 70.0) {
            cue_db += (hyper ? 2.4 : 1.1) *
                      gaussianOctaves(frequency, 2'700.0, 0.72);
        }
        magnitude *= std::pow(10.0, cue_db / 20.0);
    }

    const double phase = -2.0 * kPi * frequency * ear_delay;
    return std::polar(magnitude, phase);
}

[[nodiscard]] Complex physicalSpeakerResponse(
    double angle_degrees, double frequency, bool left_ear, double head_radius,
    double speaker_distance, TransauralProfile profile,
    unsigned rear_profile) {
    Complex response = earResponse(angle_degrees, frequency, left_ear,
                                   head_radius, profile, rear_profile, false);

    // The spherical-head response above is a far-field model. Retain its head
    // shadow and Woodworth delay, then add the finite-distance correction from
    // exact horizontal point geometry. This matters most for handheld and
    // desktop listening distances while converging to the old response as the
    // speakers move away.
    const double angle = degreesToRadians(angle_degrees);
    const double source_x = speaker_distance * std::sin(angle);
    const double source_y = speaker_distance * std::cos(angle);
    const double ear_x = left_ear ? -head_radius : head_radius;
    const double ear_distance =
        std::hypot(source_y, source_x - ear_x);
    const double opposite_ear_x = -ear_x;
    const double opposite_distance =
        std::hypot(source_y, source_x - opposite_ear_x);

    const double left_distance = left_ear ? ear_distance : opposite_distance;
    const double right_distance = left_ear ? opposite_distance : ear_distance;
    const double exact_itd =
        (left_distance - right_distance) / kSpeedOfSound;
    const double far_field_straight_itd =
        2.0 * head_radius * std::sin(angle) / kSpeedOfSound;
    const double correction = exact_itd - far_field_straight_itd;
    const double correction_delay = left_ear ? 0.5 * correction
                                             : -0.5 * correction;
    const double distance_gain = speaker_distance / ear_distance;
    const double phase = -2.0 * kPi * frequency * correction_delay;
    return response * std::polar(distance_gain, phase);
}

struct TwoVector {
    Complex left{};
    Complex right{};
};

[[nodiscard]] TwoVector solveRegularized(
    const std::array<std::array<Complex, 2>, 2>& transfer,
    const TwoVector& desired, double regularization) {
    const Complex a00 = std::conj(transfer[0][0]) * transfer[0][0] +
                        std::conj(transfer[1][0]) * transfer[1][0] +
                        regularization;
    const Complex a01 = std::conj(transfer[0][0]) * transfer[0][1] +
                        std::conj(transfer[1][0]) * transfer[1][1];
    const Complex a10 = std::conj(a01);
    const Complex a11 = std::conj(transfer[0][1]) * transfer[0][1] +
                        std::conj(transfer[1][1]) * transfer[1][1] +
                        regularization;
    const Complex b0 = std::conj(transfer[0][0]) * desired.left +
                       std::conj(transfer[1][0]) * desired.right;
    const Complex b1 = std::conj(transfer[0][1]) * desired.left +
                       std::conj(transfer[1][1]) * desired.right;
    const Complex determinant = a00 * a11 - a01 * a10;
    if (std::abs(determinant) < 1.0e-15) {
        return {};
    }
    return {(a11 * b0 - a01 * b1) / determinant,
            (-a10 * b0 + a00 * b1) / determinant};
}

[[nodiscard]] double baseRegularization(double frequency, bool hyper) {
    double value = hyper ? 0.0007 : 0.004;
    if (frequency < 500.0) {
        const double weight = std::clamp((500.0 - frequency) / 500.0, 0.0, 1.0);
        value += weight * weight * (hyper ? 0.055 : 0.11);
    }
    if (frequency > 8'000.0) {
        const double weight =
            std::clamp((frequency - 8'000.0) / 10'000.0, 0.0, 1.0);
        value += weight * weight * (hyper ? 0.025 : 0.06);
    }
    return value;
}

void fft(std::vector<Complex>& values, bool inverse) {
    const std::size_t count = values.size();
    for (std::size_t index = 1, reversed = 0; index < count; ++index) {
        std::size_t bit = count >> 1U;
        while ((reversed & bit) != 0U) {
            reversed ^= bit;
            bit >>= 1U;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }
    for (std::size_t length = 2; length <= count; length <<= 1U) {
        const double angle = (inverse ? 2.0 : -2.0) * kPi /
                             static_cast<double>(length);
        const Complex root(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < count; start += length) {
            Complex twiddle(1.0, 0.0);
            for (std::size_t offset = 0; offset < length / 2U; ++offset) {
                const Complex even = values[start + offset];
                const Complex odd =
                    values[start + offset + length / 2U] * twiddle;
                values[start + offset] = even + odd;
                values[start + offset + length / 2U] = even - odd;
                twiddle *= root;
            }
        }
    }
    if (inverse) {
        for (Complex& value : values) {
            value /= static_cast<double>(count);
        }
    }
}

[[nodiscard]] std::size_t nextPowerOfTwo(std::size_t value) {
    std::size_t result = 1;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}

}  // namespace

void TransauralDownmixer::DelayLine::resize(std::size_t delay_samples) {
    buffer_.assign(delay_samples, 0.0F);
    position_ = 0;
}

void TransauralDownmixer::DelayLine::reset() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0F);
    position_ = 0;
}

float TransauralDownmixer::DelayLine::process(float input) {
    if (buffer_.empty()) {
        return input;
    }
    const float output = buffer_[position_];
    buffer_[position_] = input;
    position_ = (position_ + 1U) % buffer_.size();
    return output;
}

TransauralDownmixer::TransauralDownmixer(const TransauralConfig& config)
    : config_(config),
      strength_(config.strength),
      side_width_(config.side_width) {
    if (!std::isfinite(config_.sample_rate) || config_.sample_rate < 8'000.0 ||
        config_.sample_rate > 192'000.0) {
        throw std::invalid_argument("Unsupported transaural sample rate");
    }
    if (!std::isfinite(config_.speaker_span_degrees) ||
        config_.speaker_span_degrees < 8.0F ||
        config_.speaker_span_degrees > 60.0F) {
        throw std::invalid_argument("Speaker span must be between 8 and 60 degrees");
    }
    if (!std::isfinite(config_.physical_speaker_distance_metres) ||
        config_.physical_speaker_distance_metres < 0.20F ||
        config_.physical_speaker_distance_metres > 5.0F) {
        throw std::invalid_argument(
            "Physical speaker distance must be between 0.20 and 5.0 metres");
    }
    if (!std::isfinite(config_.head_width_metres) ||
        config_.head_width_metres < 0.13F || config_.head_width_metres > 0.24F) {
        throw std::invalid_argument("Head width must be between 0.13 and 0.24 metres");
    }
    config_.rear_profile = std::min(config_.rear_profile, 4U);
    setStrength(strength_);
    setSideWidth(side_width_);

    if (config_.profile == TransauralProfile::game) {
        GameSpeakerConfig game_config;
        game_config.sample_rate = config_.sample_rate;
        game_config.speaker_span_degrees = config_.speaker_span_degrees;
        game_config.speaker_distance_metres = config_.physical_speaker_distance_metres;
        game_config.head_width_metres = config_.head_width_metres;
        game_config.rear_profile = config_.rear_profile;
        game_config.mix = strength_;
        game_config.expansion = side_width_;
        game_ = std::make_unique<GameSpeakerProcessor>(game_config);
        filter_length_ = game_->filterLength();
        latency_samples_ = game_->latencySamples();
        return;
    }

    const double rate_scale = config_.sample_rate / 48'000.0;
    filter_length_ = std::max<std::size_t>(64U, static_cast<std::size_t>(
        std::llround(192.0 * rate_scale)));
    latency_samples_ = std::max<std::size_t>(1U, static_cast<std::size_t>(
        std::llround(0.0015 * config_.sample_rate)));
    for (std::vector<float>& channel : history_) {
        channel.assign(filter_length_, 0.0F);
    }
    matrix_left_delay_.resize(latency_samples_);
    matrix_right_delay_.resize(latency_samples_);
    lfe_delay_.resize(latency_samples_);
    side_left_delay_.resize(latency_samples_);
    side_right_delay_.resize(latency_samples_);
    lfe_low_pass_coefficient_ = static_cast<float>(
        1.0 - std::exp(-2.0 * kPi * 120.0 / config_.sample_rate));
    // Instant attack prevents overload; a 50 ms release avoids rapid gain
    // pumping. One shared gain is always applied to both speaker feeds so an
    // overload cannot flatten the louder side and collapse the spatial cue.
    limiter_release_coefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.050 * config_.sample_rate)));
    designFilters();
    reset();
}

TransauralDownmixer::~TransauralDownmixer() = default;

float TransauralDownmixer::preLimiterPeak() const noexcept {
    return game_ ? game_->preLimiterPeak() : pre_limiter_peak_;
}
float TransauralDownmixer::limiterGain() const noexcept {
    return game_ ? game_->limiterGain() : limiter_gain_;
}
float TransauralDownmixer::comparisonGain() const noexcept {
    return game_ ? game_->fixedGain() / .36F : 1.0F;
}

void TransauralDownmixer::setStrength(float strength) noexcept {
    strength_ = std::isfinite(strength) ? std::clamp(strength, 0.0F,
        config_.profile == TransauralProfile::game ? 1.0F : 1.5F)
                                        : 1.0F;
    if (game_) game_->setMix(strength_);
}

void TransauralDownmixer::setSideWidth(float side_width) noexcept {
    side_width_ = std::isfinite(side_width)
                      ? std::clamp(side_width, 0.0F, 1.5F)
                      : (config_.profile == TransauralProfile::game ? 0.0F : 1.0F);
    if (game_) game_->setExpansion(side_width_);
}

void TransauralDownmixer::reset() {
    if (game_) { game_->reset(); return; }
    for (std::vector<float>& channel : history_) {
        std::fill(channel.begin(), channel.end(), 0.0F);
    }
    write_position_ = 0;
    matrix_left_delay_.reset();
    matrix_right_delay_.reset();
    lfe_delay_.reset();
    side_left_delay_.reset();
    side_right_delay_.reset();
    lfe_low_pass_state_ = 0.0F;
    limiter_gain_ = 1.0F;
    pre_limiter_peak_ = 0.0F;
}

void TransauralDownmixer::designFilters() {
    const bool hyper = config_.profile == TransauralProfile::aggressive;
    const std::array<double, 8> target_angles =
        hyper ? std::array<double, 8>{-25.0, 25.0, 0.0, 0.0, -150.0,
                                      150.0, -90.0, 90.0}
              : std::array<double, 8>{-30.0, 30.0, 0.0, 0.0, -135.0,
                                      135.0, -90.0, 90.0};
    const std::array<double, 8> target_energy =
        hyper ? std::array<double, 8>{0.36, 0.36, 0.32, 0.0, 0.43, 0.43,
                                      0.40, 0.40}
              : std::array<double, 8>{0.34, 0.34, 0.32, 0.0, 0.35, 0.35,
                                      0.35, 0.35};
    const std::size_t transform_size = nextPowerOfTwo(filter_length_ * 2U);
    const double head_radius = 0.5 * static_cast<double>(config_.head_width_metres);
    const double speaker_left =
        -0.5 * static_cast<double>(config_.speaker_span_degrees);
    const double speaker_right = -speaker_left;
    const double maximum_filter_gain = hyper ? 3.98 : 2.0;

    for (std::size_t channel = 0; channel < left_filters_.size(); ++channel) {
        left_filters_[channel].assign(filter_length_, 0.0F);
        right_filters_[channel].assign(filter_length_, 0.0F);
        if (!kDirectionalChannel[channel]) {
            continue;
        }

        std::vector<Complex> left_spectrum(transform_size, Complex{});
        std::vector<Complex> right_spectrum(transform_size, Complex{});
        for (std::size_t bin = 0; bin <= transform_size / 2U; ++bin) {
            const double frequency = static_cast<double>(bin) *
                                     config_.sample_rate /
                                     static_cast<double>(transform_size);
            const std::array<std::array<Complex, 2>, 2> transfer = {{
                {physicalSpeakerResponse(
                     speaker_left, frequency, true, head_radius,
                     config_.physical_speaker_distance_metres, config_.profile,
                     config_.rear_profile),
                 physicalSpeakerResponse(
                     speaker_right, frequency, true, head_radius,
                     config_.physical_speaker_distance_metres, config_.profile,
                     config_.rear_profile)},
                {physicalSpeakerResponse(
                     speaker_left, frequency, false, head_radius,
                     config_.physical_speaker_distance_metres, config_.profile,
                     config_.rear_profile),
                 physicalSpeakerResponse(
                     speaker_right, frequency, false, head_radius,
                     config_.physical_speaker_distance_metres, config_.profile,
                     config_.rear_profile)},
            }};
            const TwoVector desired = {
                earResponse(target_angles[channel], frequency, true, head_radius,
                            config_.profile, config_.rear_profile, true),
                earResponse(target_angles[channel], frequency, false, head_radius,
                            config_.profile, config_.rear_profile, true)};

            double regularization = baseRegularization(frequency, hyper);
            TwoVector solution{};
            for (int attempt = 0; attempt < 14; ++attempt) {
                solution = solveRegularized(transfer, desired, regularization);
                if (std::max(std::abs(solution.left),
                             std::abs(solution.right)) <= maximum_filter_gain) {
                    break;
                }
                regularization *= 2.0;
            }
            const double causal_phase =
                -2.0 * kPi * frequency *
                static_cast<double>(latency_samples_) / config_.sample_rate;
            const Complex causal_shift = std::polar(1.0, causal_phase);
            left_spectrum[bin] = solution.left * causal_shift;
            right_spectrum[bin] = solution.right * causal_shift;
            if (bin > 0 && bin < transform_size / 2U) {
                left_spectrum[transform_size - bin] =
                    std::conj(left_spectrum[bin]);
                right_spectrum[transform_size - bin] =
                    std::conj(right_spectrum[bin]);
            }
        }
        fft(left_spectrum, true);
        fft(right_spectrum, true);

        double energy = 0.0;
        const std::size_t taper_start = filter_length_ * 4U / 5U;
        for (std::size_t tap = 0; tap < filter_length_; ++tap) {
            double taper = 1.0;
            if (tap >= taper_start && filter_length_ > taper_start + 1U) {
                const double position =
                    static_cast<double>(tap - taper_start) /
                    static_cast<double>(filter_length_ - taper_start - 1U);
                taper = 0.5 * (1.0 + std::cos(kPi * position));
            }
            const double left = left_spectrum[tap].real() * taper;
            const double right = right_spectrum[tap].real() * taper;
            left_filters_[channel][tap] = static_cast<float>(left);
            right_filters_[channel][tap] = static_cast<float>(right);
            energy += left * left + right * right;
        }
        if (energy > 1.0e-15) {
            const float scale = static_cast<float>(
                target_energy[channel] / std::sqrt(energy));
            for (float& coefficient : left_filters_[channel]) {
                coefficient *= scale;
            }
            for (float& coefficient : right_filters_[channel]) {
                coefficient *= scale;
            }
        }
    }
}

StereoFrame TransauralDownmixer::delayedMatrix(const SurroundFrame& input) {
    constexpr float centre = 0.70710678F;
    constexpr float surround = 0.5F;
    constexpr float lfe = 0.25F;
    constexpr float headroom = 0.36F;
    const float left = headroom *
                       (input[0] + centre * input[2] + lfe * input[3] +
                        surround * input[4] + surround * input[6]);
    const float right = headroom *
                        (input[1] + centre * input[2] + lfe * input[3] +
                         surround * input[5] + surround * input[7]);
    return {matrix_left_delay_.process(left),
            matrix_right_delay_.process(right)};
}

StereoFrame TransauralDownmixer::limitStereoLinked(float left, float right) {
    // -0.45 dBFS leaves a little sample-peak margin for the downstream format
    // converter. This is a safety stage, not loudness maximization.
    constexpr float ceiling = 0.95F;
    if (!std::isfinite(left) || !std::isfinite(right)) {
        limiter_gain_ = 0.0F;
        return {};
    }
    const float peak = std::max(std::abs(left), std::abs(right));
    pre_limiter_peak_ = peak;
    const float required_gain =
        peak > ceiling ? ceiling / peak : 1.0F;
    if (required_gain < limiter_gain_) {
        limiter_gain_ = required_gain;
    } else if (limiter_gain_ < 1.0F) {
        limiter_gain_ = std::min(
            required_gain,
            limiter_gain_ + limiter_release_coefficient_ *
                                (1.0F - limiter_gain_));
    }
    return {left * limiter_gain_, right * limiter_gain_};
}

StereoFrame TransauralDownmixer::process(const SurroundFrame& input) {
    if (game_) return game_->process(input);
    for (std::size_t channel = 0; channel < history_.size(); ++channel) {
        history_[channel][write_position_] = input[channel];
    }

    double wet_left = 0.0;
    double wet_right = 0.0;
    double wet_side_left = 0.0;
    double wet_side_right = 0.0;
    for (std::size_t channel = 0; channel < history_.size(); ++channel) {
        if (!kDirectionalChannel[channel]) {
            continue;
        }
        std::size_t coefficient = 0;
        for (std::size_t position = write_position_ + 1U; position > 0U;
             --position) {
            const float sample = history_[channel][position - 1U];
            const double left = left_filters_[channel][coefficient] * sample;
            const double right = right_filters_[channel][coefficient] * sample;
            wet_left += left;
            wet_right += right;
            if (channel == 6U || channel == 7U) {
                wet_side_left += left;
                wet_side_right += right;
            }
            ++coefficient;
        }
        for (std::size_t position = filter_length_;
             position > write_position_ + 1U; --position) {
            const float sample = history_[channel][position - 1U];
            const double left = left_filters_[channel][coefficient] * sample;
            const double right = right_filters_[channel][coefficient] * sample;
            wet_left += left;
            wet_right += right;
            if (channel == 6U || channel == 7U) {
                wet_side_left += left;
                wet_side_right += right;
            }
            ++coefficient;
        }
    }
    write_position_ = (write_position_ + 1U) % filter_length_;

    // Pure XTC deliberately drives both speakers. When the analytic room/head
    // model is imperfect, its cancellation feed is heard directly and pulls a
    // side image inward. The independent same-side anchor makes failure
    // graceful and obvious while retaining enough XTC for externalization.
    // It is latency-aligned and energy-matched to the designed side FIRs.
    const bool hyper = config_.profile == TransauralProfile::aggressive;
    const float maximum_anchor_mix = hyper ? 0.72F : 0.48F;
    const float anchor_mix =
        maximum_anchor_mix * std::clamp(side_width_ / 1.5F, 0.0F, 1.0F);
    const float anchor_level = hyper ? 0.40F : 0.35F;
    const double anchor_left =
        anchor_level * side_left_delay_.process(input[6]);
    const double anchor_right =
        anchor_level * side_right_delay_.process(input[7]);
    wet_left += anchor_mix * (anchor_left - wet_side_left);
    wet_right += anchor_mix * (anchor_right - wet_side_right);

    lfe_low_pass_state_ +=
        lfe_low_pass_coefficient_ * (input[3] - lfe_low_pass_state_);
    const float delayed_lfe = lfe_delay_.process(lfe_low_pass_state_);
    wet_left += 0.09 * delayed_lfe;
    wet_right += 0.09 * delayed_lfe;

    const StereoFrame dry = delayedMatrix(input);
    const float wet_left_float = static_cast<float>(wet_left);
    const float wet_right_float = static_cast<float>(wet_right);
    return limitStereoLinked(
        dry.left + strength_ * (wet_left_float - dry.left),
        dry.right + strength_ * (wet_right_float - dry.right));
}

}  // namespace stereo_surround
