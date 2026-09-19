#include "wasapi_bridge.h"

#include "transaural_processor.h"

#include <Windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <xmmintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace stereo_surround {
namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void throwHr(const char* operation, HRESULT result) {
    std::ostringstream stream;
    stream << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(result) << ')';
    throw std::runtime_error(stream.str());
}

void checkHr(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throwHr(operation, result);
    }
}

class DynamicTransauralProcessor {
public:
    DynamicTransauralProcessor(const TransauralConfig& config,
                               BridgeRuntimeControls& controls)
        : base_config_(config),
          active_generation_(controls.geometry_generation.load(
              std::memory_order_acquire)),
          crossfade_step_(1.0F /
                          std::max(1.0F, static_cast<float>(config.sample_rate) *
                                             0.080F)) {
        // Initial setup happens before the endpoint starts, but the caller is
        // already an MMCSS thread. Keep the longer game-bank optimization on a
        // below-normal worker even at startup. Snapshot generation BEFORE its
        // fields, so a geometry edit during design is not accidentally marked
        // applied by the first poll.
        TransauralConfig initial = config;
        initial.speaker_span_degrees = controls.speaker_span_degrees.load(std::memory_order_relaxed);
        initial.physical_speaker_distance_metres = controls.speaker_distance_metres.load(std::memory_order_relaxed);
        initial.head_width_metres = controls.head_width_metres.load(std::memory_order_relaxed);
        active_ = std::async(std::launch::async,[initial] {
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
            return std::make_unique<TransauralDownmixer>(initial);
        }).get();
    }

    void poll(BridgeRuntimeControls& controls) {
        const float strength =
            controls.transaural_strength.load(std::memory_order_relaxed);
        const float side_width =
            controls.transaural_side_width.load(std::memory_order_relaxed);
        active_->setStrength(strength);
        active_->setSideWidth(side_width);
        if (incoming_ != nullptr) {
            incoming_->setStrength(strength);
            incoming_->setSideWidth(side_width);
        }

        const std::uint32_t requested_generation =
            controls.geometry_generation.load(std::memory_order_acquire);
        if (build_.valid() &&
            build_.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            std::unique_ptr<TransauralDownmixer> built = build_.get();
            if (building_generation_ == requested_generation) {
                built->setStrength(strength);
                built->setSideWidth(side_width);
                incoming_ = std::move(built);
                crossfade_mix_ = 0.0F;
            }
        }
        if (!build_.valid() && incoming_ == nullptr &&
            requested_generation != active_generation_) {
            TransauralConfig requested = base_config_;
            requested.speaker_span_degrees =
                controls.speaker_span_degrees.load(std::memory_order_relaxed);
            requested.physical_speaker_distance_metres =
                controls.speaker_distance_metres.load(
                    std::memory_order_relaxed);
            requested.head_width_metres =
                controls.head_width_metres.load(std::memory_order_relaxed);
            requested.strength = strength;
            requested.side_width = side_width;
            building_generation_ = requested_generation;
            build_ = std::async(std::launch::async, [requested] {
                // Filter design is deliberately kept away from the two MMCSS
                // audio threads. It may use a full core briefly after Apply.
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                return std::make_unique<TransauralDownmixer>(requested);
            });
        }
    }

    [[nodiscard]] StereoFrame process(const SurroundFrame& input) {
        const StereoFrame current = active_->process(input);
        if (incoming_ == nullptr) {
            return current;
        }
        const StereoFrame replacement = incoming_->process(input);
        crossfade_mix_ = std::min(1.0F, crossfade_mix_ + crossfade_step_);
        const StereoFrame output = {
            current.left + crossfade_mix_ * (replacement.left - current.left),
            current.right +
                crossfade_mix_ * (replacement.right - current.right)};
        if (crossfade_mix_ >= 1.0F) {
            active_ = std::move(incoming_);
            active_generation_ = building_generation_;
        }
        return output;
    }

    [[nodiscard]] std::size_t latencySamples() const noexcept {
        return active_->latencySamples();
    }

    [[nodiscard]] float preLimiterPeak() const noexcept {
        return incoming_ ? std::max(active_->preLimiterPeak(),incoming_->preLimiterPeak())
                         : active_->preLimiterPeak();
    }
    [[nodiscard]] float limiterGain() const noexcept {
        return incoming_ ? std::min(active_->limiterGain(),incoming_->limiterGain())
                         : active_->limiterGain();
    }
    [[nodiscard]] float comparisonGain() const noexcept {
        return incoming_ ? active_->comparisonGain() + crossfade_mix_ *
            (incoming_->comparisonGain()-active_->comparisonGain())
                         : active_->comparisonGain();
    }

private:
    TransauralConfig base_config_{};
    std::unique_ptr<TransauralDownmixer> active_;
    std::unique_ptr<TransauralDownmixer> incoming_;
    std::future<std::unique_ptr<TransauralDownmixer>> build_;
    std::uint32_t active_generation_ = 0;
    std::uint32_t building_generation_ = 0;
    float crossfade_mix_ = 0.0F;
    float crossfade_step_ = 1.0F;
};

void publishSpatialMonitor(BridgeMonitor* monitor, float peak, float gain) {
    if (!monitor) return;
    float old = monitor->spatial_pre_peak.load(std::memory_order_relaxed);
    while (old < peak && !monitor->spatial_pre_peak.compare_exchange_weak(
        old,peak,std::memory_order_relaxed)) {}
    old = monitor->spatial_limiter_gain.load(std::memory_order_relaxed);
    while (old > gain && !monitor->spatial_limiter_gain.compare_exchange_weak(
        old,gain,std::memory_order_relaxed)) {}
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

struct CoTaskMemFormatDeleter {
    void operator()(WAVEFORMATEX* format) const noexcept { CoTaskMemFree(format); }
};
using UniqueWaveFormat = std::unique_ptr<WAVEFORMATEX, CoTaskMemFormatDeleter>;

class MmcssRegistration {
public:
    MmcssRegistration() {
        // Prevent long-decaying IIR/FIR tails from falling into costly
        // subnormal arithmetic on the real-time audio threads.
#if defined(__SSE__)
        _mm_setcsr(_mm_getcsr() | 0x8040U);  // flush-to-zero + denormals-are-zero
#endif
        DWORD task_index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        if (handle_ != nullptr) {
            // This is relative priority within the MMCSS Pro Audio task, not
            // Windows' dangerous TIME_CRITICAL base priority. It needs no
            // administrator token and remains subject to MMCSS scheduling.
            if (AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL) != FALSE) {
                return;
            }
        }
        // Safe best-effort fallback when MMCSS registration is unavailable.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    ~MmcssRegistration() {
        if (handle_ != nullptr) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }

private:
    HANDLE handle_ = nullptr;
};

