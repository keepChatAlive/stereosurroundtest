#include "game_speaker_processor.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace stereo_surround {
namespace {
using C = std::complex<double>;
using V = std::array<C, 2>;
using M = std::array<V, 2>;
constexpr double pi = std::numbers::pi;
constexpr std::array<unsigned, 7> channels{0, 1, 2, 4, 5, 6, 7};
constexpr std::array<double, 7> angles{-30, 30, 0, -135, 135, -90, 90};
using Spectrum = std::vector<C>;
using Spectra = std::array<std::array<Spectrum, 2>, 7>;

double bump(double f, double center, double width) {
    if (f <= 0) return 0;
    const double x = std::log2(f / center) / width;
    return std::exp(-0.5 * x * x);
}

// Same generic head-shadow and finite-range assumptions as the legacy backend;
// the game reference deliberately does NOT model room reflections or motion.
V ears(double degrees, double f, double radius, bool physical,
       double distance, unsigned rear_profile, bool wild) {
    const double a = degrees * pi / 180;
    const double lateral = std::sin(a);
    const double abs_a = std::abs(a);
    const double ild = (physical ? 13.0 : wild ? 23.0 : 15.0) * lateral *
                       f * f / (f * f + 720.0 * 720.0);
    double l = std::pow(10.0, -ild / 40.0);
    double r = std::pow(10.0, ild / 40.0);
    const double norm = std::sqrt((l * l + r * r) / 2.0);
    l /= norm;
    r /= norm;
    double itd = std::copysign(radius / 343.0 *
        (abs_a <= pi / 2 ? abs_a + std::sin(abs_a)
                         : pi - abs_a + std::sin(abs_a)), degrees);
    if (!physical && wild) itd *= 1.0 + 0.38 * std::abs(lateral);
    if (physical) {
        const double x = distance * lateral;
        const double y = distance * std::cos(a);
        const double dl = std::hypot(x + radius, y);
        const double dr = std::hypot(x - radius, y);
        itd += (dl - dr - 2 * radius * lateral) / 343.0;
        l *= distance / dl;
        r *= distance / dr;
    } else if (std::abs(degrees) > 110) {
        constexpr std::array<double, 5> n1{4450, 4950, 5500, 6100, 6750};
        constexpr std::array<double, 5> n2{7100, 7750, 8450, 9150, 9900};
        const double amount = wild ? 1.55 : 0.72;
        // Broad, modest reference coloration; extreme bank intentionally
        // exaggerates rear spectral identity, accepting unnatural timbre.
        for (unsigned e = 0; e < 2; ++e) {
            const bool same_side = (e == 0) == (degrees < 0);
            const double shift = same_side ? 0.96 : 1.04;
            const double db = amount *
                (2.0 * bump(f, 1050, 0.65) -
                 6.0 * bump(f, n1[rear_profile] * shift, 0.23) -
                 3.8 * bump(f, n2[rear_profile] / shift, 0.25));
            (e == 0 ? l : r) *= std::pow(10.0, db / 20.0);
        }
    }
    return {std::polar(l, -pi * f * itd), std::polar(r, pi * f * itd)};
}

V multiply(const M& a, const V& x) {
    return {a[0][0] * x[0] + a[0][1] * x[1],
            a[1][0] * x[0] + a[1][1] * x[1]};
}

void fft(Spectrum& x, bool inverse) {
    const auto n = x.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const C root = std::polar(1.0, (inverse ? 2 : -2) * pi / len);
        for (std::size_t i = 0; i < n; i += len) {
            C w = 1;
            for (std::size_t j = 0; j < len / 2; ++j) {
                const C a = x[i+j], b = x[i+j+len/2] * w;
                x[i+j] = a + b;
                x[i+j+len/2] = a - b;
                w *= root;
            }
        }
    }
    if (inverse) for (auto& v : x) v /= static_cast<double>(n);
}

struct Bin {
    M c{}, h{};
    std::array<V, 7> desired{}, b{};
    double lambda = 0, weight = 1;
};

void hermitian(Spectrum& x) {
    x[0] = x[0].real();
    x[x.size()/2] = x[x.size()/2].real();
    for (std::size_t k = 1; k < x.size()/2; ++k)
        x[x.size()-k] = std::conj(x[k]);
}

float approach(float value, float target, float step) {
    return value < target ? std::min(target, value + step)
                         : std::max(target, value - step);
}
} // namespace

