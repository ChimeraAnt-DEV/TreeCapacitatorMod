#pragma once
#include <cstdint>
#include <string_view>
namespace pl::memory {
inline uintptr_t resolveSignature(std::string_view, std::string_view) { return 0; }
}