class ThreadComApartment {
public:
    ThreadComApartment() {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE) {
            throwHr("CoInitializeEx", result_);
        }
    }
    ~ThreadComApartment() {
        if (result_ == S_OK || result_ == S_FALSE) {
            CoUninitialize();
        }
    }

private:
    HRESULT result_ = E_FAIL;
};

enum class SampleEncoding {
    float32,
    float64,
    pcm8,
    pcm16,
    pcm24,
    pcm32,
};

struct AudioFormat {
    SampleEncoding encoding = SampleEncoding::float32;
    unsigned channels = 0;
    unsigned sample_rate = 0;
    unsigned block_align = 0;
    unsigned bytes_per_sample = 0;
    unsigned valid_bits = 0;
    DWORD channel_mask = 0;
};

[[nodiscard]] AudioFormat parseFormat(const WAVEFORMATEX& format) {
    AudioFormat result;
    result.channels = format.nChannels;
    result.sample_rate = format.nSamplesPerSec;
    result.block_align = format.nBlockAlign;
    if (result.channels == 0 || result.block_align == 0 ||
        result.block_align % result.channels != 0) {
        throw std::runtime_error("Endpoint returned an invalid audio block layout");
    }
    result.bytes_per_sample = result.block_align / result.channels;
    result.valid_bits = format.wBitsPerSample;

    WORD tag = format.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            throw std::runtime_error("Truncated WAVEFORMATEXTENSIBLE endpoint format");
        }
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        result.valid_bits = extensible.Samples.wValidBitsPerSample;
        result.channel_mask = extensible.dwChannelMask;
        if (IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            tag = WAVE_FORMAT_PCM;
        } else {
            throw std::runtime_error("Endpoint format is neither PCM nor IEEE float");
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (result.bytes_per_sample == sizeof(float)) {
            result.encoding = SampleEncoding::float32;
        } else if (result.bytes_per_sample == sizeof(double)) {
            result.encoding = SampleEncoding::float64;
        } else {
            throw std::runtime_error("Unsupported floating-point container size");
        }
    } else if (tag == WAVE_FORMAT_PCM) {
        switch (result.bytes_per_sample) {
            case 1:
                result.encoding = SampleEncoding::pcm8;
                break;
            case 2:
                result.encoding = SampleEncoding::pcm16;
                break;
            case 3:
                result.encoding = SampleEncoding::pcm24;
                break;
            case 4:
                result.encoding = SampleEncoding::pcm32;
                break;
            default:
                throw std::runtime_error("Unsupported PCM container size");
        }
    } else {
        throw std::runtime_error("Endpoint format is neither PCM nor IEEE float");
    }
    return result;
}

