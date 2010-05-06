/* Copyright (c) 2009 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// RAMCloud pragma [CPPLINT=0]

#include <Common.h>

#include <ctype.h>

// Output a binary buffer in 'hexdump -C' style.
// Note that this exceeds 80 characters due to 64-bit offsets. Oh, well.
void
debug_dump64(const void *buf, uint64_t bytes)
{
    const unsigned char *cbuf = reinterpret_cast<const unsigned char *>(buf);
    uint64_t i, j;

    for (i = 0; i < bytes; i += 16) {
        char offset[17];
        char hex[16][3];
        char ascii[17];

        snprintf(offset, sizeof(offset), "%016" PRIx64, i);
        offset[sizeof(offset) - 1] = '\0';

        for (j = 0; j < 16; j++) {
            if ((i + j) >= bytes) {
                strcpy(hex[j], "  ");
                ascii[j] = '\0';
            } else {
                snprintf(hex[j], sizeof(hex[0]), "%02x",
                    cbuf[i + j]);
                hex[j][sizeof(hex[0]) - 1] = '\0';
                if (isprint((int)cbuf[i + j]))
                    ascii[j] = cbuf[i + j];
                else
                    ascii[j] = '.';
            }
        }
        ascii[sizeof(ascii) - 1] = '\0';

        printf("%s  %s %s %s %s %s %s %s %s  %s %s %s %s %s %s %s %s  "
            "|%s|\n", offset, hex[0], hex[1], hex[2], hex[3], hex[4],
            hex[5], hex[6], hex[7], hex[8], hex[9], hex[10], hex[11],
            hex[12], hex[13], hex[14], hex[15], ascii);
    }
}

#if PERF_COUNTERS
uint64_t
rdtsc()
{
    uint32_t lo, hi;

#ifdef __GNUC__
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
#else
    asm("rdtsc" : "=a" (lo), "=d" (hi));
#endif

    return (((uint64_t)hi << 32) | lo);
}
#endif

#ifdef __cplusplus

namespace RAMCloud {

#if PERF_COUNTERS
/**
 * Construct a CycleCounter, starting the timer.
 */
CycleCounter::CycleCounter()
    : total(NULL), startTime(rdtsc()) {
}

/**
 * Construct a CycleCounter, starting the timer.
 * \param total
 *      Where the elapsed time should be added once #stop() is called or the
 *      object is destructed. If you change your mind on this, use #cancel().
 */
CycleCounter::CycleCounter(uint64_t* total)
    : total(total), startTime(rdtsc()) {
}

/**
 * Destructor for CycleCounter, see #stop().
 */
CycleCounter::~CycleCounter() {
    stop();
}

/**
 * Stop the timer and discard the elapsed time.
 */
void
CycleCounter::cancel() {
    total = NULL;
    startTime = ~0UL;
}

/**
 * Stop the timer if it is running, and add the elapsed time to \a total as given
 * to the constructor.
 * \return
 *      The elapsed number of cycles if the timer was running (not previously
 *      stopped or canceled). Otherwise, 0 is returned.
 */
uint64_t
CycleCounter::stop() {
    if (startTime == ~0UL)
        return 0;
    uint64_t elapsed = rdtsc() - startTime;
    if (total != NULL)
        *total += elapsed;
    startTime = ~0UL;
    return elapsed;
}
#endif

void
assert(bool invariant) {
    if (!invariant)
        throw AssertionException();
}

}

#endif
