// J3D's own drawing entry points that Petari has no body for yet: each stops the program by name.
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <cstdio>
#include <cstdlib>

void J3DPacket::draw() {
    std::fprintf(stderr, "Not brought up yet: J3DPacket::draw\n");
    std::abort();
}
