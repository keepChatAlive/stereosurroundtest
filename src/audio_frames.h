#pragma once

#include <algorithm>
#include <array>

namespace stereo_surround {

// Canonical Windows 7.1 order after channel-mask mapping:
// FL, FR, FC, LFE, BL, BR, SL, SR.
using SurroundFrame = std::array<float, 8>;

struct StereoFrame {
    float left = 0.0F;
    float right = 0.0F;
};

// Conservative diagnostic fold-down shared by both applications for their
// spatial-vs-naive A/B switch. This is routing infrastructure, not a spatial
// backend algorithm.
[[nodiscard]] inline StereoFrame matrixDownmix(const SurroundFrame& input) {
    constexpr float center = 0.70710678F;
    constexpr float surround = 0.5F;
    constexpr float lfe = 0.25F;
    constexpr float headroom = 0.36F;
    const float left = headroom *
                       (input[0] + center * input[2] + lfe * input[3] +
                        surround * input[4] + surround * input[6]);
    const float right = headroom *
                        (input[1] + center * input[2] + lfe * input[3] +
                         surround * input[5] + surround * input[7]);
    // Preserve the L/R ratio under overload. Independent clipping changes the
    // inter-channel level difference and can pull a virtual image toward the
    // centre during the diagnostic A/B comparison.
    const float peak = std::max(std::abs(left), std::abs(right));
    const float gain = peak > 1.0F ? 1.0F / peak : 1.0F;
    return {left * gain, right * gain};
}

}  // namespace stereo_surround
