#include "hud.h"

#include <pl/ModMenu.hpp>

#include <android/log.h>

#include <string>
#include <vector>

#define LOG_TAG "TreeCap"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace hud {
namespace {

constexpr const char* kModuleId = "treecapacitator";

// Compose the multi-line overlay into DrawCommand primitives. Colors are 0xAARRGGBB.
std::vector<pl::modmenu::DrawCommand> BuildDrawCommands(
    const treecap::CapacitorStats& stats) {
    std::vector<pl::modmenu::DrawCommand> cmds;
    cmds.reserve(8);

    const float x = 8.0f;
    float y = 8.0f;
    const float lineH = 16.0f;
    const uint32_t headerColor = 0xFF66E07A;   // leafy green
    const uint32_t bodyColor = 0xFFFFFFFF;

    auto addText = [&](const std::string& text, uint32_t color) {
        pl::modmenu::DrawCommand cmd;
        cmd.type = pl::modmenu::DrawCommandType::Text;
        cmd.x = x;
        cmd.y = y;
        cmd.text = text;
        cmd.color = color;
        cmd.size = 1.0f;
        cmds.push_back(std::move(cmd));
        y += lineH;
    };

    addText("TreeCapacitator", headerColor);

    const long pct = stats.capacity > 0
                         ? (stats.totalCharge * 100 / stats.capacity)
                         : 0;
    addText("Nodes: " + std::to_string(stats.nodeCount), bodyColor);
    addText("Charge: " + std::to_string(stats.totalCharge) + " / " +
                std::to_string(stats.capacity) + " (" + std::to_string(pct) +
                "%)",
            bodyColor);
    addText("Pulses delivered: " + std::to_string(stats.pulsesDelivered),
            bodyColor);
    addText(stats.weatherFromGame
                ? (stats.weather == treecap::WeatherState::Thunder
                       ? "Weather: thunderstorm (game)"
                       : stats.weather == treecap::WeatherState::Rain
                             ? "Weather: rain (game)"
                             : "Weather: clear (game)")
                : (stats.weather == treecap::WeatherState::Thunder
                       ? "Weather: thunderstorm (simulated)"
                       : stats.weather == treecap::WeatherState::Rain
                             ? "Weather: rain (simulated)"
                             : "Weather: clear (simulated)"),
            bodyColor);
    addText(stats.enabled ? "Network: ENABLED" : "Network: disabled", bodyColor);

    return cmds;
}

} // namespace

void MarkDirty() {
    // The overlay relies on the next RenderStats call (from the simulation
    // thread, at 2 Hz) to re-publish. Marking dirty here mainly serves as a
    // wake-up signal when toggling.
    treecap::CapacitorStats stats = treecap::api::Snapshot();
    RenderStats(stats);
}

void RenderStats(const treecap::CapacitorStats& stats) {
    pl::modmenu::submitDrawCommands(kModuleId, BuildDrawCommands(stats));
}

} // namespace hud