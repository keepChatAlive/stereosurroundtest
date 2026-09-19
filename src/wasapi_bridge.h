#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace stereo_surround {

struct EndpointInfo {
    std::wstring id;
    std::wstring name;
};

enum class ProcessingMode {
    transaural_natural,
    transaural_hyper,
    transaural_game,
    matrix,
};

struct BridgeOptions {
    ProcessingMode mode = ProcessingMode::transaural_hyper;
    // The spatial side of the live spatial-vs-naive A/B comparison. This is
    // also populated when mode initially starts on the naive matrix.
    ProcessingMode spatial_mode = ProcessingMode::transaural_hyper;
    float transaural_strength = 1.25F;
    float transaural_side_width = 0.0F;
    float speaker_span_degrees = 20.0F;
    // Radial distance from the listener's head centre to either physical
    // speaker. Used by the near-field physical-speaker model.
    float speaker_distance_metres = 1.0F;
    float head_width_metres = 0.18F;
    unsigned transaural_rear_profile = 2;
    // Requests a 24 kHz shared-mode client format for both capture and render.
    // Windows converts between this stream and each endpoint's mix format.
    bool use_24khz_stream = false;
    // Applies an equal 1/8 (-18.06 dB) allocation to every virtual input
    // channel before either spatial backend or the diagnostic matrix.
    bool fixed_channel_headroom = false;
    unsigned target_latency_ms = 35;
    unsigned ring_capacity_ms = 2'000;
};

struct BridgeRuntimeControls {
    std::atomic<ProcessingMode> mode{ProcessingMode::transaural_hyper};
    std::atomic<float> transaural_strength{1.25F};
    std::atomic<float> transaural_side_width{0.0F};
    std::atomic<float> speaker_span_degrees{20.0F};
    std::atomic<float> speaker_distance_metres{1.0F};
    std::atomic<float> head_width_metres{0.18F};
    std::atomic_bool fixed_channel_headroom{false};
    std::atomic<std::uint32_t> geometry_generation{0};
    std::atomic<unsigned> target_latency_ms{35};
    std::atomic<std::uint32_t> resync_generation{0};
};

struct BridgeMonitor {
    std::atomic<unsigned> input_sample_rate{0};
    std::atomic<unsigned> output_sample_rate{0};
    std::atomic<unsigned> input_channels{0};
    std::atomic<unsigned> output_channels{0};
    std::atomic<std::uint64_t> captured_frames{0};
    std::atomic<std::uint64_t> rendered_frames{0};
    std::atomic<std::uint64_t> input_overflows{0};
    std::atomic<std::uint64_t> output_underruns{0};
    std::atomic<std::uint64_t> input_discontinuities{0};
    std::atomic<std::uint64_t> resyncs{0};
    std::atomic<std::size_t> queue_frames{0};
    std::atomic<std::size_t> queue_capacity_frames{0};
    std::atomic<std::size_t> target_frames{0};
    std::atomic<double> resample_percent{100.0};
    std::atomic_bool primed{false};
    std::array<std::atomic<float>, 8> input_peaks{};
    // Max/min since UI last consumed these; includes limiter release, not just
    // samples exactly at the ceiling. Spatial-path readings during naive A/B.
    std::atomic<float> spatial_pre_peak{0.0F};
    std::atomic<float> spatial_limiter_gain{1.0F};

    void reset() noexcept;
};

[[nodiscard]] std::vector<EndpointInfo> enumerateCaptureEndpoints();
[[nodiscard]] std::vector<EndpointInfo> enumerateRenderEndpoints();

// Blocks until stop_requested becomes true or an audio worker fails.
// The caller must initialize COM before calling this function.
void runWasapiBridge(const EndpointInfo& input, const EndpointInfo& output,
                     const BridgeOptions& options,
                     BridgeRuntimeControls& controls, BridgeMonitor& monitor,
                     std::atomic_bool& stop_requested);

// Decodes a PCM/IEEE-float WAV selected by the user, injects its mono fold-down
// into one canonical 7.1 channel (0=FL, 1=FR, 2=FC, 3=LFE, 4=BL, 5=BR,
// 6=SL, 7=SR), processes it, and plays the stereo result to output.
void playChannelTestFile(const std::wstring& path, std::size_t channel,
                         const EndpointInfo& output, const BridgeOptions& options,
                         BridgeRuntimeControls& controls,
                         std::atomic_bool& stop_requested,
                         BridgeMonitor* monitor = nullptr);

}  // namespace stereo_surround