GameSpeakerProcessor::GameSpeakerProcessor(const GameSpeakerConfig& config)
    : config_(config) {
    if (!std::isfinite(config.sample_rate) || config.sample_rate < 8000 ||
        config.sample_rate > 192000 ||
        !std::isfinite(config.speaker_span_degrees) || config.speaker_span_degrees < 8 ||
        config.speaker_span_degrees > 60 ||
        !std::isfinite(config.speaker_distance_metres) || config.speaker_distance_metres < .2F ||
        config.speaker_distance_metres > 5 ||
        !std::isfinite(config.head_width_metres) || config.head_width_metres < .13F ||
        config.head_width_metres > .24F)
        throw std::invalid_argument("Invalid fixed-listener game speaker geometry/rate");
    config_.rear_profile = std::min(4U, config_.rear_profile);
    length_ = std::max<std::size_t>(64, std::lround(256 * config.sample_rate / 48000));
    delay_ = std::max<std::size_t>(1, std::lround(.002 * config.sample_rate));
    for (auto& h : history_) h.resize(length_);
    dry_delay_.resize(delay_);
    ramp_step_ = static_cast<float>(1.0 / (.040 * config.sample_rate));
    release_ = static_cast<float>(1.0 - std::exp(-1.0 / (.080 * config.sample_rate)));
    setMix(config.mix);
    setExpansion(config.expansion);
    design();
    reset();
}

