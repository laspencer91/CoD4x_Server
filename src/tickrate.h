#ifndef COD4X_TICKRATE_H
#define COD4X_TICKRATE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define TICKRATE_PROTOCOL 1
#define TICKRATE_PACKET_BACKUP 256
#define TICKRATE_PACKET_MASK (TICKRATE_PACKET_BACKUP - 1)
#define TICKRATE_MAX_FPS 100
#define TICKRATE_ARCHIVE_SECONDS 60
#define TICKRATE_ARCHIVE_DECODE_SECONDS 2 /* retain the full-frame base at the oldest boundary */

/* These rates preserve the independent 50 ms missile and vehicle schedules exactly. */
static inline int Tickrate_FrameMsec(int fps)
{
    return fps == 20 || fps == 100 ? 1000 / fps : 0;
}

/* cl_deltaSlots: only the exact sidecar size is a capability; anything else is a stock client. */
static inline int Tickrate_DeltaCapacity(const char *value)
{
    return value && !strcmp(value, "256") ? TICKRATE_PACKET_BACKUP : 32;
}

/* Same window as the stock PACKET_BACKUP - 3 rule; capacity <= 256 keeps it inside the one-byte wire field. */
static inline int Tickrate_CanDelta(int age, int capacity)
{
    return age > 0 && age < capacity - 3;
}

static inline int Tickrate_ClientRate(int requested, int minimum, int maximum)
{
    int rate = requested >= 25000 ? maximum : requested;
    if (rate < minimum) rate = minimum;
    return rate > maximum ? maximum : rate;
}

static inline int Tickrate_ArchiveShift(int next, int bytes)
{
    return next >= 0x40000000 ? ((next - bytes) / bytes) * bytes : 0;
}

static inline int Tickrate_ArchiveValid(int start, int next, int bytes)
{
    return start >= 0 && start < next && (next < bytes || start >= next - bytes);
}

typedef struct {
    double remainder;
    uint32_t wake;
} tickrateWaitPhase_t;

/* Carry rounding only across an uninterrupted timed chain of the same thread: the thread's
 * own last wake (kept in its status word by the engine) must be this phase's wake and be now.
 * New or reused thread ids, late wakes and event resumes break the chain by themselves. */
static inline uint32_t Tickrate_Wait(tickrateWaitPhase_t *phase, double seconds,
                                    int frameMsec, uint32_t now, uint32_t priorWake)
{
    const uint32_t mask = 0xffffff;
    if (!isfinite(seconds) || seconds < 0 || frameMsec < 1) return mask;
    if (seconds == 0) return 0;
    double carry = phase->wake && phase->wake == priorWake && phase->wake == now ? phase->remainder : 0;
    double exact = seconds * 1000.0 / frameMsec + carry;
    if (exact >= mask - 0.5) return mask;
    uint32_t ticks = (uint32_t)floor(exact + 0.5);
    phase->remainder = ticks ? exact - ticks : 0;
    if (!ticks) ticks = 1;
    phase->wake = (now + ticks) & mask;
    return ticks;
}

#endif
