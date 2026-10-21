#pragma once

#include "sweep_plan.h"
#include "sweep_spectrum.h"
#include <atomic>
#include <imgui.h>
#include <json.hpp>
#include <string>

namespace sweep {

class Controller {
public:
    explicit Controller(const Settings& settings = Settings{}) : settings_(settings) {}

    void load(const nlohmann::json& value) {
        try {
            settings_.startMHz = value.value("startMHz", 400.0);
            settings_.stopMHz = value.value("stopMHz", 410.0);
            settings_.stepMHz = value.value("stepMHz", 1.0);
            settings_.dwellMs = value.value("dwellMs", 20);
            settings_.settleMs = value.value("settleMs", 10);
        }
        catch (const nlohmann::json::exception&) { settings_ = Settings{}; }
        // Opening SDR++ always starts in ordinary receive mode.
        settings_.enabled = false;
    }

    nlohmann::json save() const {
        return {{"startMHz", settings_.startMHz}, {"stopMHz", settings_.stopMHz},
                {"stepMHz", settings_.stepMHz}, {"dwellMs", settings_.dwellMs},
                {"settleMs", settings_.settleMs}};
    }

    bool begin(double sampleRate, double bandwidth, int blockSize, bool serverMode) {
        active_ = settings_.enabled && !serverMode;
        fail("");
        centerHz_.store(0.0);
        if (!active_) { return true; }
        if (!plan_.configure(settings_, sampleRate, bandwidth) || blockSize < 256) {
            fail(plan_.error.empty() ? "RX block is too short for sweep FFT." : plan_.error);
            return false;
        }
        rate_ = sampleRate;
        spectrum_.configure(plan_.startHz, plan_.stopHz, rate_, plan_.usefulHz, blockSize);
        index_ = 0;
        tunePending_ = true;
        return true;
    }

    bool active() const { return active_; }
    bool needsTune() const { return active_ && tunePending_; }
    double nextCenter() const { return plan_.centers[index_]; }
    const Plan& plan() const { return plan_; }

    // Called only after a successful hardware tune. Flush the known driver
    // queue before counting settling samples, then collect at least one FFT.
    void tuned(int queuedBlocks) {
        centerHz_.store(nextCenter());
        queuedBlocks_ = queuedBlocks;
        settleSamples_ = static_cast<std::uint64_t>(std::ceil(rate_ * settings_.settleMs / 1000.0));
        dwellSamples_ = static_cast<std::uint64_t>(std::ceil(rate_ * settings_.dwellMs / 1000.0));
        tunePending_ = false;
    }

    void consume(const int16_t* iq, int samples) {
        if (!active_ || tunePending_ || samples <= 0) { return; }
        if (queuedBlocks_ > 0) { --queuedBlocks_; return; }
        if (settleSamples_ > 0) {
            settleSamples_ -= (std::min)(settleSamples_, static_cast<std::uint64_t>(samples));
            return;
        }
        if (!spectrum_.addIQ(centerHz_.load(), iq, samples)) { return; }
        dwellSamples_ -= (std::min)(dwellSamples_, static_cast<std::uint64_t>(samples));
        if (dwellSamples_ != 0) { return; }
        if (++index_ == plan_.centers.size()) {
            spectrum_.completeSweep();
            index_ = 0;
        }
        tunePending_ = true;
    }

    void fail(const std::string& message) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        error_ = message;
    }

