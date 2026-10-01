// The Wii OS's threads, mutexes, message queues and clock on the 3DS kernel, through libctru. Wii threads share
// one core by priority; these stay on the app core, their priorities mapped in the same order.
#include <3ds.h>
#include <revolution/os.h>
#include <revolution/os/OSAlarm.h>
#include <revolution/os/OSMessage.h>
#include <revolution/os/OSMutex.h>
#include <revolution/os/OSThread.h>
#include <revolution/os/OSTime.h>
#include <smg/FreeStore.hpp>
#include <smg/Interrupts.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
[[noreturn]] void unported(const char* what) {
    std::fprintf(stderr, "The Wii SDK's %s is not the port's yet\n", what);
    std::abort();
}

// A mutex keeps its lock where the SDK keeps its waiting threads; owner and depth stay where the SDK has them.
static_assert(sizeof(LightLock) <= sizeof(OSThreadQueue));
LightLock* lockOf(OSMutex* mutex) { return reinterpret_cast<LightLock*>(&mutex->queue); }

// A message queue's lock and conditions sit in its two thread queues.
struct QueueSync {
    LightLock lock;
    CondVar notFull, notEmpty;
};
static_assert(sizeof(QueueSync) <= 2 * sizeof(OSThreadQueue));
QueueSync* syncOf(OSMessageQueue* queue) { return reinterpret_cast<QueueSync*>(&queue->queueSend); }

// What the 3DS side of a Wii thread needs; the game allocates the OSThread itself, so this lives beside it.
struct Native {
    void* (*function)(void*);
    void* parameter;
    u32 stackSize;
    Thread thread = nullptr;
    LightEvent resumed;
    void* result = nullptr;
};
LightLock sTableLock = 1;
smg::FreeMap<OSThread*, Native*>& natives() {
    static smg::FreeMap<OSThread*, Native*> table;
    return table;
}
Native* nativeOf(OSThread* thread) {
    LightLock_Lock(&sTableLock);
    const auto found = natives().find(thread);
    Native* native = found == natives().end() ? nullptr : found->second;
    LightLock_Unlock(&sTableLock);
    return native;
}

// The game's main thread is the Wii's default thread at priority 16.
OSThread sDefaultThread;
thread_local OSThread* tCurrent = nullptr;

// Wii priorities run 0 (first) to 31, the default thread at 16; 3DS app threads 0x18 to 0x3F, main at 0x30.
int nativePriority(OSPriority priority) {
    const int mapped = 0x30 + (priority - 16);
    return mapped < 0x18 ? 0x18 : mapped > 0x3F ? 0x3F : mapped;
}

void run(void* argument) {
    auto* thread = static_cast<OSThread*>(argument);
    Native* native = nativeOf(thread);
    tCurrent = thread;
    thread->state = OS_THREAD_STATE_RUNNING;
    OSExitThread(native->function(native->parameter));
}

// OS_TIMER_CLOCK ticks per second; the 3DS system tick runs at SYSCLOCK_ARM11.
constexpr u64 sTimerClock = 243000000 / 4;
u64 toNanoseconds(OSTime ticks) { return ticks <= 0 ? 0 : static_cast<u64>(ticks) / sTimerClock * 1000000000ULL + static_cast<u64>(ticks) % sTimerClock * 1000000000ULL / sTimerClock; }
} // namespace

