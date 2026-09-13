#include "pl/memory/Hook.hpp"
namespace pl::memory {
int hook(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc, HookPriority) {
    (void)target; (void)detour;
    if (originalFunc) *originalFunc = nullptr;
    return 0;
}
bool unhook(FuncPtr, FuncPtr) { return true; }
}