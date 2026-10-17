// N3DS_TIMED(what, statement): on the 3DS, the statement's time goes to
// the log's measure what (N3DS_PROFILE_...); elsewhere, just the statement.

#ifndef N3DS_TIMED_H
#define N3DS_TIMED_H

#ifdef __3DS__
#include "n3ds/n3ds.h"

#define N3DS_TIMED(what, statement) \
  do { \
    const unsigned long long n3ds_timed_start = N3DS_ProfileNow(); \
    statement; \
    N3DS_ProfileAdd(what, n3ds_timed_start); \
  } while (0)
#else
#define N3DS_TIMED(what, statement) statement
#endif

#endif
