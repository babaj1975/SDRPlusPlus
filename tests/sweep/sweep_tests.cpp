#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <imgui.h>
#include <json.hpp>

// Reproduce Windows SDK function-like macros after the standard headers.
#ifdef SWEEP_TEST_WINDOWS_MACROS
#define min(a, b) windows_min_macro_must_not_expand(a, b)
#define max(a, b) windows_max_macro_must_not_expand(a, b)
#endif
#include "../../source_modules/sweep/sweep_controller.h"
#ifdef SWEEP_TEST_WINDOWS_MACROS
#undef min
#undef max
#endif

static void check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

static std::vector<int16_t> tone(int count, double cyclesPerSample, double amplitude = 0.5) {
    std::vector<int16_t> iq(count * 2);
    for (int i = 0; i < count; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * cyclesPerSample * i;
        iq[2 * i] = static_cast<int16_t>(std::round(amplitude * 32767.0 * std::cos(phase)));
        iq[2 * i + 1] = static_cast<int16_t>(std::round(amplitude * 32767.0 * std::sin(phase)));
    }
    return iq;
}

static void testPlan() {
    sweep::Settings settings;
    settings.startMHz = 100;
    settings.stopMHz = 110;
    settings.stepMHz = 100; // Deliberately wider than the receiver bandwidth.
    sweep::Plan plan;
    check(plan.configure(settings, 4e6, 2e6), "valid sweep rejected");
    check(std::abs(plan.stepHz - 1.6e6) < 1e-6, "step must use narrower RF bandwidth");
    double coveredUntil = plan.startHz;
    for (double center : plan.centers) {
        check(center - plan.usefulHz / 2 <= coveredUntil + 1e-6, "uncovered frequency gap");
        coveredUntil = center + plan.usefulHz / 2;
    }
    check(std::abs(coveredUntil - plan.stopHz) < 1e-6, "last segment misses stop edge");
    settings.stopMHz = 100.1;
    check(plan.configure(settings, 4e6, 4e6), "narrow sweep rejected");
    check(plan.centers.size() == 1 && std::abs(plan.centers[0] - 100.05e6) < 1e-6, "single-center sweep incorrect");
    settings.stopMHz = settings.startMHz;
    check(!plan.configure(settings, 4e6, 4e6), "zero span accepted");
    settings.stopMHz = 110;
    settings.stepMHz = std::numeric_limits<double>::quiet_NaN();
    check(!plan.configure(settings, 4e6, 4e6), "NaN step accepted");
    settings.stepMHz = 1e-12;
    check(!plan.configure(settings, 4e6, 4e6), "unbounded tuning plan accepted");
    settings.stepMHz = 1;
    settings.dwellMs = -1;
    check(!plan.configure(settings, 4e6, 4e6), "negative dwell accepted");
}

static void testToneAndPasses() {
    sweep::Spectrum spectrum;
    spectrum.configure(99.6e6, 100.4e6, 1e6, 0.8e6, 2048);
    auto iq = tone(2048, 0.125);
    check(spectrum.addIQ(100e6, iq.data(), 2048), "tone rejected");
    spectrum.completeSweep();
    auto snap = spectrum.snapshot();
    const auto peak = std::max_element(snap.spectrum.begin(), snap.spectrum.end());
    const double frequency = snap.startHz + (peak - snap.spectrum.begin() + 0.5) *
        (snap.stopHz - snap.startHz) / snap.spectrum.size();
    check(std::abs(frequency - 100.125e6) < 2000, "FFT positive frequency is mirrored or shifted");
    check(std::abs(*peak + 6.0206f) < 0.1f, "Hann normalization incorrect");
    check(snap.passes == 1 && snap.waterfall.size() == 1, "pass did not commit");
    check(snap.spectrum == snap.waterfall.back(), "completed spectrum disappears at wrap");
    spectrum.completeSweep();
    check(spectrum.snapshot().passes == 1, "empty pass committed");
    iq = tone(2048, -0.125);
    spectrum.addIQ(100e6, iq.data(), 2048);
    spectrum.completeSweep();
    snap = spectrum.snapshot();
    const auto negativePeak = std::max_element(snap.spectrum.begin(), snap.spectrum.end());
    const double negativeFrequency = snap.startHz + (negativePeak - snap.spectrum.begin() + 0.5) *
        (snap.stopHz - snap.startHz) / snap.spectrum.size();
    check(std::abs(negativeFrequency - 99.875e6) < 2000, "FFT negative frequency is mirrored");
    for (int i = 0; i < 80; ++i) {
        spectrum.addIQ(100e6, iq.data(), 2048);
        spectrum.completeSweep();
    }
    check(spectrum.snapshot().waterfall.size() == 64, "waterfall history is unbounded");
    spectrum.configure(1e6, 1.1e6, 1e6, 0.8e6, 1024);
    check(spectrum.snapshot().passes == 0 && spectrum.snapshot().waterfall.empty(), "restart retained stale history");
    check(!spectrum.addIQ(1.05e6, nullptr, 2048), "null IQ accepted");
    check(!spectrum.addIQ(1.05e6, iq.data(), 255), "short IQ block accepted");
}