extern "C" {
OSThread* OSGetCurrentThread() {
    if (!tCurrent) {
        // Only the main thread gets here: every thread made by OSCreateThread sets its own on entry.
        if (sDefaultThread.state == 0) {
            sDefaultThread.state = OS_THREAD_STATE_RUNNING;
            sDefaultThread.priority = sDefaultThread.base = 16;
        }
        tCurrent = &sDefaultThread;
    }
    return tCurrent;
}

void OSInitMutex(OSMutex* mutex) {
    std::memset(mutex, 0, sizeof *mutex);
    LightLock_Init(lockOf(mutex));
}

// Recursive, as the SDK's: the owner takes it again by counting.
void OSLockMutex(OSMutex* mutex) {
    OSThread* self = OSGetCurrentThread();
    if (mutex->thread == self) {
        ++mutex->count;
        return;
    }
    if (LightLock_TryLock(lockOf(mutex)) != 0) {
        const smg::InterruptsReleased released;
        LightLock_Lock(lockOf(mutex));
    }
    mutex->thread = self;
    mutex->count = 1;
}

void OSUnlockMutex(OSMutex* mutex) {
    // The SDK ignores an unlock from a thread that does not hold it.
    if (mutex->thread != OSGetCurrentThread()) return;
    if (--mutex->count == 0) {
        mutex->thread = nullptr;
        LightLock_Unlock(lockOf(mutex));
    }
}

BOOL OSTryLockMutex(OSMutex* mutex) {
    OSThread* self = OSGetCurrentThread();
    if (mutex->thread == self) {
        ++mutex->count;
        return TRUE;
    }
    if (LightLock_TryLock(lockOf(mutex)) != 0) return FALSE;
    mutex->thread = self;
    mutex->count = 1;
    return TRUE;
}

void OSInitMessageQueue(OSMessageQueue* queue, OSMessage* messages, s32 count) {
    QueueSync* sync = syncOf(queue);
    LightLock_Init(&sync->lock);
    CondVar_Init(&sync->notFull);
    CondVar_Init(&sync->notEmpty);
    queue->msgArray = messages;
    queue->msgCount = count;
    queue->firstIndex = 0;
    queue->usedCount = 0;
}

// A full queue makes a blocking sender wait and a non-blocking one fail, as the SDK's does; Jam puts the
// message first in line. A wait is tried first without it, so only a real one lets interrupts in.
static BOOL putMessage(OSMessageQueue* queue, OSMessage message, bool wait, bool first) {
    QueueSync* sync = syncOf(queue);
    LightLock_Lock(&sync->lock);
    while (queue->usedCount >= queue->msgCount) {
        if (!wait) {
            LightLock_Unlock(&sync->lock);
            return FALSE;
        }
        CondVar_Wait(&sync->notFull, &sync->lock);
    }
    if (first) {
        queue->firstIndex = (queue->firstIndex + queue->msgCount - 1) % queue->msgCount;
        queue->msgArray[queue->firstIndex] = message;
    } else {
        queue->msgArray[(queue->firstIndex + queue->usedCount) % queue->msgCount] = message;
    }
    ++queue->usedCount;
    CondVar_Broadcast(&sync->notEmpty);
    LightLock_Unlock(&sync->lock);
    return TRUE;
}

static BOOL sendMessage(OSMessageQueue* queue, OSMessage message, s32 flags, bool first) {
    if (putMessage(queue, message, false, first)) return TRUE;
    if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
    const smg::InterruptsReleased released;
    return putMessage(queue, message, true, first);
}

BOOL OSSendMessage(OSMessageQueue* queue, OSMessage message, s32 flags) { return sendMessage(queue, message, flags, false); }
BOOL OSJamMessage(OSMessageQueue* queue, OSMessage message, s32 flags) { return sendMessage(queue, message, flags, true); }

static BOOL takeMessage(OSMessageQueue* queue, OSMessage* message, bool wait) {
    QueueSync* sync = syncOf(queue);
    LightLock_Lock(&sync->lock);
    while (queue->usedCount == 0) {
        if (!wait) {
            LightLock_Unlock(&sync->lock);
            return FALSE;
        }
        CondVar_Wait(&sync->notEmpty, &sync->lock);
    }
    if (message) *message = queue->msgArray[queue->firstIndex];
    queue->firstIndex = (queue->firstIndex + 1) % queue->msgCount;
    --queue->usedCount;
    CondVar_Broadcast(&sync->notFull);
    LightLock_Unlock(&sync->lock);
    return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* queue, OSMessage* message, s32 flags) {
    if (takeMessage(queue, message, false)) return TRUE;
    if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
    const smg::InterruptsReleased released;
    return takeMessage(queue, message, true);
}

// A thread starts suspended, as the SDK makes it, and first runs when OSResumeThread brings its count to zero.
// Its stack is the 3DS kernel's, as large as the one the game set aside.
BOOL OSCreateThread(OSThread* thread, void* (*function)(void*), void* parameter, void* stackTop, u32 stackSize, OSPriority priority,
                    u16 attributes) {
    if (priority < 0 || priority > 31) return FALSE;
    std::memset(thread, 0, sizeof *thread);
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attributes;
    thread->suspend = 1;
    thread->priority = thread->base = priority;
    thread->stackBase = static_cast<u8*>(stackTop);
    thread->stackEnd = reinterpret_cast<u32*>(static_cast<u8*>(stackTop) - stackSize);
    auto* native = new (std::malloc(sizeof(Native))) Native{function, parameter, stackSize};
    LightEvent_Init(&native->resumed, RESET_ONESHOT);
    LightLock_Lock(&sTableLock);
    natives()[thread] = native;
    LightLock_Unlock(&sTableLock);
    return TRUE;
}

s32 OSResumeThread(OSThread* thread) {
    const s32 previous = thread->suspend;
    if (previous > 0 && --thread->suspend == 0) {
        Native* native = nativeOf(thread);
        if (!native) unported("OSResumeThread of a thread OSCreateThread did not make");
        if (!native->thread) {
            // At least libctru's own default, which its newlib reentrancy needs.
            const size_t stack = native->stackSize < 0x8000 ? 0x8000 : native->stackSize;
            native->thread = threadCreate(run, thread, stack, nativePriority(thread->priority), -2, false);
            if (!native->thread) unported("OSResumeThread: the 3DS kernel made no thread");
        } else {
            LightEvent_Signal(&native->resumed);
        }
    }
    return previous;
}

// Only a thread's own suspension can wait on the 3DS; stopping another running thread has no counterpart.
s32 OSSuspendThread(OSThread* thread) {
    const s32 previous = thread->suspend++;
    Native* native = nativeOf(thread);
    if (previous > 0 || !native || !native->thread) return previous;
    if (thread != OSGetCurrentThread()) unported("OSSuspendThread of another running thread");
    const smg::InterruptsReleased released;
    while (thread->suspend > 0) LightEvent_Wait(&native->resumed);
    return previous;
}

BOOL OSIsThreadSuspended(OSThread* thread) { return thread->suspend > 0; }
BOOL OSIsThreadTerminated(OSThread* thread) { return thread->state == OS_THREAD_STATE_MORIBUND || thread->state == 0; }

void OSExitThread(void* result) {
    OSThread* self = OSGetCurrentThread();
    if (Native* native = nativeOf(self)) native->result = result;
    self->value = result;
    self->state = OS_THREAD_STATE_MORIBUND;
    threadExit(0);
}

BOOL OSJoinThread(OSThread* thread, void** result) {
    Native* native = nativeOf(thread);
    if (!native || !native->thread) return FALSE;
    {
        const smg::InterruptsReleased released;
        threadJoin(native->thread, U64_MAX);
    }
    threadFree(native->thread);
    native->thread = nullptr;
    if (result) *result = native->result;
    return TRUE;
}

void OSDetachThread(OSThread* thread) {
    thread->attr |= 1;  // OS_THREAD_ATTR_DETACH
    if (Native* native = nativeOf(thread); native && native->thread) threadDetach(native->thread);
}

// A thread that never ran can simply end; ending one mid-run has no safe counterpart on the 3DS.
void OSCancelThread(OSThread* thread) {
    Native* native = nativeOf(thread);
    if (native && native->thread && thread->state != OS_THREAD_STATE_MORIBUND) unported("OSCancelThread of a running thread");
    thread->state = OS_THREAD_STATE_MORIBUND;
}

// Threads asleep on a queue wake together when it is woken, as the SDK's do. Each queue counts its wake-ups, so a
// sleeper waits for the next one after it lay down.
namespace {
struct Sleepers {
    CondVar woken;
    u32 wakeups = 0;
};
LightLock sSleepLock = 1;
smg::FreeMap<OSThreadQueue*, Sleepers>& sleepers() {
    static smg::FreeMap<OSThreadQueue*, Sleepers> queues;
    return queues;
}
} // namespace

void OSInitThreadQueue(OSThreadQueue* queue) {
    queue->head = queue->tail = nullptr;
}

// The SDK sleeps with interrupts off until a handler wakes the queue: the wake-up count is noted before they come
// on, and they come on without the sleep lock held, since a handler run then may wake this very queue.
void OSSleepThread(OSThreadQueue* queue) {
    LightLock_Lock(&sSleepLock);
    Sleepers& waiting = sleepers()[queue];
    if (waiting.wakeups == 0 && !waiting.woken) CondVar_Init(&waiting.woken);
    const u32 lay = waiting.wakeups;
    LightLock_Unlock(&sSleepLock);
    const smg::InterruptsReleased released;
    LightLock_Lock(&sSleepLock);
    while (waiting.wakeups == lay) CondVar_Wait(&waiting.woken, &sSleepLock);
    LightLock_Unlock(&sSleepLock);
}

void OSWakeupThread(OSThreadQueue* queue) {
    LightLock_Lock(&sSleepLock);
    Sleepers& waiting = sleepers()[queue];
    ++waiting.wakeups;
    CondVar_Broadcast(&waiting.woken);
    LightLock_Unlock(&sSleepLock);
}

void OSYieldThread() {
    const smg::InterruptsReleased released;
    svcSleepThread(0);
}
void OSSleepTicks(OSTime ticks) {
    const smg::InterruptsReleased released;
    svcSleepThread(static_cast<s64>(toNanoseconds(ticks)));
}

// The time base counts OS_TIMER_CLOCK ticks from 2000-01-01, as the Wii's does: set from the 3DS clock once,
// then advanced by its system tick.
OSTime OSGetTime() {
    static const u64 startTick = svcGetSystemTick();
    static const OSTime start = static_cast<OSTime>((osGetTime() - 3155673600000ULL) * (sTimerClock / 1000));
    const u64 elapsed = svcGetSystemTick() - startTick;
    return start + static_cast<OSTime>(elapsed / SYSCLOCK_ARM11 * sTimerClock + elapsed % SYSCLOCK_ARM11 * sTimerClock / SYSCLOCK_ARM11);
}

OSTick OSGetTick() { return static_cast<OSTick>(OSGetTime()); }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* calendar) {
    s64 days = ticks / (static_cast<s64>(sTimerClock) * 86400);
    s64 rest = ticks % (static_cast<s64>(sTimerClock) * 86400);
    if (rest < 0) {
        rest += static_cast<s64>(sTimerClock) * 86400;
        --days;
    }
    const s64 microseconds = rest / (sTimerClock / 1000000);
    calendar->usec = microseconds % 1000;
    calendar->msec = microseconds / 1000 % 1000;
    const s64 seconds = microseconds / 1000000;
    calendar->sec = seconds % 60;
    calendar->min = seconds / 60 % 60;
    calendar->hour = seconds / 3600;
    calendar->wday = static_cast<int>(((days + 6) % 7 + 7) % 7);  // 2000-01-01 was a Saturday
    // Days since 2000-01-01 to a civil date (Howard Hinnant's algorithm, from 0000-03-01).
    const s64 z = days + 730425;
    const s64 era = (z >= 0 ? z : z - 146096) / 146097;
    const s64 dayOfEra = z - era * 146097;
    const s64 yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const s64 dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const s64 shiftedMonth = (5 * dayOfYear + 2) / 153;
    calendar->mday = static_cast<int>(dayOfYear - (153 * shiftedMonth + 2) / 5 + 1);
    calendar->mon = static_cast<int>(shiftedMonth < 10 ? shiftedMonth + 2 : shiftedMonth - 10);  // 0 = January
    const s64 year = yearOfEra + era * 400 + (calendar->mon <= 1);
    calendar->year = static_cast<int>(year);
    static const int monthStarts[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    calendar->yday = monthStarts[calendar->mon] + calendar->mday - 1 + (leap && calendar->mon > 1);
}

OSPriority OSGetThreadPriority(OSThread* thread) { return thread->priority; }

BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority) {
    if (priority < 0 || priority > 31) return FALSE;
    thread->priority = thread->base = priority;
    const Native* native = thread == &sDefaultThread ? nullptr : nativeOf(thread);
    if (native && native->thread) svcSetThreadPriority(threadGetHandle(native->thread), nativePriority(priority));
    else if (thread == OSGetCurrentThread()) svcSetThreadPriority(CUR_THREAD_HANDLE, nativePriority(priority));
    return TRUE;
}
}