void GameSpeakerProcessor::design() {
    std::size_t n = 1;
    while (n < length_ * 4) n <<= 1;
    // Scene covariance weights individual channels plus adjacent panning and
    // coherent centre/rear combinations. No runtime content classification.
    std::array<std::array<double, 7>, 7> scene{};
    for (unsigned c = 0; c < 7; ++c) scene[c][c] = 1;
    constexpr std::array<std::array<unsigned,2>,8> pairs{{
        {0,2},{1,2},{0,5},{1,6},{5,3},{6,4},{3,4},{0,1}}};
    for (auto p : pairs) for (auto a : p) for (auto b : p) scene[a][b] += .15;
    double scene_norm = 0;
    for (const auto& row : scene) {
        double sum = 0;
        for (double value : row) sum += value;
        scene_norm = std::max(scene_norm, sum);
    }

    for (unsigned variant = 0; variant < 2; ++variant) {
        const bool wild = variant == 1;
        auto& bank = wild ? expanded_ : reference_;
        for (auto& h : bank) h.assign(length_, {0,0});
        std::vector<Bin> bins(n/2+1);
        double lipschitz = 0;
        for (std::size_t k = 0; k <= n/2; ++k) {
            const double f = k * config_.sample_rate / n;
            auto& bin = bins[k];
            const auto left = ears(-config_.speaker_span_degrees / 2, f,
                config_.head_width_metres/2, true, config_.speaker_distance_metres, 2, false);
            const auto right = ears(config_.speaker_span_degrees / 2, f,
                config_.head_width_metres/2, true, config_.speaker_distance_metres, 2, false);
            bin.c = {{{left[0], right[0]}, {left[1], right[1]}}};
            bin.weight = .2 + .8 * f*f/(f*f+150.0*150.0);
            bin.lambda = .006 + .15 * std::pow(std::max(0.0, 1-f/350), 2) +
                .04 * std::pow(std::clamp((f-6500)/6500, 0.0, 1.0), 2);
            for (unsigned a = 0; a < 2; ++a) for (unsigned b = 0; b < 2; ++b)
                bin.h[a][b] = bin.weight * (std::conj(bin.c[0][a])*bin.c[0][b] +
                                                           std::conj(bin.c[1][a])*bin.c[1][b]);
            lipschitz = std::max(lipschitz, scene_norm *
                std::max(std::abs(bin.h[0][0])+std::abs(bin.h[0][1]),
                         std::abs(bin.h[1][0])+std::abs(bin.h[1][1])) + bin.lambda);
            for (unsigned c = 0; c < 7; ++c) {
                V d = ears(angles[c], f, config_.head_width_metres/2,
                    false, 1, config_.rear_profile, wild);
                if (wild && angles[c] != 0) {
                    // Research-inspired, NOT a reproduction of Tan et al.'s
                    // complete velocity-vector method. Boost contralateral
                    // cancellation only where the model feeds oppose in phase.
                    const auto& a = bin.c;
                    const C det = a[0][0]*a[1][1]-a[0][1]*a[1][0];
                    if (std::abs(det) > .02) {
                        V g{(a[1][1]*d[0]-a[0][1]*d[1])/det,
                            (-a[1][0]*d[0]+a[0][0]*d[1])/det};
                        if (std::real(g[0]*std::conj(g[1])) < 0) {
                            const double band = f*f/(f*f+220.0*220.0) /
                                (1+std::pow(f/1000, 4));
                            const double role = std::abs(angles[c]) < 60 ? .35 : 1.0;
                            g[angles[c] < 0 ? 1 : 0] *= 1 + .65 * band * role;
                            d = multiply(a, g);
                        }
                    }
                }
                const C shift = std::polar(1.0, -2*pi*f*delay_/config_.sample_rate);
                for (auto& v : d) v *= shift;
                if (k == 0 || k == n/2) for (auto& v : d) v = v.real();
                bin.desired[c] = d;
                for (unsigned a = 0; a < 2; ++a)
                    bin.b[c][a] = bin.weight *
                        (std::conj(bin.c[0][a])*d[0] + std::conj(bin.c[1][a])*d[1]);
            }
        }
        Spectra y{}, x{}, next{};
        for (unsigned c = 0; c < 7; ++c) for (unsigned e = 0; e < 2; ++e) {
            y[c][e].resize(n);
            x[c][e].resize(n);
            next[c][e].resize(n);
        }
        const double step = .95 / lipschitz;
        double t = 1;
        // Projected accelerated least squares over ACTUAL causal FIR support.
        // No post-solve truncation/taper or per-channel renormalization follows.
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
            for (std::size_t k = 0; k <= n/2; ++k) {
                const auto& b = bins[k];
                std::array<V,7> error{};
                for (unsigned c = 0; c < 7; ++c) {
                    error[c] = multiply(b.h, {y[c][0][k],y[c][1][k]});
                    for (unsigned e = 0; e < 2; ++e) error[c][e] -= b.b[c][e];
                }
                for (unsigned c = 0; c < 7; ++c) for (unsigned e = 0; e < 2; ++e) {
                    C gradient = b.lambda * y[c][e][k];
                    for (unsigned j = 0; j < 7; ++j) gradient += scene[c][j]*error[j][e];
                    next[c][e][k] = y[c][e][k] - step * gradient;
                }
            }
            const double next_t = (1+std::sqrt(1+4*t*t))/2;
            for (unsigned c = 0; c < 7; ++c) for (unsigned e = 0; e < 2; ++e) {
                auto& v = next[c][e];
                hermitian(v);
                fft(v, true);
                for (std::size_t j = 0; j < n; ++j)
                    v[j] = j < length_ ? C(v[j].real(),0) : C{};
                fft(v, false);
                for (std::size_t j = 0; j < n; ++j) {
                    y[c][e][j] = v[j] + (t-1)/next_t * (v[j]-x[c][e][j]);
                    x[c][e][j] = v[j];
                }
            }
            t = next_t;
        }
        if (!wild) {
            double error = 0, energy = 0;
            for (std::size_t k = 0; k <= n/2; ++k) for (unsigned c = 0; c < 7; ++c) {
                const auto delivered = multiply(bins[k].c, {x[c][0][k],x[c][1][k]});
                for (unsigned e = 0; e < 2; ++e) {
                    error += bins[k].weight * std::norm(delivered[e]-bins[k].desired[c][e]);
                    energy += bins[k].weight * std::norm(bins[k].desired[c][e]);
                }
            }
            fit_error_ = std::sqrt(error / std::max(energy, 1e-20));
        }
        for (unsigned c = 0; c < 7; ++c) for (unsigned e = 0; e < 2; ++e) {
            fft(x[c][e], true);
            for (std::size_t j = 0; j < length_; ++j)
                bank[channels[c]][j][e] = static_cast<float>(x[c][e][j].real());
        }
        // Finite, unity-DC LFE lowpass with the SAME reference delay in both
        // banks. LFE gain is explicit, not automatically +10 dB.
        double total = 0;
        for (std::size_t j = delay_; j < length_; ++j) {
            const double v = std::exp(-2*pi*120*(j-delay_)/config_.sample_rate);
            total += v;
            bank[3][j] = {static_cast<float>(v),static_cast<float>(v)};
        }
        for (auto& p : bank[3]) for (auto& v : p) v *= static_cast<float>(.22/total);
    }
    // FC must not change when only expansion changes. Joint-scene fitting can
    // otherwise leak the exaggerated neighbors' residual errors into FC.
    expanded_[2] = reference_[2];
    double maximum_energy = 0, maximum_l1 = 0, maximum_peak = 0;
    for (const Bank* bank : {&reference_, &expanded_}) for (unsigned e = 0; e < 2; ++e) {
        double energy = 0, l1 = 0;
        for (unsigned c = 0; c < 8; ++c) {
            Spectrum response(n*2);
            for (std::size_t j = 0; j < length_; ++j) {
                const double h = (*bank)[c][j][e];
                energy += h*h;
                l1 += std::abs(h);
                response[j] = h;
            }
            fft(response, false);
            for (const auto& v : response) maximum_peak = std::max(maximum_peak, std::abs(v));
        }
        maximum_energy = std::max(maximum_energy, energy);
        maximum_l1 = std::max(maximum_l1, l1);
    }
    // Fixed for both endpoints of the expansion control: music cannot steal
    // gain, and changing width does not quietly engage auto gain compensation.
    // This crest allowance is practical, NOT a mathematical no-overload proof.
    gain_ = static_cast<float>(std::min({.22, .80/(3.5*std::sqrt(maximum_energy)),
                                       .70/std::max(maximum_peak, 1e-9)}));
    strict_gain_ = static_cast<float>(.94/std::max(maximum_l1, 3.35));
    max_filter_gain_ = maximum_peak * gain_;
    for (unsigned c = 0; c < 8; ++c) {
        packed_[c].resize(length_);
        for (std::size_t j = 0; j < length_; ++j)
            packed_[c][j] = {reference_[c][j][0],reference_[c][j][1],
                            expanded_[c][j][0],expanded_[c][j][1]};
    }
}

