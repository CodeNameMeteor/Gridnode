// claim-lab: the one mod for trying research claims in game before they become SDK/API.
// Put claim_lab.dll in the game's mods/ folder. Its window is in the GridNode menu (INSERT).
//
// Each research topic is one file in experiments/ (copy experiments/example_events.cpp). An
// experiment uses only the GridNode API that exists today, and does nothing until you tick
// "Install" for it. Hooks and patches go through GridNode, so unloading this mod (the button, or
// the Mods list) undoes all of them. When a topic's claims are proven in game, it moves into the
// SDK/API and its experiment is retired.
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/ui.hpp"
#include "lab.hpp"

GN_MOD_INFO("claim-lab", "Claim Lab", "1.0.0", "GridNode", "Tests research claims in game before they become SDK/API");

namespace lab {
std::vector<Experiment>& registry() {
    static std::vector<Experiment> r;
    return r;
}
} // namespace lab

namespace {

enum class State : uint8_t { Idle, Installed, Failed };
std::vector<State> g_state;

} // namespace

GN_MOD("claim-lab") {
    g_state.assign(lab::registry().size(), State::Idle);
    gn::log("claim-lab: %u experiments; nothing is installed until you tick it in the window",
            static_cast<unsigned>(lab::registry().size()));
    gn::ui::onDraw([](bool menuOpen) {
        if (!menuOpen) return;
        // A fixed first size and a height cap: grown to its content it ran off the bottom of the screen.
        const ImVec2 view = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, view.y * 0.5f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 20, ImGui::GetFontSize() * 8), ImVec2(view.x, view.y * 0.8f));
        ImGui::Begin("Claim Lab###claim-lab");
        ImGui::TextWrapped("Tests research claims before they become SDK/API. Nothing runs until you press Install; "
                           "unloading this mod undoes every hook.");
        const auto& reg = lab::registry();
        for (size_t i = 0; i < reg.size() && i < g_state.size(); ++i) {
            const lab::Experiment& e = reg[i];
            ImGui::PushID(static_cast<int>(i));
            const char* tag = g_state[i] == State::Installed ? " [installed]" : g_state[i] == State::Failed ? " [FAILED]" : "";
            const std::string title = std::string(e.topic) + tag + "###exp";
            if (ImGui::CollapsingHeader(title.c_str())) {
                ImGui::TextWrapped("Claims: %s", e.claims);
                if (g_state[i] == State::Idle) {
                    if (ImGui::Button("Install")) {
                        gn::log("claim-lab: installing %s", e.topic);
                        g_state[i] = e.install && e.install() ? State::Installed : State::Failed;
                        gn::log("claim-lab: %s %s", e.topic, g_state[i] == State::Installed ? "installed" : "FAILED");
                    }
                } else if (g_state[i] == State::Installed) {
                    if (e.draw) e.draw();
                    if (e.reset && ImGui::Button("Reset")) e.reset();
                } else {
                    ImGui::TextDisabled("Install failed (see gridnode.log). Unload and reload this mod to try again.");
                }
            }
            ImGui::PopID();
        }
        if (reg.empty()) ImGui::TextDisabled("No experiments built in.");
        ImGui::Separator();
        if (ImGui::Button("Unload Claim Lab (undoes all hooks)")) gn::unloadSelf();
        ImGui::End();
    });
    return true;
}

GN_MOD_UNLOAD() { return true; }