[[nodiscard]] float decodeSample(const BYTE* frame, unsigned channel,
                                 const AudioFormat& format) {
    const BYTE* sample = frame + channel * format.bytes_per_sample;
    switch (format.encoding) {
        case SampleEncoding::float32: {
            float value = 0.0F;
            std::memcpy(&value, sample, sizeof(value));
            return std::isfinite(value) ? value : 0.0F;
        }
        case SampleEncoding::float64: {
            double value = 0.0;
            std::memcpy(&value, sample, sizeof(value));
            return std::isfinite(value) ? static_cast<float>(value) : 0.0F;
        }
        case SampleEncoding::pcm8:
            return (static_cast<int>(*sample) - 128) / 128.0F;
        case SampleEncoding::pcm16: {
            std::int16_t value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return static_cast<float>(value) / 32768.0F;
        }
        case SampleEncoding::pcm24: {
            std::int32_t value = static_cast<std::int32_t>(sample[0]) |
                                 (static_cast<std::int32_t>(sample[1]) << 8) |
                                 (static_cast<std::int32_t>(sample[2]) << 16);
            if ((value & 0x00800000) != 0) {
                value |= static_cast<std::int32_t>(0xFF000000);
            }
            return static_cast<float>(value) / 8'388'608.0F;
        }
        case SampleEncoding::pcm32: {
            std::int32_t value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return static_cast<float>(static_cast<double>(value) / 2'147'483'648.0);
        }
    }
    return 0.0F;
}

void encodeSample(BYTE* frame, unsigned channel, const AudioFormat& format,
                  float input) {
    BYTE* sample = frame + channel * format.bytes_per_sample;
    const float value = std::clamp(input, -1.0F, 1.0F);
    switch (format.encoding) {
        case SampleEncoding::float32:
            std::memcpy(sample, &value, sizeof(value));
            return;
        case SampleEncoding::float64: {
            const double converted = value;
            std::memcpy(sample, &converted, sizeof(converted));
            return;
        }
        case SampleEncoding::pcm8: {
            const auto converted = static_cast<unsigned char>(
                std::clamp(std::lround(value * 127.0F + 128.0F), 0L, 255L));
            *sample = converted;
            return;
        }
        case SampleEncoding::pcm16: {
            const auto converted = static_cast<std::int16_t>(
                std::clamp(std::lround(value * 32767.0F), -32768L, 32767L));
            std::memcpy(sample, &converted, sizeof(converted));
            return;
        }
        case SampleEncoding::pcm24: {
            const auto converted = static_cast<std::int32_t>(std::clamp(
                std::llround(static_cast<double>(value) * 8'388'607.0),
                -8'388'608LL, 8'388'607LL));
            sample[0] = static_cast<BYTE>(converted & 0xFF);
            sample[1] = static_cast<BYTE>((converted >> 8) & 0xFF);
            sample[2] = static_cast<BYTE>((converted >> 16) & 0xFF);
            return;
        }
        case SampleEncoding::pcm32: {
            std::int64_t wide = std::clamp(
                std::llround(static_cast<double>(value) * 2'147'483'647.0),
                -2'147'483'648LL, 2'147'483'647LL);
            if (format.valid_bits > 0 && format.valid_bits < 32) {
                const unsigned shift = 32U - format.valid_bits;
                wide = (wide >> shift) << shift;
            }
            const auto converted = static_cast<std::int32_t>(wide);
            std::memcpy(sample, &converted, sizeof(converted));
            return;
        }
    }
}

[[nodiscard]] int channelIndexFromMask(DWORD mask, DWORD speaker) {
    if ((mask & speaker) == 0) {
        return -1;
    }
    int index = 0;
    for (DWORD bit = 1; bit != 0 && bit < speaker; bit <<= 1U) {
        if ((mask & bit) != 0) {
            ++index;
        }
    }
    return index;
}

[[nodiscard]] std::array<int, 8> buildCanonicalChannelMap(const AudioFormat& format) {
    if (format.channels != 8) {
        throw std::runtime_error(
            "The capture endpoint must be configured for exactly 8 channels (7.1)");
    }

    if (format.channel_mask == 0) {
        return {0, 1, 2, 3, 4, 5, 6, 7};
    }

    constexpr std::array<DWORD, 8> required = {
        SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER,
        SPEAKER_LOW_FREQUENCY, SPEAKER_BACK_LEFT, SPEAKER_BACK_RIGHT,
        SPEAKER_SIDE_LEFT, SPEAKER_SIDE_RIGHT};
    std::array<int, 8> mapping{};
    for (std::size_t index = 0; index < required.size(); ++index) {
        mapping[index] = channelIndexFromMask(format.channel_mask, required[index]);
        if (mapping[index] < 0) {
            throw std::runtime_error(
                "The capture endpoint is not standard 7.1 Surround "
                "(required order/mask: FL FR FC LFE BL BR SL SR)");
        }
    }
    return mapping;
}

class StereoRingBuffer {
public:
    explicit StereoRingBuffer(std::size_t capacity)
        : data_(std::max<std::size_t>(capacity, 4)) {}

    [[nodiscard]] bool push(const StereoFrame& frame) {
        const std::uint64_t write = write_.load(std::memory_order_relaxed);
        const std::uint64_t read = read_.load(std::memory_order_acquire);
        if (write - read >= data_.size()) {
            return false;
        }
        data_[static_cast<std::size_t>(write % data_.size())] = frame;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool peek(std::size_t offset, StereoFrame& frame) const {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_.load(std::memory_order_acquire);
        if (read + offset >= write) {
            return false;
        }
        frame = data_[static_cast<std::size_t>((read + offset) % data_.size())];
        return true;
    }

    [[nodiscard]] bool discardOne() {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_.load(std::memory_order_acquire);
        if (read >= write) {
            return false;
        }
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size() const {
        // Read the consumer cursor first. Loading write first can pair an old
        // write value with a newer read value and unsigned-underflow while the
        // render thread is advancing concurrently.
        const std::uint64_t read = read_.load(std::memory_order_acquire);
        const std::uint64_t write = write_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(write - read);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return data_.size(); }

    void trimTo(std::size_t remaining_frames) {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_.load(std::memory_order_acquire);
        const std::uint64_t available = write - read;
        if (available > remaining_frames) {
            read_.store(write - remaining_frames, std::memory_order_release);
        }
    }

private:
    std::vector<StereoFrame> data_;
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};

struct WavAudio {
    AudioFormat format;
    std::vector<float> mono_samples;
};

[[nodiscard]] std::uint32_t readLe32(const BYTE* value) {
    return static_cast<std::uint32_t>(value[0]) |
           (static_cast<std::uint32_t>(value[1]) << 8U) |
           (static_cast<std::uint32_t>(value[2]) << 16U) |
           (static_cast<std::uint32_t>(value[3]) << 24U);
}

[[nodiscard]] WavAudio loadWavFile(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open the selected WAV file");
    }

    std::array<BYTE, 12> riff{};
    file.read(reinterpret_cast<char*>(riff.data()), static_cast<std::streamsize>(riff.size()));
    if (file.gcount() != static_cast<std::streamsize>(riff.size()) ||
        std::memcmp(riff.data(), "RIFF", 4) != 0 ||
        std::memcmp(riff.data() + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("The selected file is not a RIFF/WAVE file");
    }

    std::vector<BYTE> format_bytes;
    std::vector<BYTE> audio_bytes;
    constexpr std::uint32_t maximum_chunk_size = 512U * 1024U * 1024U;
    while (file && (format_bytes.empty() || audio_bytes.empty())) {
        std::array<BYTE, 8> header{};
        file.read(reinterpret_cast<char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
        if (file.gcount() != static_cast<std::streamsize>(header.size())) {
            break;
        }
        const std::uint32_t chunk_size = readLe32(header.data() + 4);
        if (chunk_size > maximum_chunk_size) {
            throw std::runtime_error("WAV chunk is too large for this demo");
        }

        if (std::memcmp(header.data(), "fmt ", 4) == 0) {
            format_bytes.assign(chunk_size, 0);
            file.read(reinterpret_cast<char*>(format_bytes.data()), chunk_size);
        } else if (std::memcmp(header.data(), "data", 4) == 0) {
            audio_bytes.assign(chunk_size, 0);
            file.read(reinterpret_cast<char*>(audio_bytes.data()), chunk_size);
        } else {
            file.seekg(chunk_size, std::ios::cur);
        }
        if (!file) {
            throw std::runtime_error("WAV file ended inside a chunk");
        }
        if ((chunk_size & 1U) != 0U) {
            file.seekg(1, std::ios::cur);
        }
    }

    if (format_bytes.size() < 16 || audio_bytes.empty()) {
        throw std::runtime_error("WAV file is missing its format or audio-data chunk");
    }
    std::array<BYTE, sizeof(WAVEFORMATEXTENSIBLE)> storage{};
    std::memcpy(storage.data(), format_bytes.data(),
                std::min(storage.size(), format_bytes.size()));
    auto* wave_format = reinterpret_cast<WAVEFORMATEX*>(storage.data());
    if (format_bytes.size() == 16) {
        wave_format->cbSize = 0;
    }
    const AudioFormat format = parseFormat(*wave_format);
    if (format.sample_rate == 0 || format.channels == 0 || format.block_align == 0 ||
        audio_bytes.size() % format.block_align != 0) {
        throw std::runtime_error("WAV file has an invalid sample layout");
    }

    const std::size_t frame_count = audio_bytes.size() / format.block_align;
    std::vector<float> mono(frame_count, 0.0F);
    for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        const BYTE* frame = audio_bytes.data() + frame_index * format.block_align;
        double sum = 0.0;
        for (unsigned channel = 0; channel < format.channels; ++channel) {
            sum += decodeSample(frame, channel, format);
        }
        mono[frame_index] = static_cast<float>(sum / format.channels);
    }
    return {format, std::move(mono)};
}

[[nodiscard]] ComPtr<IMMDeviceEnumerator> makeEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    checkHr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&enumerator)),
            "CoCreateInstance(MMDeviceEnumerator)");
    return enumerator;
}

