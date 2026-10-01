#pragma once
// The Wii's interrupts off is its one CPU held (source/compat/Os.cpp). Lock order: the CPU before the GX FIFO, so
// nothing holding the FIFO may wait for the CPU (a device raising an interrupt only tries it).

namespace smg {
// A thread waiting in the OS with interrupts off lets them on for the wait, as the Wii's scheduler loads the next
// thread's own machine state; they are off again before the wait returns.
class InterruptsReleased {
public:
    InterruptsReleased();
    ~InterruptsReleased();
    InterruptsReleased(const InterruptsReleased&) = delete;
    InterruptsReleased& operator=(const InterruptsReleased&) = delete;

private:
    bool held_;
};

// A port thread standing in for one of the Wii's interrupts (an alarm, the retrace, a disc transfer) runs the
// game's handler with interrupts off, as the Wii runs it.
class AsInterrupt {
public:
    AsInterrupt();
    ~AsInterrupt();
    AsInterrupt(const AsInterrupt&) = delete;
    AsInterrupt& operator=(const AsInterrupt&) = delete;

private:
    bool held_;
};
} // namespace smg