void GameSpeakerProcessor::setMix(float mix) noexcept {
    mix_target_ = std::isfinite(mix) ? std::clamp(mix,0.0F,1.0F) : 1.0F;
}
void GameSpeakerProcessor::setExpansion(float expansion) noexcept {
    expansion_target_ = std::isfinite(expansion) ? std::clamp(expansion/1.5F,0.0F,1.0F) : 0.0F;
}
void GameSpeakerProcessor::reset() noexcept {
    for (auto& h : history_) std::fill(h.begin(),h.end(),0);
    std::fill(dry_delay_.begin(),dry_delay_.end(),SurroundFrame{});
    position_ = dry_position_ = 0;
    mix_ = mix_target_;
    expansion_ = expansion_target_;
    pre_peak_ = 0;
    limiter_ = 1;
}

StereoFrame GameSpeakerProcessor::process(const SurroundFrame& raw) noexcept {
    SurroundFrame input = raw;
    for (auto& x : input) if (!std::isfinite(x)) x = 0;
    mix_ = approach(mix_,mix_target_,ramp_step_);
    expansion_ = approach(expansion_,expansion_target_,ramp_step_);
    for (unsigned c = 0; c < 8; ++c) history_[c][position_] = input[c];
    std::array<float,4> sum{};
#if defined(__SSE2__)
    __m128 accumulator = _mm_setzero_ps();
#endif
    for (unsigned c = 0; c < 8; ++c) {
        // Two contiguous loops avoid per-tap modulo. Float accumulators permit
        // SIMD-friendly code and are sufficient for these bounded short FIRs.
        std::size_t tap = 0;
        const auto accumulate = [&](std::size_t i) {
            const float s = history_[c][i];
#if defined(__SSE2__)
            accumulator = _mm_add_ps(accumulator,
                _mm_mul_ps(_mm_loadu_ps(packed_[c][tap].data()),_mm_set1_ps(s)));
#else
            for (unsigned e = 0; e < 4; ++e) sum[e] += packed_[c][tap][e]*s;
#endif
            ++tap;
        };
        for (std::size_t i = position_+1; i > 0; --i) accumulate(i-1);
        for (std::size_t i = length_; i > position_+1; --i) accumulate(i-1);
    }
#if defined(__SSE2__)
    _mm_storeu_ps(sum.data(),accumulator);
#endif
    const auto [base_l,base_r,wild_l,wild_r] = sum;
    position_ = (position_+1)%length_;
    const auto dry = dry_delay_[dry_position_];
    dry_delay_[dry_position_] = input;
    dry_position_ = (dry_position_+1)%delay_;
    const float dl = dry[0]+.70710678F*dry[2]+.70710678F*(dry[4]+dry[6])+.22F*dry[3];
    const float dr = dry[1]+.70710678F*dry[2]+.70710678F*(dry[5]+dry[7])+.22F*dry[3];
    float l = gain_*(dl+mix_*(base_l+expansion_*(wild_l-base_l)-dl));
    float r = gain_*(dr+mix_*(base_r+expansion_*(wild_r-base_r)-dr));
    pre_peak_ = std::max(std::abs(l),std::abs(r));
    if (!std::isfinite(pre_peak_)) { pre_peak_ = 0; limiter_ = 0; return {}; }
    const float required = pre_peak_ > .95F ? .95F/pre_peak_ : 1.0F;
    limiter_ = required < limiter_ ? required :
        std::min(required,limiter_+release_*(1-limiter_));
    return {l*limiter_,r*limiter_};
}
} // namespace stereo_surround
