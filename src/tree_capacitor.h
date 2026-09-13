#pragma once

#include <atomic>
#include <cstdint>

namespace treecap {

// Weather state that drives capacitor charging. Populated either from the
// optional real game hook (WeatherHooks.kt-style hooks via pl::memory) or
// from the built-in storm simulator when no signature matches the running
// Minecraft build.
enum class WeatherState : int {
    Clear = 0,
    Rain = 1,
    Thunder = 2,
};

constexpr int kMaxCharge = 10000;   // charge capacity per capacitor node
constexpr int kRainRate = 60;       // charge units gained per engine tick in rain
constexpr int kThunderRate = 250;   // charge per tick during thunderstorm
constexpr int kLeakRate = 2;        // passive loss per tick

// Tunables (exposed through the LeviLauncher mod menu). Rates are per
// simulation tick (the engine runs at 10 Hz).
struct Config {
    int rainRate = kRainRate;
    int thunderRate = kThunderRate;
    int chargeCapacity = kMaxCharge;
    int leakRate = kLeakRate;
};

struct CapacitorNode {
    int charge = 0;
    int x = 0;
    int y = 0;
    int z = 0;
    bool recharged = false; // became fully charged since last check
};

struct CapacitorStats {
    int nodeCount = 0;
    int chargedCount = 0;
    long totalCharge = 0;
    long capacity = 0;
    long pulsesDelivered = 0;
    WeatherState weather = WeatherState::Clear;
    bool weatherFromGame = false; // true when driven by a real signature hook
    bool enabled = false;
};

// Self-contained tree-capacitor simulation engine.
//
// The capacitor network is a grid of cells that absorb sky charge during rain
// and thunderstorms and, once full, release a "growth pulse" that a game-side
// integration can consume to accelerate nearby trees. The engine itself has no
// dependency on the Minecraft binary — it runs on a background thread and can
// be driven by the optional game-weather hook when a matching signature can be
// resolved, otherwise it falls back to a built-in storm simulator.
class Engine {
public:
    static Engine& instance();

    void start(int nodeCount, int seed);
    void stop();

    // Standard library mutex-free control flags.
    void setEnabled(bool on);
    bool enabled() const;

    void setWeather(WeatherState weather, bool fromGame);

    // Runtime tunables (used by the Mod Menu).
    void setRainRate(int rate);
    void setThunderRate(int rate);
    void setLeakRate(int rate);
    void setCapacity(int capacity);

    // Advances the simulation by one logical tick. Safe from any thread.
    void tick();

    // Thread-safe snapshot for HUD/logging.
    CapacitorStats snapshot() const;

    long pulsesDelivered() const;

private:
    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    struct Impl;
    std::atomic<Impl*> mImpl{nullptr};
    std::atomic<bool> mEnabled{true};
};

// Convenience back-ends used by the LeviLauncher entry point.
namespace api {
    void Init(int nodeCount, int seed);
    void Shutdown();
    void SetEnabled(bool on);
    bool IsEnabled();
    void SetWeather(WeatherState weather, bool fromGame);
    void Tick();
    void SetRainRate(int rate);
    void SetThunderRate(int rate);
    void SetLeakRate(int rate);
    void SetCapacity(int capacity);
    CapacitorStats Snapshot();
    long PulsesDelivered();
}

} // namespace treecap
