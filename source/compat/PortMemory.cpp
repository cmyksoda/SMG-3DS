// smg::PortMemory (compat/smg/FreeStore.hpp): a per-thread count of open scopes, which the whole game's plain new
// consults (source/compat/GameHeap.cpp); elsewhere nothing replaces new, so the count changes nothing.
#include <smg/FreeStore.hpp>

namespace {
thread_local int sDepth = 0;
} // namespace

smg::PortMemory::PortMemory() { ++sDepth; }
smg::PortMemory::~PortMemory() { --sDepth; }
bool smg::PortMemory::active() { return sDepth != 0; }
