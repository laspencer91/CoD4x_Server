#include "../../src/tickrate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An uninterrupted chain: the thread wakes on schedule, so its prior wake is the current time. */
static void wait_chain(int step, double seconds, int count, int expected_ms)
{
    tickrateWaitPhase_t phase = {0};
    uint32_t time = 0;
    for (int i = 0; i < count; ++i) time += Tickrate_Wait(&phase, seconds, step, time, time);
    assert(abs((int)time * step - expected_ms) <= step / 2);
}

int main(void)
{
    assert(Tickrate_FrameMsec(20) == 50);
    assert(Tickrate_FrameMsec(100) == 10);
    assert(!Tickrate_FrameMsec(125));
    assert(!Tickrate_FrameMsec(30) && !Tickrate_FrameMsec(0) && !Tickrate_FrameMsec(250));
    const char *invalid[] = {"", "0", "-256", "9999999999999", "256x", " 256", "512", "128", "+256", "0256"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(Tickrate_DeltaCapacity(invalid[i]) == 32);
    assert(Tickrate_DeltaCapacity(NULL) == 32);
    assert(Tickrate_DeltaCapacity("256") == 256);
    for (int age = -5; age < 600; ++age) {
        assert(Tickrate_CanDelta(age, 32) == (age > 0 && age < 29));
        assert(Tickrate_CanDelta(age, 256) == (age > 0 && age < 253));
    }
    assert(Tickrate_ClientRate(5000, 2500, 100000) == 5000);
    assert(Tickrate_ClientRate(25000, 2500, 100000) == 100000);
    assert(Tickrate_ClientRate(5000, 100000, 40000) == 40000);
    assert(Tickrate_ClientRate(50000, 100000, 40000) == 40000);
    for (int step = 8; step <= 50; ++step) {
        wait_chain(step, .05, 200, 10000);
        wait_chain(step, .1, 600, 60000);
        wait_chain(step, 1, 60, 60000);
    }
    tickrateWaitPhase_t phase = {0};
    assert(Tickrate_Wait(&phase, .05, 8, 0, 0) == 6 && phase.wake == 6);
    assert(Tickrate_Wait(&phase, .05, 8, 6, 6) == 7 && phase.wake == 13);
    // Late wake, event resume (prior wake is a notify string id) and a reused thread id
    // (prior wake zero) cannot borrow the chain's fractional phase; an on-time wake can.
    tickrateWaitPhase_t late = phase, event = phase, reused = phase;
    assert(Tickrate_Wait(&late, .05, 8, 14, 13) == 6);
    assert(Tickrate_Wait(&event, .05, 8, 13, 500) == 6);
    assert(Tickrate_Wait(&reused, .05, 8, 13, 0) == 6);
    assert(Tickrate_Wait(&phase, .05, 8, 13, 13) == 6 && phase.wake == 19);
    tickrateWaitPhase_t before = phase;
    assert(Tickrate_Wait(&phase, 0, 8, 26, 26) == 0 && !memcmp(&before, &phase, sizeof(phase)));
    assert(Tickrate_Wait(&phase, INFINITY, 8, 0, 0) == 0xffffff);
    assert(Tickrate_Wait(&phase, NAN, 8, 0, 0) == 0xffffff);
    assert(Tickrate_Wait(&phase, -1, 8, 0, 0) == 0xffffff);
    assert(Tickrate_Wait(&phase, 1, 0, 0, 0) == 0xffffff);
    assert(Tickrate_Wait(&phase, 1e9, 8, 0, 0) == 0xffffff);
    tickrateWaitPhase_t wrap = {0};
    assert(Tickrate_Wait(&wrap, .05, 8, 0xfffffd, 0) == 6 && wrap.wake == 3);
    assert(Tickrate_Wait(&wrap, .05, 8, 3, 3) == 7);
    const int ring = 0x8000000;
    for (int tail = 0; tail < 1000; ++tail) {
        int next = 0x40000000 + tail * 1000;
        int shift = Tickrate_ArchiveShift(next, ring);
        assert(shift > 0 && shift % ring == 0);
        for (int offset = -2; offset <= 2; ++offset) {
            int start = next - ring + offset;
            assert(Tickrate_ArchiveValid(start, next, ring) ==
                   Tickrate_ArchiveValid(start - shift, next - shift, ring));
        }
        assert(next % ring == (next - shift) % ring);
        assert(!Tickrate_ArchiveValid(next, next, ring));
        assert(!Tickrate_ArchiveValid(-1, next, ring));
    }
    puts("tickrate policy, fractional waits, and archive rollover checks passed");
    return 0;
}
