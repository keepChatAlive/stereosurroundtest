#include "transaural_processor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using stereo_surround::StereoFrame;
using stereo_surround::SurroundFrame;
using stereo_surround::TransauralConfig;
using stereo_surround::TransauralDownmixer;
using stereo_surround::TransauralProfile;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct Energy {
    double left = 0.0;
    double right = 0.0;
};

Energy impulseEnergy(std::size_t channel, TransauralProfile profile,
                     unsigned rear_profile = 2, float strength = 1.0F,
                     float side_width = 1.5F) {
    TransauralConfig config;
    config.profile = profile;
    config.rear_profile = rear_profile;
    config.strength = strength;
    config.side_width = side_width;
    TransauralDownmixer processor(config);
    Energy energy;
    for (int frame = 0; frame < 4'096; ++frame) {
        SurroundFrame input{};
        if (frame == 0) {
            input[channel] = 1.0F;
        }
        const StereoFrame output = processor.process(input);
        require(std::isfinite(output.left) && std::isfinite(output.right),
                "transaural output must remain finite");
        energy.left += output.left * output.left;
        energy.right += output.right * output.right;
    }
    return energy;
}

double impulseDifference(std::size_t channel, unsigned first_profile,
                         unsigned second_profile) {
    TransauralConfig first_config;
    first_config.profile = TransauralProfile::aggressive;
    first_config.rear_profile = first_profile;
    first_config.strength = 1.0F;
    TransauralConfig second_config = first_config;
    second_config.rear_profile = second_profile;
    TransauralDownmixer first(first_config);
    TransauralDownmixer second(second_config);
    double difference = 0.0;
    for (int frame = 0; frame < 4'096; ++frame) {
        SurroundFrame input{};
        if (frame == 0) {
            input[channel] = 1.0F;
        }
        const StereoFrame a = first.process(input);
        const StereoFrame b = second.process(input);
        const double left = a.left - b.left;
        const double right = a.right - b.right;
        difference += left * left + right * right;
    }
    return difference;
}

double distanceImpulseDifference(std::size_t channel, float first_distance,
                                 float second_distance) {
    TransauralConfig first_config;
    first_config.profile = TransauralProfile::aggressive;
    first_config.strength = 1.0F;
    first_config.side_width = 0.0F;
    first_config.physical_speaker_distance_metres = first_distance;
    TransauralConfig second_config = first_config;
    second_config.physical_speaker_distance_metres = second_distance;
    TransauralDownmixer first(first_config);
    TransauralDownmixer second(second_config);
    double difference = 0.0;
    for (int frame = 0; frame < 4'096; ++frame) {
        SurroundFrame input{};
        if (frame == 0) {
            input[channel] = 1.0F;
        }
        const StereoFrame a = first.process(input);
        const StereoFrame b = second.process(input);
        difference += (a.left - b.left) * (a.left - b.left) +
                      (a.right - b.right) * (a.right - b.right);
    }
    return difference;
}

}  // namespace