[[nodiscard]] std::vector<EndpointInfo> enumerateEndpoints(EDataFlow flow) {
    const auto enumerator = makeEnumerator();
    ComPtr<IMMDeviceCollection> collection;
    checkHr(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection),
            "EnumAudioEndpoints");
    UINT count = 0;
    checkHr(collection->GetCount(&count), "IMMDeviceCollection::GetCount");

    std::vector<EndpointInfo> endpoints;
    endpoints.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        checkHr(collection->Item(index, &device), "IMMDeviceCollection::Item");

        LPWSTR raw_id = nullptr;
        checkHr(device->GetId(&raw_id), "IMMDevice::GetId");
        std::wstring id(raw_id);
        CoTaskMemFree(raw_id);

        ComPtr<IPropertyStore> properties;
        checkHr(device->OpenPropertyStore(STGM_READ, &properties),
                "IMMDevice::OpenPropertyStore");
        PROPVARIANT value;
        PropVariantInit(&value);
        checkHr(properties->GetValue(PKEY_Device_FriendlyName, &value),
                "IPropertyStore::GetValue(PKEY_Device_FriendlyName)");
        std::wstring name = value.vt == VT_LPWSTR && value.pwszVal != nullptr
                                ? value.pwszVal
                                : L"(unnamed endpoint)";
        PropVariantClear(&value);
        endpoints.push_back({std::move(id), std::move(name)});
    }
    return endpoints;
}

struct AudioClientBundle {
    ComPtr<IAudioClient> client;
    UniqueWaveFormat wave_format;
    AudioFormat format;
    UniqueHandle event;
    UINT32 buffer_frames = 0;
};

[[nodiscard]] AudioClientBundle openAudioClient(const EndpointInfo& endpoint,
                                                bool capture,
                                                unsigned requested_sample_rate = 0) {
    const auto enumerator = makeEnumerator();
    ComPtr<IMMDevice> device;
    checkHr(enumerator->GetDevice(endpoint.id.c_str(), &device),
            "IMMDeviceEnumerator::GetDevice");

    AudioClientBundle bundle;
    checkHr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             &bundle.client),
            "IMMDevice::Activate(IAudioClient)");
    WAVEFORMATEX* raw_format = nullptr;
    checkHr(bundle.client->GetMixFormat(&raw_format), "IAudioClient::GetMixFormat");
    bundle.wave_format.reset(raw_format);
    bundle.format = parseFormat(*bundle.wave_format);

    if (capture && bundle.format.channels != 8) {
        std::ostringstream message;
        message << "Capture endpoint exposes " << bundle.format.channels
                << " channels; configure the virtual endpoint as 7.1 first";
        throw std::runtime_error(message.str());
    }
    if (!capture && bundle.format.channels != 2) {
        std::ostringstream message;
        message << "Render endpoint exposes " << bundle.format.channels
                << " channels; this demo requires a stereo speaker endpoint";
        throw std::runtime_error(message.str());
    }

    DWORD stream_flags =
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    if (requested_sample_rate != 0 &&
        bundle.wave_format->nSamplesPerSec != requested_sample_rate) {
        bundle.wave_format->nSamplesPerSec = requested_sample_rate;
        bundle.wave_format->nAvgBytesPerSec =
            requested_sample_rate * bundle.wave_format->nBlockAlign;
        stream_flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        bundle.format = parseFormat(*bundle.wave_format);
    }

    checkHr(bundle.client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                      0, 0, bundle.wave_format.get(), nullptr),
            requested_sample_rate == 0
                ? "IAudioClient::Initialize(shared/event-driven)"
                : "IAudioClient::Initialize(24 kHz shared/autoconvert)");
    bundle.event = UniqueHandle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (bundle.event.get() == nullptr) {
        throw std::runtime_error("CreateEventW failed");
    }
    checkHr(bundle.client->SetEventHandle(bundle.event.get()),
            "IAudioClient::SetEventHandle");
    checkHr(bundle.client->GetBufferSize(&bundle.buffer_frames),
            "IAudioClient::GetBufferSize");
    return bundle;
}

