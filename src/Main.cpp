// TreeCapacitator — LeviLauncher (LeviLaunchroid) native mod entry point.
//
// Lifecycle integration follows the preloader SDK contract:
//   * the launcher dlopens this library and calls PLGetModRegistration()
//   * load()  -> engine is created, optional game hooks are attempted
//   * enable()-> simulation thread starts
//   * disable/unload -> clean shutdown
//
// All pl::* symbols referenced here are provided at runtime by the
// libpreloader.so injected by LeviLaunchroid (the mod ships with DT_NEEDED
// libpreloader.so but does not bundle it).

#include "tree_capacitor.h"
#include "weather_hooks.h"
#include "hud.h"

#include <jni.h>

#include <pl/Mod.hpp>
#include <pl/Export.hpp>
#include <pl/Logger.hpp>
#include <pl/ModMenu.hpp>
#include <pl/Input.hpp>

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#define LOG_TAG "TreeCap"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Forward-declared lifecycle helpers (defined at the bottom, after the
// anonymous-namespace onToggle callbacks that need them).
namespace {
void SetEnabledState(bool on);
}

namespace {

constexpr const char* kModuleId = "treecapacitator";
constexpr const char* kModAuthor = "ChimeraAnt-DEV";
constexpr const char* kModVersion = "1.0.0";

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_hooksInstalled{false};
std::thread g_simThread;
std::atomic<bool> g_simRunning{false};

// ───────────────────────────────────────────────────────────────────────────
// Simulation thread. Advances the capacitor grid at 10 Hz and refreshes the
// HUD overlay a couple of times per second.
// ───────────────────────────────────────────────────────────────────────────
void SimLoop() {
    LOGI("Simulation thread started");
    treecap::api::SetEnabled(true);

    int frame = 0;
    while (g_simRunning.load(std::memory_order_relaxed)) {
        treecap::api::Tick();

        if (++frame % 20 == 0) {
            const treecap::CapacitorStats stats = treecap::api::Snapshot();
            LOGI("[TreeCap] nodes=%d charged=%d charge=%ld/%ld pulses=%ld weather=%d%s",
                 stats.nodeCount, stats.chargedCount, stats.totalCharge,
                 stats.capacity, stats.pulsesDelivered,
                 static_cast<int>(stats.weather),
                 stats.weatherFromGame ? " (game)" : " (simulated)");
            hud::RenderStats(stats);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOGI("Simulation thread stopped");
}

// ───────────────────────────────────────────────────────────────────────────
// Mod menu integration
// ───────────────────────────────────────────────────────────────────────────
pl::modmenu::ModuleBuilder MakeModuleBuilder() {
    auto builder = pl::modmenu::ModuleBuilder(kModuleId, "TreeCapacitator")
                       .description("Tree capacitor network: rain and thunder "
                                    "charge capacitor roots that release growth "
                                    "pulses into nearby trees.")
                       .modId(kModuleId);

    builder.config("rain_rate", "Rain charge rate",
                   pl::modmenu::ConfigType::SliderInt,
                   std::to_string(treecap::kRainRate), "0",
                   std::to_string(treecap::kThunderRate));
    builder.config("thunder_rate", "Thunder charge rate",
                   pl::modmenu::ConfigType::SliderInt,
                   std::to_string(treecap::kThunderRate), "0",
                   std::to_string(treecap::kThunderRate * 2));
    builder.config("leak_rate", "Discharge leak",
                   pl::modmenu::ConfigType::SliderInt,
                   std::to_string(treecap::kLeakRate), "0", "50");

    builder.onToggle([](std::string_view moduleId, bool enabled) {
        if (moduleId == kModuleId) {
            SetEnabledState(enabled);
        }
    });
    builder.onConfigChanged([](std::string_view moduleId, std::string_view key,
                               std::string_view value) {
        if (moduleId != kModuleId) return;
        int v = 0;
        try {
            v = std::stoi(std::string(value));
        } catch (...) {
            return;
        }
        if (key == "rain_rate") treecap::api::SetRainRate(v);
        else if (key == "thunder_rate") treecap::api::SetThunderRate(v);
        else if (key == "leak_rate") treecap::api::SetLeakRate(v);
    });

    return builder;
}

// ───────────────────────────────────────────────────────────────────────────
// Input bridge: toggle the network on/off with a keypress (e.g. a button).
// ───────────────────────────────────────────────────────────────────────────
bool OnKeyEvent(const pl::input::KeyEvent& event) {
    if (!event.isKeyDown || event.keyCode == 0) return false;
    static const int kDefaultToggleKey = 48; // Android KEYCODE_T
    if (event.keyCode == kDefaultToggleKey) {
        SetEnabledState(!g_enabled.load(std::memory_order_relaxed));
        return true;
    }
    return false;
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// Lifecycle: load/enable/disable/unload invoked by the preloader.
// ───────────────────────────────────────────────────────────────────────────
namespace {

bool OnLoad(pl::mod::ModContext& ctx) {
    LOGI("TreeCapacitator v%s loading (author: %s)", kModVersion, kModAuthor);

    if (!MakeModuleBuilder().registerModule()) {
        LOGE("Failed to register mod menu module; continuing without menu UI");
    }
    pl::input::registerKeyCallback(OnKeyEvent);

    treecap::api::Init(256, 0x5EED);
    treecap::api::SetWeather(treecap::WeatherState::Clear, false);
    return true;
}

bool OnEnable(pl::mod::ModContext& ctx) {
    LOGI("TreeCapacitator enabling");
    SetEnabledState(true);

    // Hook the real in-game weather. If the current Minecraft build does not
    // match the shipped signatures, the network runs on its built-in storm
    // simulator instead — the mod stays fully functional.
    g_hooksInstalled.store(weatherhooks::Install());
    if (!g_hooksInstalled.load()) {
        LOGI("Weather signatures not found — using built-in storm simulator");
    }
    return true;
}

bool OnDisable(pl::mod::ModContext& ctx) {
    LOGI("TreeCapacitator disabling");
    SetEnabledState(false);
    if (g_hooksInstalled.exchange(false)) {
        weatherhooks::Uninstall();
        treecap::api::SetWeather(treecap::WeatherState::Clear, false);
    }
    return true;
}

bool OnUnload(pl::mod::ModContext& ctx) {
    LOGI("TreeCapacitator unloading");
    SetEnabledState(false);
    weatherhooks::Uninstall();
    pl::modmenu::unregisterModule(kModuleId);
    treecap::api::Shutdown();
    return true;
}

void SetEnabledState(bool on) {
    const bool was = g_enabled.exchange(on);
    if (on == was) return;

    if (on) {
        g_simRunning.store(true);
        g_simThread = std::thread(SimLoop);
    } else {
        g_simRunning.store(false);
        if (g_simThread.joinable()) g_simThread.join();
    }
    hud::MarkDirty();
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// Preloader registration
// ───────────────────────────────────────────────────────────────────────────
class TreeCapacitatorMod {
public:
    static TreeCapacitatorMod& instance() {
        static TreeCapacitatorMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext& context) { return OnLoad(context); }
    bool enable(pl::mod::ModContext& context) { return OnEnable(context); }
    bool disable(pl::mod::ModContext& context) { return OnDisable(context); }
    bool unload(pl::mod::ModContext& context) { return OnUnload(context); }
};

PL_REGISTER_MOD(TreeCapacitatorMod, TreeCapacitatorMod::instance())
