#include <smg/GxCommands.hpp>
#include <cstring>

namespace smg {
namespace {
std::uint32_t word(const std::uint8_t* at) { return std::uint32_t(at[0]) << 24 | std::uint32_t(at[1]) << 16 | std::uint32_t(at[2]) << 8 | at[3]; }
} // namespace

void GxCommandProcessor::loadBp(std::uint32_t value) {
    const std::uint32_t address = value >> 24, data = value & 0xFFFFFF;
    bp[address] = data;
    if (address == 0x45) host_.drawDone();                                    // PE_DONE
    else if (address == 0x47 || address == 0x48) host_.token(static_cast<std::uint16_t>(data), address == 0x48);  // PE_TOKEN(_INT)
}

void GxCommandProcessor::run(const std::uint8_t* at, std::uint32_t size) {
    while (size) {
        const std::uint32_t used = command(at, size);
        if (!used) {
            host_.unsupported("a display list ends inside a command", size);
            return;
        }
        at += used;
        size -= used;
    }
}

std::uint32_t GxCommandProcessor::command(const std::uint8_t* at, std::uint32_t available) {
    const std::uint8_t opcode = at[0];
    if (opcode == 0x00 || opcode == 0x48) return 1;  // NOP; invalidate the vertex cache, which the port has none of
    if (opcode == 0x08) {                           // load a CP register
        if (available < 6) return 0;
        cp[at[1]] = word(at + 2);
        return 6;
    }
    if (opcode == 0x10) {  // load XF registers: the count less one, then the first address
        if (available < 5) return 0;
        const std::uint32_t header = word(at + 1), count = (header >> 16) + 1, address = header & 0xFFFF;
        if (available < 5 + count * 4) return 0;
        for (std::uint32_t i = 0; i < count; ++i)
            if (address + i < sizeof xf / sizeof *xf) xf[address + i] = word(at + 5 + i * 4);
        return 5 + count * 4;
    }
    if (opcode == 0x61) {  // load a BP register
        if (available < 5) return 0;
        loadBp(word(at + 1));
        return 5;
    }
    if (opcode == 0x40) {  // call a display list at its physical address
        if (available < 9) return 0;
        run(host_.memory(word(at + 1)), word(at + 5));
        return 9;
    }
    if ((opcode & 0xE7) == 0x20) host_.unsupported("indexed XF loads are not the port's yet", opcode);
    else if (opcode >= 0x80 && opcode < 0xC0) host_.unsupported("drawing a primitive is not the port's yet (type, vertex format)", opcode);
    else host_.unsupported("an opcode the Wii's GPU does not know", opcode);
    // Only reached when `unsupported` returns (a test's host): nothing past a command of unknown length can be read.
    return available;
}

void GxCommandProcessor::put(const std::uint8_t* bytes, std::uint32_t count) {
    if (pendingSize_ + count > sizeof pending_) {
        host_.unsupported("a command longer than the FIFO's staging", pendingSize_ + count);
        return;
    }
    std::memcpy(pending_ + pendingSize_, bytes, count);
    pendingSize_ += count;
    std::uint32_t read = 0;
    while (read < pendingSize_) {
        const std::uint32_t used = command(pending_ + read, pendingSize_ - read);
        if (!used) break;
        read += used;
    }
    std::memmove(pending_, pending_ + read, pendingSize_ - read);
    pendingSize_ -= read;
}
} // namespace smg