void captureWorker(AudioClientBundle bundle, const BridgeOptions& options,
                   BridgeRuntimeControls& controls, StereoRingBuffer& ring,
                   BridgeMonitor& monitor,
                   std::atomic_bool& stop_requested,
                   std::exception_ptr& worker_error, std::mutex& error_mutex) {
    try {
        ThreadComApartment apartment;
        MmcssRegistration mmcss;
        ComPtr<IAudioCaptureClient> capture;
        checkHr(bundle.client->GetService(IID_PPV_ARGS(&capture)),
                "IAudioClient::GetService(IAudioCaptureClient)");
        const auto channel_map = buildCanonicalChannelMap(bundle.format);
        const ProcessingMode spatial_mode = options.spatial_mode;
        std::unique_ptr<DynamicTransauralProcessor> transaural;
        if (spatial_mode != ProcessingMode::transaural_natural &&
            spatial_mode != ProcessingMode::transaural_hyper &&
            spatial_mode != ProcessingMode::transaural_game) {
            throw std::invalid_argument(
                "Transaural application received a non-transaural spatial mode");
        }
        {
            TransauralConfig config;
            config.sample_rate = bundle.format.sample_rate;
            config.profile = spatial_mode == ProcessingMode::transaural_game
                                 ? TransauralProfile::game
                                 : spatial_mode == ProcessingMode::transaural_hyper
                                 ? TransauralProfile::aggressive
                                 : TransauralProfile::natural;
            config.speaker_span_degrees = options.speaker_span_degrees;
            config.physical_speaker_distance_metres =
                options.speaker_distance_metres;
            config.head_width_metres = options.head_width_metres;
            config.rear_profile = options.transaural_rear_profile;
            config.strength = options.transaural_strength;
            config.side_width = options.transaural_side_width;
            transaural = std::make_unique<DynamicTransauralProcessor>(
                config, controls);
        }
        const std::size_t comparison_latency = transaural->latencySamples();
        // Keep the naive side of a transaural A/B comparison aligned with the
        // backend's small causal design delay. This prevents a mode switch from
        // also becoming a timing comparison or creating a crossfade comb.
        std::vector<StereoFrame> comparison_delay(comparison_latency);
        std::size_t comparison_delay_position = 0;
        float spatial_mix =
            controls.mode.load(std::memory_order_relaxed) ==
                    ProcessingMode::matrix
                ? 0.0F
                : 1.0F;
        // The selected spatial backend keeps advancing while naive mode is
        // heard, so its FIR/delay state is ready when the user switches back.
        // A short crossfade avoids an A/B button click.
        constexpr float kModeCrossfadeSeconds = 0.020F;
        const float mode_crossfade_step =
            1.0F / std::max(1.0F, static_cast<float>(bundle.format.sample_rate) *
                                      kModeCrossfadeSeconds);
        constexpr float kAllocatedChannelGain = 1.0F / 8.0F;
        constexpr float kHeadroomRampSeconds = 0.020F;
        float channel_headroom_gain = options.fixed_channel_headroom
                                          ? kAllocatedChannelGain
                                          : 1.0F;
        const float headroom_ramp_step =
            (1.0F - kAllocatedChannelGain) /
            std::max(1.0F, static_cast<float>(bundle.format.sample_rate) *
                               kHeadroomRampSeconds);
        checkHr(bundle.client->Start(), "capture IAudioClient::Start");
        while (!stop_requested.load(std::memory_order_relaxed)) {
            const DWORD wait = WaitForSingleObject(bundle.event.get(), 200);
            if (wait == WAIT_TIMEOUT) {
                continue;
            }
            if (wait != WAIT_OBJECT_0) {
                throw std::runtime_error("Capture event wait failed");
            }

            UINT32 packet_frames = 0;
            checkHr(capture->GetNextPacketSize(&packet_frames),
                    "IAudioCaptureClient::GetNextPacketSize");
            while (packet_frames > 0) {
                transaural->poll(controls);
                const float target_spatial_mix =
                    controls.mode.load(std::memory_order_relaxed) ==
                            ProcessingMode::matrix
                        ? 0.0F
                        : 1.0F;
                const float target_channel_headroom_gain =
                    controls.fixed_channel_headroom.load(
                        std::memory_order_relaxed)
                        ? kAllocatedChannelGain
                        : 1.0F;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                checkHr(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr),
                        "IAudioCaptureClient::GetBuffer");
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    monitor.input_discontinuities.fetch_add(
                        1, std::memory_order_relaxed);
                }

                std::array<float, 8> packet_peaks{};
                float spatial_peak = 0, spatial_gain = 1;

                for (UINT32 frame_index = 0; frame_index < frames; ++frame_index) {
                    SurroundFrame surround{};
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                        const BYTE* source = data +
                                             static_cast<std::size_t>(frame_index) *
                                                 bundle.format.block_align;
                        for (std::size_t channel = 0; channel < surround.size(); ++channel) {
                            surround[channel] = decodeSample(
                                source, static_cast<unsigned>(channel_map[channel]),
                                bundle.format);
                            packet_peaks[channel] = std::max(
                                packet_peaks[channel], std::abs(surround[channel]));
                        }
                    }

                    if (spatial_mix < target_spatial_mix) {
                        spatial_mix = std::min(target_spatial_mix,
                                               spatial_mix + mode_crossfade_step);
                    } else if (spatial_mix > target_spatial_mix) {
                        spatial_mix = std::max(target_spatial_mix,
                                               spatial_mix - mode_crossfade_step);
                    }
                    if (channel_headroom_gain < target_channel_headroom_gain) {
                        channel_headroom_gain = std::min(
                            target_channel_headroom_gain,
                            channel_headroom_gain + headroom_ramp_step);
                    } else if (channel_headroom_gain >
                               target_channel_headroom_gain) {
                        channel_headroom_gain = std::max(
                            target_channel_headroom_gain,
                            channel_headroom_gain - headroom_ramp_step);
                    }
                    SurroundFrame allocated = surround;
                    for (float& sample : allocated) {
                        sample *= channel_headroom_gain;
                    }
                    const StereoFrame spatial = transaural->process(allocated);
                    spatial_peak = std::max(spatial_peak,transaural->preLimiterPeak());
                    spatial_gain = std::min(spatial_gain,transaural->limiterGain());
                    StereoFrame matrix{};
                    bool matrix_ready = false;
                    if (!comparison_delay.empty()) {
                        matrix = comparison_delay[comparison_delay_position];
                        comparison_delay[comparison_delay_position] =
                            matrixDownmix(allocated);
                        comparison_delay_position =
                            (comparison_delay_position + 1U) %
                            comparison_delay.size();
                        matrix_ready = true;
                    }
                    StereoFrame stereo = spatial;
                    if (matrix_ready) {
                        matrix.left *= transaural->comparisonGain();
                        matrix.right *= transaural->comparisonGain();
                    }
                    if (spatial_mix < 1.0F || target_spatial_mix < 1.0F) {
                        if (!matrix_ready) {
                            matrix = matrixDownmix(allocated);
                            matrix.left *= transaural->comparisonGain();
                            matrix.right *= transaural->comparisonGain();
                        }
                        stereo = {
                            matrix.left +
                                spatial_mix * (spatial.left - matrix.left),
                            matrix.right +
                                spatial_mix * (spatial.right - matrix.right)};
                    }
                    if (!ring.push(stereo)) {
                        monitor.input_overflows.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
                for (std::size_t channel = 0; channel < packet_peaks.size();
                     ++channel) {
                    monitor.input_peaks[channel].store(
                        packet_peaks[channel], std::memory_order_relaxed);
                }
                monitor.captured_frames.fetch_add(frames,
                                                  std::memory_order_relaxed);
                monitor.queue_frames.store(ring.size(),
                                           std::memory_order_relaxed);
                publishSpatialMonitor(&monitor,spatial_peak,spatial_gain);
                checkHr(capture->ReleaseBuffer(frames),
                        "IAudioCaptureClient::ReleaseBuffer");
                checkHr(capture->GetNextPacketSize(&packet_frames),
                        "IAudioCaptureClient::GetNextPacketSize");
            }
        }
        bundle.client->Stop();
    } catch (...) {
        {
            std::lock_guard lock(error_mutex);
            if (worker_error == nullptr) {
                worker_error = std::current_exception();
            }
        }
        stop_requested.store(true, std::memory_order_relaxed);
    }
}

