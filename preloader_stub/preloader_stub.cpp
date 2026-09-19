// Build-time stub of libpreloader.so for linking LeviLaunchroid mods.
//
// LeviLaunchroid injects the real libpreloader.so (with its pl::* runtime
// APIs) into the Minecraft process and loads mods with RTLD_GLOBAL, so a mod
// only needs to *link* against libpreloader.so and let the runtime resolve the
// symbols. This stub is used purely at build time to (a) emit the correct
// DT_NEEDED libpreloader.so entry and (b) produce symbol references with the
// same NDK/libc++ mangling as the real preloader. None of these functions are
// ever called from the stub.
//
// IMPORTANT: keep the list of referenced symbols in sync with src/ so the
// produced undefined symbols match what libpreloader.so actually exports.

#include <pl/Input.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>
#include <pl/memory/Vtable.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace pl::mod {

NativeMod *NativeMod::current() noexcept {
    return nullptr;
}

} // namespace pl::mod

namespace pl::memory {

int hook(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc,
         HookPriority priority) {
    return -1;
}

bool unhook(FuncPtr target, FuncPtr detour) {
    return false;
}

std::uintptr_t resolveSignature(std::string_view signature,
                                std::string_view moduleName) {
    return 0;
}

std::uintptr_t resolveVtableFunction(std::string_view typeInfoName,
                                     std::size_t slot,
                                     std::string_view moduleName) {
    return 0;
}

} // namespace pl::memory

namespace pl::input {

void registerKeyCallback(KeyCallback callback) {
    (void)callback;
}

} // namespace pl::input

namespace pl::modmenu {

bool registerModule(const ModuleInfo &info) {
    (void)info;
    return false;
}

void unregisterModule(std::string_view moduleId) {
    (void)moduleId;
}

void submitDrawCommands(std::string_view moduleId,
                        std::span<const DrawCommand> commands) {
    (void)moduleId;
    (void)commands;
}

} // namespace pl::modmenu