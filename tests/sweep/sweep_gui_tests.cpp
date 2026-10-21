#include "../../source_modules/sweep/sweep_controller.h"
#include <iostream>
#include <vector>

int main() {
    sweep::Settings settings;
    settings.enabled = true;
    settings.startMHz = 100;
    settings.stopMHz = 100.1;
    settings.dwellMs = 1;
    settings.settleMs = 0;
    sweep::Controller controller(settings);
    if (!controller.begin(1e6, 1e6, 2048, false)) { return 1; }
    std::vector<int16_t> iq(4096, 1000);
    for (int i = 0; i < 70; ++i) {
        controller.tuned(0);
        controller.consume(iq.data(), 2048);
    }

    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 1200);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    bool valid = true;
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(360, 1100));
        ImGui::Begin("Sweep test");
        controller.draw(true);
        ImGui::End();
        ImGui::Render();
        const auto* draw = ImGui::GetDrawData();
        if (frame > 0) {
            valid = valid && draw->TotalVtxCount > 30000;
            for (int i = 0; i < draw->CmdListsCount; ++i) {
                valid = valid && draw->CmdLists[i]->VtxBuffer.Size < 65536;
            }
        }
    }
    ImGui::DestroyContext();
    if (!valid) {
        std::cerr << "Sweep GUI failed the full-history 16-bit mesh check\n";
        return 1;
    }
    std::cout << "Sweep GUI passed: full waterfall, 16-bit indices, no renderer required\n";
    return 0;
}