void renderWorker(AudioClientBundle bundle, unsigned input_sample_rate,
                  BridgeRuntimeControls& controls, StereoRingBuffer& ring,
                  BridgeMonitor& monitor, std::atomic_bool& stop_requested,
                  std::exception_ptr& worker_error, std::mutex& error_mutex) {
    try {
        ThreadComApartment apartment;
        MmcssRegistration mmcss;
        ComPtr<IAudioRenderClient> render;
        checkHr(bundle.client->GetService(IID_PPV_ARGS(&render)),
                "IAudioClient::GetService(IAudioRenderClient)");

        BYTE* initial = nullptr;
        checkHr(render->GetBuffer(bundle.buffer_frames, &initial),
                "IAudioRenderClient::GetBuffer(initial)");
        checkHr(render->ReleaseBuffer(bundle.buffer_frames,
                                      AUDCLNT_BUFFERFLAGS_SILENT),
                "IAudioRenderClient::ReleaseBuffer(initial)");
        checkHr(bundle.client->Start(), "render IAudioClient::Start");

        const double nominal_ratio = static_cast<double>(input_sample_rate) /
                                     static_cast<double>(bundle.format.sample_rate);
        double phase = 0.0;
        bool primed = false;
        std::uint32_t seen_resync =
            controls.resync_generation.load(std::memory_order_relaxed);

        while (!stop_requested.load(std::memory_order_relaxed)) {
            const DWORD wait = WaitForSingleObject(bundle.event.get(), 200);
            if (wait == WAIT_TIMEOUT) {
                continue;
            }
            if (wait != WAIT_OBJECT_0) {
                throw std::runtime_error("Render event wait failed");
            }

            UINT32 padding = 0;
            checkHr(bundle.client->GetCurrentPadding(&padding),
                    "IAudioClient::GetCurrentPadding");
            const UINT32 available = bundle.buffer_frames - padding;
            if (available == 0) {
                continue;
            }

            BYTE* destination = nullptr;
            checkHr(render->GetBuffer(available, &destination),
                    "IAudioRenderClient::GetBuffer");

            const unsigned requested_latency_ms = std::clamp(
                controls.target_latency_ms.load(std::memory_order_relaxed), 10U,
                250U);
            const std::size_t target_frames = std::clamp<std::size_t>(
                static_cast<std::size_t>(input_sample_rate) *
                    requested_latency_ms / 1000U,
                2U, ring.capacity() - 2U);
            monitor.target_frames.store(target_frames,
                                        std::memory_order_relaxed);

            const std::uint32_t requested_resync =
                controls.resync_generation.load(std::memory_order_relaxed);
            if (requested_resync != seen_resync) {
                ring.trimTo(target_frames);
                phase = 0.0;
                primed = false;
                seen_resync = requested_resync;
                monitor.resyncs.fetch_add(1, std::memory_order_relaxed);
            }

            const std::size_t fill = ring.size();
            if (!primed && fill >= target_frames) {
                primed = true;
            }
            const double normalized_error =
                target_frames == 0
                    ? 0.0
                    : (static_cast<double>(fill) - static_cast<double>(target_frames)) /
                          static_cast<double>(target_frames);
            const double drift_correction =
                std::clamp(normalized_error * 0.0005, -0.0025, 0.0025);
            const double ratio = nominal_ratio * (1.0 + drift_correction);
            monitor.resample_percent.store(100.0 * ratio / nominal_ratio,
                                           std::memory_order_relaxed);
            bool buffer_underrun = false;

            for (UINT32 output_index = 0; output_index < available; ++output_index) {
                StereoFrame output{};
                StereoFrame first{};
                StereoFrame second{};
                if (primed && ring.peek(0, first) && ring.peek(1, second)) {
                    const float fraction = static_cast<float>(phase);
                    output.left = first.left + (second.left - first.left) * fraction;
                    output.right = first.right + (second.right - first.right) * fraction;
                    phase += ratio;
                    while (phase >= 1.0) {
                        if (!ring.discardOne()) {
                            primed = false;
                            buffer_underrun = true;
                            phase = 0.0;
                            break;
                        }
                        phase -= 1.0;
                    }
                } else {
                    if (primed) {
                        buffer_underrun = true;
                    }
                    primed = false;
                    phase = 0.0;
                }

                BYTE* frame = destination +
                              static_cast<std::size_t>(output_index) *
                                  bundle.format.block_align;
                encodeSample(frame, 0, bundle.format, output.left);
                encodeSample(frame, 1, bundle.format, output.right);
            }
            if (buffer_underrun) {
                monitor.output_underruns.fetch_add(1,
                                                   std::memory_order_relaxed);
            }
            monitor.rendered_frames.fetch_add(available,
                                              std::memory_order_relaxed);
            monitor.queue_frames.store(ring.size(),
                                       std::memory_order_relaxed);
            monitor.primed.store(primed, std::memory_order_relaxed);
            checkHr(render->ReleaseBuffer(available, 0),
                    "IAudioRenderClient::ReleaseBuffer");
        }
        bundle.client->Stop();
    } catch (...) {
        {
            std::lock_guard lock(error_mutex);
            if (worker_error == nullptr) {
                worker_error = std::current_exception();
            }
        }
        stop_requested.store(true, std::memory_order_relaxed);
    }
}

}  // namespace

