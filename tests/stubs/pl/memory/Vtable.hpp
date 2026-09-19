#pragma once
// Host-test stub of pl/memory/Vtable.hpp. The feller calls
// resolveVtableFunction() as its version-independent fallback; the host test
// redirects it through a settable hook so the behaviour can be exercised
// without a real libminecraftpe.so.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pl::memory {

using VtableResolver = uintptr_t (*)(std::string_view typeInfoName,
                                     std::size_t slot,
                                     std::string_view moduleName);

// Sets (or clears, with nullptr) the function returned by
// resolveVtableFunction for the duration of a test.
void SetVtableResolverForTest(VtableResolver resolver);

extern VtableResolver g_testVtableResolver;

inline uintptr_t resolveVtableFunction(std::string_view typeInfoName,
                                       std::size_t slot,
                                       std::string_view moduleName) {
    return g_testVtableResolver
               ? g_testVtableResolver(typeInfoName, slot, moduleName)
               : 0;
}

} // namespace pl::memory