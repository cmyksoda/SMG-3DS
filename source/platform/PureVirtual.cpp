// A pure virtual call means an object used while being built or after being destroyed. The toolchain leaves
// __cxa_pure_virtual a weak, undefined name, which would make the call do nothing: here it stops by name.
#include <cstdio>
#include <cstdlib>

extern "C" void __cxa_pure_virtual() {
    std::fprintf(stderr, "A pure virtual function was called\n");
    std::abort();
}