void BridgeMonitor::reset() noexcept {
    spatial_pre_peak.store(0,std::memory_order_relaxed);
    spatial_limiter_gain.store(1,std::memory_order_relaxed);
    input_sample_rate.store(0, std::memory_order_relaxed);
    output_sample_rate.store(0, std::memory_order_relaxed);
    input_channels.store(0, std::memory_order_relaxed);
    output_channels.store(0, std::memory_order_relaxed);
    captured_frames.store(0, std::memory_order_relaxed);
    rendered_frames.store(0, std::memory_order_relaxed);
    input_overflows.store(0, std::memory_order_relaxed);
    output_underruns.store(0, std::memory_order_relaxed);
    input_discontinuities.store(0, std::memory_order_relaxed);
    resyncs.store(0, std::memory_order_relaxed);
    queue_frames.store(0, std::memory_order_relaxed);
    queue_capacity_frames.store(0, std::memory_order_relaxed);
    target_frames.store(0, std::memory_order_relaxed);
    resample_percent.store(100.0, std::memory_order_relaxed);
    primed.store(false, std::memory_order_relaxed);
    for (auto& peak : input_peaks) {
        peak.store(0.0F, std::memory_order_relaxed);
    }
}

std::vector<EndpointInfo> enumerateCaptureEndpoints() {
    return enumerateEndpoints(eCapture);
}

std::vector<EndpointInfo> enumerateRenderEndpoints() {
    return enumerateEndpoints(eRender);
}

