#include "pl/memory/Vtable.hpp"

namespace pl::memory {

VtableResolver g_testVtableResolver = nullptr;

void SetVtableResolverForTest(VtableResolver resolver) {
    g_testVtableResolver = resolver;
}

} // namespace pl::memory