    // Call only in the local GUI, never from a server-mode SmGui recorder.
    bool draw(bool running) {
        ImGui::Separator();
        ImGui::TextUnformatted("Wideband sweep");
        ImGui::PushID(this);
        ImGui::BeginDisabled(running);
        bool changed = ImGui::Checkbox("Enable sweep", &settings_.enabled);
        ImGui::TextUnformatted("Start (MHz)");
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::InputDouble("##start", &settings_.startMHz, 1.0, 10.0, "%.3f");
        ImGui::TextUnformatted("Stop (MHz)");
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::InputDouble("##stop", &settings_.stopMHz, 1.0, 10.0, "%.3f");
        ImGui::TextUnformatted("Step (MHz)");
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::InputDouble("##step", &settings_.stepMHz, 0.1, 1.0, "%.3f");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        changed |= ImGui::InputInt("Dwell ms", &settings_.dwellMs);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        changed |= ImGui::InputInt("Settle ms", &settings_.settleMs);
        ImGui::EndDisabled();
        if (running && active_) {
            ImGui::Text("LO: %.3f MHz", centerHz_.load() / 1e6);
            ImGui::Text("Step: %.3f MHz (%d centers)", plan_.stepHz / 1e6,
                        static_cast<int>(plan_.centers.size()));
            ImGui::TextWrapped("Sweep view below. Main spectrum and audio are muted.");
        }
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            if (!error_.empty()) { ImGui::TextWrapped("Sweep: %s", error_.c_str()); }
        }
        const auto snapshot = spectrum_.snapshot();
        if (!snapshot.spectrum.empty()) { drawSpectrum(snapshot); }
        ImGui::PopID();
        return changed;
    }

private:
    void drawSpectrum(const Spectrum::Snapshot& snapshot) {
        ImGui::Text("%.3f - %.3f MHz", snapshot.startHz / 1e6, snapshot.stopHz / 1e6);
        ImGui::Text("Completed sweeps: %llu", static_cast<unsigned long long>(snapshot.passes));
        ImGui::TextUnformatted("Floor (relative dB)");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##floor", &floorDb_, -140.0f, -30.0f, "%.0f");
        ImGui::PlotLines("##spectrum", snapshot.spectrum.data(),
                         static_cast<int>(snapshot.spectrum.size()), 0, nullptr,
                         floorDb_, 0.0f, ImVec2(-1.0f, 100.0f));

        // A child draw list and a bounded mesh also work with 16-bit ImDrawIdx
        // backends without RendererHasVtxOffset (the old per-pixel plot overflowed).
        if (ImGui::BeginChild("waterfall", ImVec2(0, 170), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            const ImVec2 size((std::max)(1.0f, ImGui::GetContentRegionAvail().x), 150.0f);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##waterfall", size);
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, 255));
            const int columns = (std::min)(128, (std::max)(1, static_cast<int>(size.x)));
            const int bins = static_cast<int>(snapshot.spectrum.size());
            const int rows = static_cast<int>(snapshot.waterfall.size());
            for (int y = 0; y < rows; ++y) {
                const auto& row = snapshot.waterfall[rows - 1 - y];
                for (int x = 0; x < columns; ++x) {
                    float peak = Spectrum::floorDb;
                    for (int b = x * bins / columns; b < (x + 1) * bins / columns; ++b) {
                        peak = (std::max)(peak, row[b]);
                    }
                    const float t = std::clamp((peak - floorDb_) / -floorDb_, 0.0f, 1.0f);
                    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(std::clamp(3.0f * t - 1.5f, 0.0f, 1.0f),
                               std::clamp(3.0f * t - 0.5f, 0.0f, 1.0f),
                               std::clamp(3.0f * t, 0.0f, 1.0f), 1.0f));
                    draw->AddRectFilled(ImVec2(pos.x + x * size.x / columns, pos.y + y * size.y / 64),
                                        ImVec2(pos.x + (x + 1) * size.x / columns, pos.y + (y + 1) * size.y / 64), color);
                }
            }
            if (ImGui::IsItemHovered()) {
                const double fraction = std::clamp(static_cast<double>((ImGui::GetIO().MousePos.x - pos.x) / size.x), 0.0, 1.0);
                ImGui::SetTooltip("%.6f MHz", (snapshot.startHz + fraction * (snapshot.stopHz - snapshot.startHz)) / 1e6);
            }
        }
        ImGui::EndChild();
    }

    Settings settings_;
    Plan plan_;
    Spectrum spectrum_;
    bool active_ = false, tunePending_ = false;
    std::size_t index_ = 0;
    int queuedBlocks_ = 0;
    double rate_ = 1.0;
    std::uint64_t settleSamples_ = 0, dwellSamples_ = 0;
    std::atomic<double> centerHz_{0.0};
    std::mutex statusMutex_;
    std::string error_;
    float floorDb_ = -100.0f;
};

} // namespace sweep
