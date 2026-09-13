#pragma once
#include "pl/Export.hpp"
namespace pl::memory {
using FuncPtr = void *;
enum class HookPriority : int { Normal = 200, High = 100 };
PL_EXPORT int hook(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc,
                   HookPriority priority = HookPriority::Normal);
PL_EXPORT bool unhook(FuncPtr target, FuncPtr detour);
}