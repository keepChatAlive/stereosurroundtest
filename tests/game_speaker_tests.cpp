#include "game_speaker_processor.h"
#include "transaural_processor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

using namespace stereo_surround;
namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
float randomSample(unsigned& seed) {
    seed = 1664525U * seed + 1013904223U;
    return static_cast<float>(static_cast<int>(seed >> 8)-8388608)/8388608;
}
std::vector<StereoFrame> impulse(GameSpeakerProcessor& p, unsigned channel) {
    p.reset();
    std::vector<StereoFrame> result(2048);
    for (unsigned i = 0; i < result.size(); ++i) {
        SurroundFrame input{};
        if (i == 0) input[channel] = .2F;
        result[i] = p.process(input);
    }
    return result;
}
double energy(const std::vector<StereoFrame>& data) {
    double sum = 0;
    for (auto f : data) sum += f.left*f.left+f.right*f.right;
    return sum;
}
void geometryCase(double rate, float distance, float span) {
    GameSpeakerConfig config;
    config.sample_rate = rate;
    config.speaker_distance_metres = distance;
    config.speaker_span_degrees = span;
    const auto begin = std::chrono::steady_clock::now();
    GameSpeakerProcessor p(config);
    std::cout << "rate=" << rate << " distance=" << distance << " span=" << span
              << " fixedGain=" << p.fixedGain() << " strictGain=" << p.strictPeakGain()
              << " fitError=" << p.referenceFitError()
              << " finalMaxFilter=" << p.maximumFilterGain()
              << " designMs=" << std::chrono::duration<double,std::milli>(
                  std::chrono::steady_clock::now()-begin).count() << '\n';
    check(std::isfinite(p.referenceFitError()) && p.referenceFitError() < .45,
          "reference ear fit error unexpectedly large");
    check(p.maximumFilterGain() <= .701, "final filter effort cap");
    check(p.fixedGain() > .01F, "unusable excessive fixed attenuation");
    const auto center = impulse(p,2);
    const auto lfe = impulse(p,3);
    const auto reference_side = impulse(p,6);
    for (float expansion : {0.0F, .75F, 1.5F}) {
        p.setExpansion(expansion);
        for (auto pair : {std::pair{0U,1U}, {4U,5U}, {6U,7U}}) {
            const auto left = impulse(p,pair.first), right = impulse(p,pair.second);
            double error = 0;
            for (unsigned i = 0; i < left.size(); ++i) {
                error += std::pow(left[i].left-right[i].right,2)+
                         std::pow(left[i].right-right[i].left,2);
            }
            check(error < 1e-10, "mirror symmetry");
            check(energy(left) > 1e-6, "directional channel missing");
        }
        const auto expanded_center = impulse(p,2), expanded_lfe = impulse(p,3);
        double center_error = 0, lfe_error = 0;
        for (unsigned i = 0; i < center.size(); ++i) {
            center_error += std::abs(center[i].left-expanded_center[i].left);
            lfe_error += std::abs(lfe[i].left-expanded_lfe[i].left);
        }
        check(center_error < 1e-6 && lfe_error < 1e-6, "expansion changes FC/LFE");
    }
    const auto wild_side = impulse(p,6);
    double expansion_difference = 0;
    for (unsigned i=0;i<wild_side.size();++i)
        expansion_difference += std::pow(wild_side[i].left-reference_side[i].left,2)+
                                std::pow(wild_side[i].right-reference_side[i].right,2);
    check(expansion_difference/energy(reference_side) > .05,"wild expansion ineffective");
    // Coherent adjacent-channel equal-power pans: bounded broad-band energy,
    // no deep hole caused by incompatible electrical filters. Not a substitute
    // for ear-domain/perceptual position measurement.
    for (float expansion : {0.0F,1.5F}) {
        p.setExpansion(expansion);
        for (auto pair : {std::pair{0U,6U}, {6U,4U}, {4U,5U}, {2U,0U}}) {
            const auto a=impulse(p,pair.first), b=impulse(p,pair.second);
            const double floor=std::min(energy(a),energy(b));
            for (unsigned step=0;step<=20;++step) {
                const double angle=step/20.0*1.5707963267948966;
                double pan_energy=0;
                for (unsigned i=0;i<a.size();++i) {
                    const double l=std::cos(angle)*a[i].left+std::sin(angle)*b[i].left;
                    const double r=std::cos(angle)*a[i].right+std::sin(angle)*b[i].right;
                    pan_energy+=l*l+r*r;
                }
                check(pan_energy > .15*floor,"deep coherent-panning energy hole");
            }
        }
    }
    // Known stems permit a genuine nonlinearity test: subtract background-only
    // from the combined run, not just compare L/R energy of the entire mix.
    const unsigned frames = 12000;
    std::vector<SurroundFrame> bg(frames), fx(frames);
    unsigned seed = 12345;
    for (unsigned i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i/rate);
        const float common = .20F*std::sin(2*3.14159265F*233*t);
        for (unsigned c = 0; c < 8; ++c)
            bg[i][c] = common + .34F*randomSample(seed);
        fx[i][6] = .06F*std::sin(2*3.14159265F*1531*t);
    }
    p.setExpansion(1.5F);
    p.reset();
    std::vector<StereoFrame> background(frames), effect(frames);
    for (unsigned i=0;i<frames;++i) background[i]=p.process(bg[i]);
    p.reset();
    for (unsigned i=0;i<frames;++i) effect[i]=p.process(fx[i]);
    p.reset();
    double error=0, norm=0;
    float minimum_gain=1, maximum_peak=0;
    const auto run_start=std::chrono::steady_clock::now();
    for (unsigned i=0;i<frames;++i) {
        auto input=bg[i];
        for (unsigned c=0;c<8;++c) input[c]+=fx[i][c];
        const auto out=p.process(input);
        minimum_gain=std::min(minimum_gain,p.limiterGain());
        maximum_peak=std::max(maximum_peak,p.preLimiterPeak());
        error+=std::pow(out.left-background[i].left-effect[i].left,2)+
               std::pow(out.right-background[i].right-effect[i].right,2);
        norm+=effect[i].left*effect[i].left+effect[i].right*effect[i].right;
    }
    const double speed=(frames/rate)/std::chrono::duration<double>(
        std::chrono::steady_clock::now()-run_start).count();
    std::cout << "  wild busy mix peak=" << maximum_peak << " limiter=" << minimum_gain
              << " effect residual=" << std::sqrt(error/norm) << " realtime=" << speed << "x\n";
    check(minimum_gain == 1, "routine synthetic busy mix hits limiter");
    check(std::sqrt(error/norm)<.0003, "music changes separate effect response");
    p.reset();
    float full_peak=0, full_gain=1;
    for (unsigned i=0;i<frames;++i) {
        auto input=bg[i];
        for (auto& s:input) s/=.54F;
        (void)p.process(input);
        full_peak=std::max(full_peak,p.preLimiterPeak());
        full_gain=std::min(full_gain,p.limiterGain());
    }
    std::cout << "  unit-peak busy bed: peak=" << full_peak << " limiter=" << full_gain << '\n';
    check(full_gain==1,"unit-peak synthetic bed unexpectedly limits");

    // Exercise overload and control ramps, no NaN/inf escapes, no allocation
    // needed by the processing method. This is not a listening test.
    for (unsigned i=0;i<8000;++i) {
        if (i%800==0) { p.setExpansion(i%1600 ? 1.5F : 0); p.setMix(i%2400 ? 1 : 0); }
        SurroundFrame input{};
        for (auto& s:input) s=4*randomSample(seed);
        if (i==4) input[6]=std::numeric_limits<float>::quiet_NaN();
        const auto out=p.process(input);
        check(std::isfinite(out.left)&&std::isfinite(out.right),"non-finite output");
        check(std::max(std::abs(out.left),std::abs(out.right))<.95001F,"overload ceiling");
    }
    p.reset();
    for (unsigned i=0;i<2000;++i) {
        const auto out=p.process({});
        check(out.left==0&&out.right==0,"reset tail");
    }
}
}
int main() {
    geometryCase(48000, .8F, 40);
    geometryCase(48000, .3F, 35);
    geometryCase(24000, .3F, 35);
    geometryCase(48000, .2F, 8);
    geometryCase(48000, .8F, 60);
    // Portable facade used by the endpoint and WAV paths reaches the new DSP.
    TransauralConfig facade_config;
    facade_config.profile=TransauralProfile::game;
    TransauralDownmixer facade(facade_config);
    check(facade.filterLength()==256&&facade.latencySamples()==96,"facade routing");
    check(facade.comparisonGain()>0&&facade.comparisonGain()<1,"naive gain alignment");
    GameSpeakerConfig invalid;
    invalid.sample_rate=std::numeric_limits<double>::quiet_NaN();
    bool rejected=false;
    try { GameSpeakerProcessor bad(invalid); } catch (const std::invalid_argument&) { rejected=true; }
    check(rejected,"NaN configuration accepted");
    std::cout << "Game speaker tests passed\n";
}