static void testCoverage() {
    sweep::Settings settings;
    settings.startMHz = 100;
    settings.stopMHz = 100.93;
    settings.stepMHz = 1;
    sweep::Plan plan;
    check(plan.configure(settings, 256000, 256000), "coverage plan failed");
    sweep::Spectrum spectrum;
    spectrum.configure(plan.startHz, plan.stopHz, 256000, plan.usefulHz, 2048, 8192);
    std::vector<int16_t> noise(4096);
    std::uint32_t random = 42;
    for (auto& sample : noise) {
        random = random * 1664525u + 1013904223u;
        sample = static_cast<int16_t>(static_cast<int>(random >> 16) - 32768);
    }
    for (double center : plan.centers) { spectrum.addIQ(center, noise.data(), 2048); }
    spectrum.completeSweep();
    const auto snap = spectrum.snapshot();
    check(std::all_of(snap.spectrum.begin(), snap.spectrum.end(), [](float db) {
        return std::isfinite(db) && db > sweep::Spectrum::floorDb;
    }), "edge cropping left unmeasured display bins");
}

static void testRetuneAndSettling() {
    sweep::Settings settings;
    settings.enabled = true;
    settings.startMHz = 100;
    settings.stopMHz = 101;
    settings.dwellMs = 1;
    settings.settleMs = 2;
    sweep::Controller controller(settings);
    check(controller.begin(1e6, 1e6, 1024, false), "controller start failed");
    check(controller.needsTune(), "first tune missing");
    const double first = controller.nextCenter();
    const auto iq = tone(1024, 0.125);
    controller.consume(iq.data(), 1024);
    check(controller.needsTune(), "IQ accepted before hardware tune");
    controller.tuned(2);
    // Two old queue blocks, then two settling blocks. None counts as dwell.
    for (int i = 0; i < 4; ++i) {
        controller.consume(iq.data(), 1024);
        check(!controller.needsTune(), "old or unsettled IQ advanced the sweep");
    }
    controller.consume(iq.data(), 1024);
    check(controller.needsTune() && controller.nextCenter() > first, "valid dwell did not advance");
    controller.tuned(0);
    for (int i = 0; i < 3; ++i) { controller.consume(iq.data(), 1024); }
    check(controller.needsTune() && controller.nextCenter() == first, "sweep did not wrap");
    settings.stopMHz = 100.1;
    settings.settleMs = 0;
    sweep::Controller narrow(settings);
    check(narrow.begin(1e6, 1e6, 1024, false), "single center start failed");
    narrow.tuned(0);
    narrow.consume(iq.data(), 1024);
    check(narrow.needsTune(), "single-center pass never completes");
    check(controller.begin(1e6, 1e6, 1024, true) && !controller.active(), "headless server enabled local sweep");
    controller.load(controller.save());
    check(controller.begin(1e6, 1e6, 1024, false) && !controller.active(), "config load enabled sweep unexpectedly");
}

static void testConcurrentSnapshots() {
    sweep::Spectrum spectrum;
    spectrum.configure(99.6e6, 100.4e6, 1e6, 0.8e6, 2048);
    const auto iq = tone(2048, 0.125);
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int i = 0; i < 100; ++i) {
            spectrum.addIQ(100e6, iq.data(), 2048);
            spectrum.completeSweep();
        }
        done = true;
    });
    bool valid = true;
    while (!done) {
        const auto snap = spectrum.snapshot();
        valid = valid && snap.spectrum.size() == 1024 && snap.waterfall.size() <= 64;
    }
    producer.join();
    check(valid && spectrum.snapshot().passes == 100, "concurrent snapshot inconsistent");
}

int main() {
    try {
        testPlan();
        testToneAndPasses();
        testCoverage();
        testRetuneAndSettling();
        testConcurrentSnapshots();
        std::cout << "Sweep tests passed: planning, FFT, coverage, queue/settling, snapshots\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Sweep test failed: " << error.what() << '\n';
        return 1;
    }
}
