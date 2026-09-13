#include "tree_capacitor.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace treecap {
namespace {

// Grid of capacitor cells. Each cell occupies a 48x48 block region so an
// average tree farm sits comfortably inside one cell.
constexpr int kCellSize = 48;
constexpr int kGridHalfWidth = 8; // 16 x 16 cells = 256 capacitor roots
constexpr int kMaxNodes = kGridHalfWidth * 2 * kGridHalfWidth * 2;

// Deterministic, allocation-friendly pseudo-random number generator.
struct FastRng {
    uint64_t state;
    explicit FastRng(uint64_t seed) : state(seed ? seed : 0x853C49E6748FEA9BULL) {}
    uint32_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<uint32_t>((state * 2685821657736338717ULL) >> 32);
    }
    int range(int min, int max) {
        return min + static_cast<int>(next() % static_cast<uint32_t>(max - min + 1));
    }
};

} // namespace

struct Engine::Impl {
    std::mutex mutex;
    std::vector<CapacitorNode> nodes;
    Config config;
    int tickCount = 0;
    WeatherState weather = WeatherState::Clear;
    bool weatherFromGame = false;
    long pulses = 0;
    long capacity = 0;
    bool running = false;
};

Engine& Engine::instance() {
    static Engine engine;
    return engine;
}

void Engine::start(int nodeCount, int seed) {
    stop();

    auto* impl = new (std::nothrow) Impl();
    if (!impl) return;

    FastRng rng(static_cast<uint64_t>(seed) ^ 0x9E3779B97F4A7C15ULL);
    nodeCount = std::clamp(nodeCount, 1, kMaxNodes);

    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->nodes.reserve(static_cast<size_t>(nodeCount));
        impl->capacity = 0;

        for (int i = 0; i < nodeCount; ++i) {
            const int gx = (i % (kGridHalfWidth * 2)) - kGridHalfWidth;
            const int gz = (i / (kGridHalfWidth * 2)) - kGridHalfWidth;

            CapacitorNode node;
            node.x = static_cast<int>((gx * kCellSize) + (kCellSize / 2));
            node.z = static_cast<int>((gz * kCellSize) + (kCellSize / 2));
            // Slight per-node vertical ripple so the "roots" sit at plausible
            // ground levels (y ~ 62-66).
            node.y = 62 + rng.range(0, 4);
            node.charge = 0;
            node.recharged = false;
            impl->capacity += kMaxCharge;
            impl->nodes.push_back(node);
        }

        impl->running = true;
    }

    mImpl.store(impl, std::memory_order_release);
}

void Engine::stop() {
    Impl* impl = mImpl.exchange(nullptr, std::memory_order_acq_rel);
    if (!impl) return;

    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->running = false;
    delete impl;
}

void Engine::setEnabled(bool on) {
    mEnabled.store(on, std::memory_order_release);
}

bool Engine::enabled() const {
    return mEnabled.load(std::memory_order_acquire);
}

void Engine::setWeather(WeatherState weather, bool fromGame) {
    Impl* impl = mImpl.load(std::memory_order_acquire);
    if (!impl) return;

    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->weather = weather;
    impl->weatherFromGame = fromGame;
}

void Engine::setRainRate(int rate) {
    Impl* impl = mImpl.load(std::memory_order_acquire);
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->config.rainRate = std::max(0, rate);
}

void Engine::setThunderRate(int rate) {
    Impl* impl = mImpl.load(std::memory_order_acquire);
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->config.thunderRate = std::max(0, rate);
}

void Engine::setLeakRate(int rate) {
    Impl* impl = mImpl.load(std::memory_order_acquire);
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->config.leakRate = std::max(0, rate);
}

void Engine::setCapacity(int capacity) {
    Impl* impl = mImpl.load(std::memory_order_acquire);
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->config.chargeCapacity = std::max(1, capacity);
    impl->capacity = static_cast<long>(impl->nodes.size()) * std::max(1, capacity);
}

void Engine::tick() {
    if (!enabled()) return;

    Impl* impl = mImpl.load(std::memory_order_acquire);
    if (!impl) return;

    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->running) return;

    ++impl->tickCount;

    const int capacity = std::max(1, impl->config.chargeCapacity);
    const int rate = impl->weather == WeatherState::Thunder
                         ? impl->config.thunderRate
                         : (impl->weather == WeatherState::Rain ? impl->config.rainRate : 0);
    const int leak = std::max(0, impl->config.leakRate);

    for (auto& node : impl->nodes) {
        const bool wasFull = node.charge >= capacity;

        if (rate > 0 && node.charge < capacity) {
            node.charge += rate;
            if (node.charge > capacity) node.charge = capacity;
        }

        const bool nowFull = node.charge >= capacity;
        node.recharged = nowFull && !wasFull;
        if (nowFull) {
            ++impl->pulses; // deliver a growth pulse to nearby trees
            node.charge = 0;
        } else if (node.charge > 0) {
            // Slow ambient leakage prevents the grid from staying saturated.
            node.charge -= leak;
            if (node.charge < 0) node.charge = 0;
        }
    }
}

CapacitorStats Engine::snapshot() const {
    CapacitorStats stats;
    Impl* impl = mImpl.load(std::memory_order_acquire);
    if (!impl) return stats;

    std::lock_guard<std::mutex> lock(impl->mutex);

    stats.nodeCount = static_cast<int>(impl->nodes.size());
    stats.chargedCount = 0;
    stats.totalCharge = 0;
    stats.capacity = impl->capacity;
    stats.pulsesDelivered = impl->pulses;
    stats.weather = impl->weather;
    stats.weatherFromGame = impl->weatherFromGame;
    stats.enabled = enabled();

    const int capacity = std::max(1, impl->config.chargeCapacity);
    for (const auto& node : impl->nodes) {
        stats.totalCharge += node.charge;
        if (node.charge >= capacity) ++stats.chargedCount;
    }
    return stats;
}

long Engine::pulsesDelivered() const {
    Impl* impl = mImpl.load(std::memory_order_acquire);
    if (!impl) return 0;
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->pulses;
}

namespace api {

void Init(int nodeCount, int seed) {
    Engine::instance().start(nodeCount, seed);
}

void Shutdown() {
    Engine::instance().stop();
}

void SetEnabled(bool on) {
    Engine::instance().setEnabled(on);
}

bool IsEnabled() {
    return Engine::instance().enabled();
}

void SetWeather(WeatherState weather, bool fromGame) {
    Engine::instance().setWeather(weather, fromGame);
}

void SetRainRate(int rate) {
    Engine::instance().setRainRate(rate);
}

void SetThunderRate(int rate) {
    Engine::instance().setThunderRate(rate);
}

void SetLeakRate(int rate) {
    Engine::instance().setLeakRate(rate);
}

void SetCapacity(int capacity) {
    Engine::instance().setCapacity(capacity);
}

void Tick() {
    Engine::instance().tick();
}

CapacitorStats Snapshot() {
    return Engine::instance().snapshot();
}

long PulsesDelivered() {
    return Engine::instance().pulsesDelivered();
}

} // namespace api
} // namespace treecap
