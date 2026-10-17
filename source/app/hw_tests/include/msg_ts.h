#ifndef MSG_TS_H
#define MSG_TS_H

#include <stdint.h>
#include "message.h"

/* C version of tests/test_support/message_ts.h for the hardware tests and demos. */

/* Read a Message's 32-bit timestamp (stored as two 16-bit halves). */
static inline uint32_t msg_ts(const Message *m)
{
    return ((uint32_t)m->timestamp_hi << 16) | m->timestamp_lo;
}

/* Set a Message's 32-bit timestamp. */
static inline void msg_set_ts(Message *m, uint32_t ts)
{
    m->timestamp_hi = (uint16_t)(ts >> 16);
    m->timestamp_lo = (uint16_t)(ts & 0xFFFF);
}

#endif
