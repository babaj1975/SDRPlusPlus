#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace sweep {

struct Settings {
    bool enabled = false;
    double startMHz = 400.0;
    double stopMHz = 410.0;
    double stepMHz = 1.0;
    int dwellMs = 20;
    int settleMs = 10;
};

// Settings and the plan are immutable between source start and stop.
class Plan {
public:
    bool configure(const Settings& settings, double sampleRate, double bandwidth) {
        centers.clear();
        error.clear();
        if (!std::isfinite(settings.startMHz) || !std::isfinite(settings.stopMHz) ||
            !std::isfinite(settings.stepMHz) || settings.startMHz <= 0.0 ||
            settings.stopMHz <= settings.startMHz || settings.stopMHz > 10000.0 ||
            settings.stepMHz <= 0.0 || settings.dwellMs < 1 || settings.dwellMs > 10000 ||
            settings.settleMs < 0 || settings.settleMs > 10000 ||
            !std::isfinite(sampleRate) || sampleRate <= 0.0 ||
            !std::isfinite(bandwidth) || bandwidth <= 0.0) {
            error = "Invalid sweep range, rate or timing.";
            return false;
        }

        startHz = settings.startMHz * 1e6;
        stopHz = settings.stopMHz * 1e6;
        usefulHz = 0.8 * (std::min)(sampleRate, bandwidth);
        // Never leave gaps when the requested step exceeds usable bandwidth.
        stepHz = (std::min)(settings.stepMHz * 1e6, usefulHz);
        const double span = stopHz - startHz;
        const double travel = (std::max)(0.0, span - usefulHz);
        const double hops = std::ceil(travel / stepHz);
        if (!std::isfinite(hops) || hops > 100000.0) {
            error = "Too many sweep steps; increase Step MHz.";
            return false;
        }

        if (span <= usefulHz) {
            centers.push_back((startHz + stopHz) * 0.5);
        }
        else {
            const double first = startHz + usefulHz * 0.5;
            const double last = stopHz - usefulHz * 0.5;
            for (std::size_t i = 0; i <= static_cast<std::size_t>(hops); ++i) {
                centers.push_back((std::min)(first + i * stepHz, last));
            }
        }
        return true;
    }

    double startHz = 0.0;
    double stopHz = 0.0;
    double usefulHz = 0.0;
    double stepHz = 0.0;
    std::vector<double> centers;
    std::string error;
};

} // namespace sweep