// Alarms, kept as the SDK keeps them: a list in firing order through their own links. A thread above every game
// thread sleeps until the first is due and runs its handler, as the Wii's decrementer interrupt did.
namespace {
LightLock sAlarmLock = 1;
LightEvent sAlarmChanged;
OSAlarm* sFirstAlarm = nullptr;

void unlinkAlarm(OSAlarm* alarm) {
    if (alarm->prev) alarm->prev->next = alarm->next;
    else if (sFirstAlarm == alarm) sFirstAlarm = alarm->next;
    if (alarm->next) alarm->next->prev = alarm->prev;
    alarm->prev = alarm->next = nullptr;
}

// A periodic alarm that fell behind fires next at its first due time still ahead, as the SDK's InsertAlarm does.
void insertAlarm(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler) {
    if (alarm->period > 0) {
        const OSTime now = OSGetTime();
        fire = alarm->start;
        if (alarm->start < now) fire += alarm->period * ((now - alarm->start) / alarm->period + 1);
    }
    alarm->handler = handler;
    alarm->fire = fire;
    OSAlarm** at = &sFirstAlarm;
    OSAlarm* previous = nullptr;
    while (*at && (*at)->fire <= fire) previous = *at, at = &(*at)->next;
    alarm->prev = previous;
    alarm->next = *at;
    if (*at) (*at)->prev = alarm;
    *at = alarm;
}

void alarms(void*) {
    while (true) {
        LightLock_Lock(&sAlarmLock);
        OSAlarm* due = sFirstAlarm;
        const OSTime now = OSGetTime();
        if (due && due->fire <= now) {
            // A one-shot alarm is idle again before its handler runs, which may set it anew.
            const OSAlarmHandler handler = due->handler;
            unlinkAlarm(due);
            due->handler = nullptr;
            if (due->period > 0) insertAlarm(due, 0, handler);
            LightLock_Unlock(&sAlarmLock);
            OSContext context;
            std::memset(&context, 0, sizeof context);
            const smg::AsInterrupt decrementer;
            handler(due, &context);
            continue;
        }
        const s64 wait = due ? static_cast<s64>(toNanoseconds(due->fire - now)) : -1;
        LightLock_Unlock(&sAlarmLock);
        if (wait < 0) LightEvent_Wait(&sAlarmChanged);
        else LightEvent_WaitTimeout(&sAlarmChanged, wait);
    }
}

void startAlarms() {
    static Thread thread = nullptr;
    if (thread) return;
    LightEvent_Init(&sAlarmChanged, RESET_ONESHOT);
    thread = threadCreate(alarms, nullptr, 0x4000, 0x18, -2, true);
    if (!thread) unported("OSSetAlarm: the 3DS kernel made no alarm thread");
}

void setAlarm(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler) {
    startAlarms();
    LightLock_Lock(&sAlarmLock);
    if (alarm->handler) unlinkAlarm(alarm);
    insertAlarm(alarm, fire, handler);
    LightLock_Unlock(&sAlarmLock);
    LightEvent_Signal(&sAlarmChanged);
}
} // namespace

extern "C" {
void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = nullptr;
    alarm->tag = 0;
}

void OSSetAlarm(OSAlarm* alarm, OSTime ticks, OSAlarmHandler handler) {
    alarm->period = 0;
    setAlarm(alarm, OSGetTime() + ticks, handler);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    alarm->period = period;
    alarm->start = start;
    setAlarm(alarm, 0, handler);
}

void OSCancelAlarm(OSAlarm* alarm) {
    LightLock_Lock(&sAlarmLock);
    if (alarm->handler) unlinkAlarm(alarm);
    alarm->handler = nullptr;
    LightLock_Unlock(&sAlarmLock);
    LightEvent_Signal(&sAlarmChanged);
}
}