int main() {
    TransauralConfig config;
    TransauralDownmixer processor(config);
    require(processor.filterLength() >= 64,
            "transaural FIR bank must have a useful filter length");
    require(processor.latencySamples() > 0,
            "transaural inverse must have a causal bulk delay");
    require(processor.strength() == 1.25F,
            "hyper transaural strength must retain the aggressive default");
    require(processor.sideWidth() == 1.5F,
            "side anchor must retain the aggressive default");

    SurroundFrame silence{};
    for (int frame = 0; frame < 5'000; ++frame) {
        const StereoFrame output = processor.process(silence);
        require(output.left == 0.0F && output.right == 0.0F,
                "silence must remain exact silence");
    }

    processor.setStrength(-1.0F);
    require(processor.strength() == 0.0F, "strength must clamp at zero");
    processor.setStrength(3.0F);
    require(processor.strength() == 1.5F, "strength must clamp at 150 percent");
    processor.setSideWidth(-1.0F);
    require(processor.sideWidth() == 0.0F, "side width must clamp at zero");
    processor.setSideWidth(3.0F);
    require(processor.sideWidth() == 1.5F,
            "side width must clamp at 150 percent");

    for (std::size_t channel = 0; channel < 8; ++channel) {
        const Energy energy =
            impulseEnergy(channel, TransauralProfile::aggressive);
        require(energy.left + energy.right > 1.0e-8,
                "every 7.1 channel must reach the transaural output");
        if (channel == 0 || channel == 4 || channel == 6) {
            require(energy.left > energy.right,
                    "left virtual channels must favor the left output");
        }
        if (channel == 1 || channel == 5 || channel == 7) {
            require(energy.right > energy.left,
                    "right virtual channels must favor the right output");
        }
    }

    const Energy hyper_side =
        impulseEnergy(6, TransauralProfile::aggressive, 2, 1.0F);
    const Energy natural_side =
        impulseEnergy(6, TransauralProfile::natural, 2, 1.0F);
    require(hyper_side.left / std::max(hyper_side.right, 1.0e-12) >
                natural_side.left / std::max(natural_side.right, 1.0e-12),
            "hyper mode must increase side-channel output contrast");

    const Energy pure_xtc_side =
        impulseEnergy(6, TransauralProfile::aggressive, 2, 1.0F, 0.0F);
    require(hyper_side.left / std::max(hyper_side.right, 1.0e-12) >
                pure_xtc_side.left /
                    std::max(pure_xtc_side.right, 1.0e-12),
            "side anchor must increase same-side physical output dominance");

    for (const float side_width : {0.0F, 0.5F, 1.0F, 1.5F}) {
        const Energy side_left = impulseEnergy(
            6, TransauralProfile::aggressive, 2, 1.25F, side_width);
        const Energy side_right = impulseEnergy(
            7, TransauralProfile::aggressive, 2, 1.25F, side_width);
        const double scale = std::max(
            {side_left.left, side_left.right, side_right.left,
             side_right.right, 1.0e-12});
        require(std::abs(side_left.left - side_right.right) < scale * 1.0e-5 &&
                    std::abs(side_left.right - side_right.left) < scale * 1.0e-5,
                "SL and SR must remain exact stereo-energy mirrors at every "
                "side-width setting");
    }

    require(impulseDifference(4, 0, 4) > 1.0e-6,
            "rear profiles must produce measurably different responses");
    require(distanceImpulseDifference(6, 0.40F, 1.0F) > 1.0e-8,
            "near-field speaker distance must alter the designed response");
    try {
        TransauralConfig invalid_distance;
        invalid_distance.physical_speaker_distance_metres = 0.10F;
        TransauralDownmixer invalid(invalid_distance);
        static_cast<void>(invalid);
        require(false, "too-small physical speaker distance must be rejected");
    } catch (const std::invalid_argument&) {
    }

    TransauralDownmixer full_scale(config);
    SurroundFrame loud{};
    loud.fill(1.0F);
    for (int frame = 0; frame < 10'000; ++frame) {
        const StereoFrame output = full_scale.process(loud);
        require(std::isfinite(output.left) && std::isfinite(output.right),
                "full-scale output must remain finite");
        require(std::abs(output.left) <= 1.0F && std::abs(output.right) <= 1.0F,
                "stereo-linked limiter must bound transaural output");
    }

    // Compare a low-level linear reference with the same independent
    // multichannel signal at full scale. Once overload protection engages,
    // the high-level stereo vector must remain parallel to the reference
    // vector: common gain may change loudness, but not L/R spatial ratio.
    TransauralConfig limiter_config;
    limiter_config.profile = TransauralProfile::aggressive;
    limiter_config.strength = 1.25F;
    limiter_config.side_width = 0.0F;
    TransauralDownmixer low_level(limiter_config);
    TransauralDownmixer high_level(limiter_config);
    std::uint32_t random = 0xC001D00DU;
    bool limiter_engaged = false;
    double maximum_direction_error = 0.0;
    for (int frame = 0; frame < 48'000; ++frame) {
        SurroundFrame unit{};
        for (float& sample : unit) {
            random = random * 1'664'525U + 1'013'904'223U;
            sample = static_cast<float>(static_cast<std::int32_t>(random)) /
                     2'147'483'648.0F;
        }
        SurroundFrame quiet = unit;
        for (float& sample : quiet) {
            sample *= 0.1F;
        }
        const StereoFrame reference = low_level.process(quiet);
        const StereoFrame limited = high_level.process(unit);
        const double expected_left = reference.left * 10.0;
        const double expected_right = reference.right * 10.0;
        const double expected_norm = std::hypot(expected_left, expected_right);
        const double limited_norm = std::hypot(limited.left, limited.right);
        if (std::max(std::abs(expected_left), std::abs(expected_right)) > 0.95) {
            limiter_engaged = true;
        }
        if (expected_norm > 0.1 && limited_norm > 1.0e-8) {
            const double cross = std::abs(limited.left * expected_right -
                                          limited.right * expected_left);
            maximum_direction_error =
                std::max(maximum_direction_error,
                         cross / (expected_norm * limited_norm));
        }
    }
    require(limiter_engaged,
            "multichannel regression signal must engage output protection");
    require(maximum_direction_error < 1.0e-4,
            "linked limiting must preserve the instantaneous stereo direction; error=" +
                std::to_string(maximum_direction_error));

    std::cout << "All transaural processor tests passed.\n";
    return 0;
}
