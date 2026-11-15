/* Freestanding stand-in for minimp3's unused <stdlib.h> include.
   bare-metal clang has no hosted C library headers. minimp3 includes this
   header in its implementation block but references none of its functions. */
#ifndef CANOPUS_CSTUB_STDLIB_H
#define CANOPUS_CSTUB_STDLIB_H
#include <stddef.h>
#endif
