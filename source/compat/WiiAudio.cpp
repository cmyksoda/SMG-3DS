// The Wii's audio DSP, which JAudio boots with its own microcode. The port's audio will be its own; until then,
// booting the DSP stops the program by name.
#include <JSystem/JAudio2/dsptask.hpp>
#include <cstdio>
#include <cstdlib>

void DspBoot(void (*)(void*)) {
    std::fprintf(stderr, "The Wii's audio DSP (DspBoot) is not the port's yet\n");
    std::abort();
}
