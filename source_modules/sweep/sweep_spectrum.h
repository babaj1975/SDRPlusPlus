#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace sweep {

// Shared by PlutoSDR and bladeRF. Based on the earlier sweep_spectrum_msvc.h;
// explicit pass boundaries also support sweeps containing a single LO center.
class Spectrum {
public:
    static constexpr float floorDb = -140.0f;
    struct Snapshot {
        double startHz = 0.0;
        double stopHz = 0.0;
        std::vector<float> spectrum;
        std::deque<std::vector<float>> waterfall;
        std::uint64_t passes = 0;
    };

    void configure(double startHz, double stopHz, double rate, double usefulHz,
                   int blockSize, int bins = 1024) {
        std::lock_guard<std::mutex> lock(mutex_);
        startHz_ = startHz;
        stopHz_ = stopHz;
        rate_ = rate;
        usefulHz_ = usefulHz;
        int n = 256;
        while (n < 2048 && n * 2 <= blockSize) { n *= 2; }
        fft_.resize(n);
        window_.resize(n);
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) {
            window_[i] = 0.5f - 0.5f * std::cos(2.0f * pi * i / (n - 1));
            sum += window_[i];
        }
        normalization_ = sum * sum;
        current_.assign(std::clamp(bins, 256, 8192), floorDb);
        previous_ = current_;
        hit_.assign(current_.size(), false);
        rows_.clear();
        passes_ = 0;
    }

    // The source owns the producer thread. Configuration happens before it starts.
    // Levels are relative to the existing SDR++ int16 / 32768 sample conversion.
    bool addIQ(double centerHz, const int16_t* iq, int samples) {
        const int n = static_cast<int>(fft_.size());
        if (!iq || n == 0 || samples < n || !std::isfinite(centerHz)) { return false; }
        for (int i = 0; i < n; ++i) {
            fft_[i] = std::complex<float>(iq[2 * i] / 32768.0f, iq[2 * i + 1] / 32768.0f) * window_[i];
        }
        fft(fft_);

        std::lock_guard<std::mutex> lock(mutex_);
        const int bins = static_cast<int>(current_.size());
        const double binHz = rate_ / n;
        const double pixelsPerHz = bins / (stopHz_ - startHz_);
        bool added = false;
        for (int k = 0; k < n; ++k) {
            const double offset = (static_cast<double>(k) / n - 0.5) * rate_;
            if (offset + binHz * 0.5 < -usefulHz_ * 0.5 ||
                offset - binHz * 0.5 > usefulHz_ * 0.5) { continue; }
            const double frequency = centerHz + offset;
            const double lo = (std::max)(frequency - binHz * 0.5, centerHz - usefulHz_ * 0.5);
            const double hi = (std::min)(frequency + binHz * 0.5, centerHz + usefulHz_ * 0.5);
            if (hi < startHz_ || lo >= stopHz_) { continue; }
            const int first = std::clamp(static_cast<int>(std::floor((lo - startHz_) * pixelsPerHz)), 0, bins - 1);
            const int last = std::clamp(static_cast<int>(std::floor((hi - startHz_) * pixelsPerHz)), 0, bins - 1);
            const float power = std::norm(fft_[(k + n / 2) % n]) / normalization_;
            const float db = 10.0f * std::log10((std::max)(power, 1e-14f));
            for (int out = first; out <= last; ++out) {
                current_[out] = (std::max)(current_[out], db);
                hit_[out] = true;
            }
            added = true;
        }
        return added;
    }

    void completeSweep() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::none_of(hit_.begin(), hit_.end(), [](bool hit) { return hit; })) { return; }
        previous_ = current_;
        rows_.push_back(current_);
        if (rows_.size() > 64) { rows_.pop_front(); }
        ++passes_;
        std::fill(current_.begin(), current_.end(), floorDb);
        std::fill(hit_.begin(), hit_.end(), false);
    }

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot result;
        result.startHz = startHz_;
        result.stopHz = stopHz_;
        result.spectrum = previous_;
        for (std::size_t i = 0; i < current_.size(); ++i) {
            if (hit_[i]) { result.spectrum[i] = current_[i]; }
        }
        result.waterfall = rows_;
        result.passes = passes_;
        return result;
    }

private:
    static constexpr float pi = 3.14159265358979323846f;
    static void fft(std::vector<std::complex<float>>& a) {
        const int n = static_cast<int>(a.size());
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) { j ^= bit; }
            j ^= bit;
            if (i < j) { std::swap(a[i], a[j]); }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const float angle = -2.0f * pi / len;
            const std::complex<float> step(std::cos(angle), std::sin(angle));
            for (int i = 0; i < n; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (int j = 0; j < len / 2; ++j) {
                    const auto u = a[i + j];
                    const auto v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= step;
                }
            }
        }
    }

    mutable std::mutex mutex_;
    double startHz_ = 0.0, stopHz_ = 1.0, rate_ = 1.0, usefulHz_ = 1.0;
    float normalization_ = 1.0f;
    std::vector<std::complex<float>> fft_;
    std::vector<float> window_, current_, previous_;
    std::vector<bool> hit_;
    std::deque<std::vector<float>> rows_;
    std::uint64_t passes_ = 0;
};

} // namespace sweep