void runWasapiBridge(const EndpointInfo& input, const EndpointInfo& output,
                     const BridgeOptions& options,
                     BridgeRuntimeControls& controls, BridgeMonitor& monitor,
                     std::atomic_bool& stop_requested) {
    monitor.reset();
    const unsigned requested_rate = options.use_24khz_stream ? 24'000U : 0U;
    AudioClientBundle capture = openAudioClient(input, true, requested_rate);
    AudioClientBundle render = openAudioClient(output, false, requested_rate);
    (void)buildCanonicalChannelMap(capture.format);

    const std::size_t ring_frames = std::max<std::size_t>(
        capture.format.sample_rate,
        static_cast<std::size_t>(capture.format.sample_rate) *
            options.ring_capacity_ms / 1000U);
    StereoRingBuffer ring(ring_frames);
    monitor.input_sample_rate.store(capture.format.sample_rate,
                                    std::memory_order_relaxed);
    monitor.output_sample_rate.store(render.format.sample_rate,
                                     std::memory_order_relaxed);
    monitor.input_channels.store(capture.format.channels,
                                 std::memory_order_relaxed);
    monitor.output_channels.store(render.format.channels,
                                  std::memory_order_relaxed);
    monitor.queue_capacity_frames.store(ring.capacity(),
                                        std::memory_order_relaxed);
    std::exception_ptr worker_error;
    std::mutex error_mutex;

    const unsigned input_rate = capture.format.sample_rate;
    std::jthread capture_thread(
        captureWorker, std::move(capture), std::cref(options),
        std::ref(controls), std::ref(ring), std::ref(monitor),
        std::ref(stop_requested), std::ref(worker_error), std::ref(error_mutex));
    std::jthread render_thread(
        renderWorker, std::move(render), input_rate, std::ref(controls),
        std::ref(ring), std::ref(monitor), std::ref(stop_requested),
        std::ref(worker_error), std::ref(error_mutex));

    while (!stop_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    capture_thread.join();
    render_thread.join();
    if (worker_error != nullptr) {
        std::rethrow_exception(worker_error);
    }
}

void playChannelTestFile(const std::wstring& path, std::size_t channel,
                         const EndpointInfo& output, const BridgeOptions& options,
                         BridgeRuntimeControls& controls,
                         std::atomic_bool& stop_requested,
                         BridgeMonitor* monitor) {
    if (monitor) monitor->reset();
    if (channel >= SurroundFrame{}.size()) {
        throw std::invalid_argument("Test channel index is outside the 7.1 layout");
    }
    const WavAudio wav = loadWavFile(path);
    MmcssRegistration mmcss;
    const unsigned requested_rate = options.use_24khz_stream ? 24'000U : 0U;
    AudioClientBundle render_bundle =
        openAudioClient(output, false, requested_rate);
    std::unique_ptr<DynamicTransauralProcessor> transaural;
    if (options.mode != ProcessingMode::matrix) {
        if (options.mode != ProcessingMode::transaural_natural &&
            options.mode != ProcessingMode::transaural_hyper &&
            options.mode != ProcessingMode::transaural_game) {
            throw std::invalid_argument(
                "Transaural application received an unsupported test mode");
        }
        TransauralConfig config;
        config.sample_rate = render_bundle.format.sample_rate;
        config.profile = options.mode == ProcessingMode::transaural_game
                             ? TransauralProfile::game
                             : options.mode == ProcessingMode::transaural_hyper
                             ? TransauralProfile::aggressive
                             : TransauralProfile::natural;
        config.speaker_span_degrees = options.speaker_span_degrees;
        config.physical_speaker_distance_metres =
            options.speaker_distance_metres;
        config.head_width_metres = options.head_width_metres;
        config.rear_profile = options.transaural_rear_profile;
        config.strength = options.transaural_strength;
        config.side_width = options.transaural_side_width;
        transaural = std::make_unique<DynamicTransauralProcessor>(
            config, controls);
    }

    ComPtr<IAudioRenderClient> render;
    checkHr(render_bundle.client->GetService(IID_PPV_ARGS(&render)),
            "IAudioClient::GetService(IAudioRenderClient)");

    BYTE* initial = nullptr;
    checkHr(render->GetBuffer(render_bundle.buffer_frames, &initial),
            "IAudioRenderClient::GetBuffer(test initial)");
    checkHr(render->ReleaseBuffer(render_bundle.buffer_frames,
                                  AUDCLNT_BUFFERFLAGS_SILENT),
            "IAudioRenderClient::ReleaseBuffer(test initial)");
    checkHr(render_bundle.client->Start(), "test render IAudioClient::Start");

    const double source_step = static_cast<double>(wav.format.sample_rate) /
                               static_cast<double>(render_bundle.format.sample_rate);
    const std::uint64_t audio_output_frames = static_cast<std::uint64_t>(std::ceil(
        static_cast<double>(wav.mono_samples.size()) / source_step));
    const std::uint64_t tail_frames = render_bundle.format.sample_rate / 4U;
    const std::uint64_t total_frames = audio_output_frames + tail_frames;
    std::uint64_t output_position = 0;
    constexpr float kAllocatedChannelGain = 1.0F / 8.0F;
    constexpr float kHeadroomRampSeconds = 0.020F;
    float channel_headroom_gain =
        options.fixed_channel_headroom ? kAllocatedChannelGain : 1.0F;
    const float headroom_ramp_step =
        (1.0F - kAllocatedChannelGain) /
        std::max(1.0F,
                 static_cast<float>(render_bundle.format.sample_rate) *
                     kHeadroomRampSeconds);

    while (!stop_requested.load(std::memory_order_relaxed) &&
           output_position < total_frames) {
        const DWORD wait = WaitForSingleObject(render_bundle.event.get(), 200);
        if (wait == WAIT_TIMEOUT) {
            continue;
        }
        if (wait != WAIT_OBJECT_0) {
            throw std::runtime_error("Test render event wait failed");
        }

        UINT32 padding = 0;
        checkHr(render_bundle.client->GetCurrentPadding(&padding),
                "IAudioClient::GetCurrentPadding(test)");
        const UINT32 available = render_bundle.buffer_frames - padding;
        if (available == 0) {
            continue;
        }
        BYTE* destination = nullptr;
        checkHr(render->GetBuffer(available, &destination),
                "IAudioRenderClient::GetBuffer(test)");

        if (transaural != nullptr) {
            transaural->poll(controls);
        }
        float spatial_peak = 0, spatial_gain = 1;

        for (UINT32 frame_index = 0; frame_index < available; ++frame_index) {
            SurroundFrame surround{};
            if (output_position < audio_output_frames && !wav.mono_samples.empty()) {
                const double source_position =
                    static_cast<double>(output_position) * source_step;
                const std::size_t first_index = static_cast<std::size_t>(source_position);
                const std::size_t second_index =
                    std::min(first_index + 1, wav.mono_samples.size() - 1);
                const float fraction =
                    static_cast<float>(source_position - first_index);
                if (first_index < wav.mono_samples.size()) {
                    surround[channel] =
                        wav.mono_samples[first_index] +
                        (wav.mono_samples[second_index] - wav.mono_samples[first_index]) *
                            fraction;
                }
            }
            const float target_channel_headroom_gain =
                controls.fixed_channel_headroom.load(std::memory_order_relaxed)
                    ? kAllocatedChannelGain
                    : 1.0F;
            if (channel_headroom_gain < target_channel_headroom_gain) {
                channel_headroom_gain = std::min(
                    target_channel_headroom_gain,
                    channel_headroom_gain + headroom_ramp_step);
            } else if (channel_headroom_gain > target_channel_headroom_gain) {
                channel_headroom_gain = std::max(
                    target_channel_headroom_gain,
                    channel_headroom_gain - headroom_ramp_step);
            }
            for (float& sample : surround) {
                sample *= channel_headroom_gain;
            }
            StereoFrame stereo{};
            if (transaural != nullptr) {
                stereo = transaural->process(surround);
                spatial_peak = std::max(spatial_peak,transaural->preLimiterPeak());
                spatial_gain = std::min(spatial_gain,transaural->limiterGain());
            } else {
                stereo = matrixDownmix(surround);
            }
            BYTE* frame = destination +
                          static_cast<std::size_t>(frame_index) *
                              render_bundle.format.block_align;
            encodeSample(frame, 0, render_bundle.format, stereo.left);
            encodeSample(frame, 1, render_bundle.format, stereo.right);
            ++output_position;
        }
        publishSpatialMonitor(monitor,spatial_peak,spatial_gain);
        checkHr(render->ReleaseBuffer(available, 0),
                "IAudioRenderClient::ReleaseBuffer(test)");
    }
    render_bundle.client->Stop();
}

}  // namespace stereo_surround
