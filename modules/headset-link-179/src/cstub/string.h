/* Freestanding <string.h> for minimp3 on bare-metal clang.
   minimp3 uses exactly memcpy / memmove / memset. Macro-renaming them before
   the compiler sees the names prevents clang from emitting calls to the
   hosted libc symbols; the static inline bodies keep the object file free of
   undefined externals. */
#ifndef CANOPUS_CSTUB_STRING_H
#define CANOPUS_CSTUB_STRING_H
#include <stddef.h>

static inline void *mp3stub_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static inline void *mp3stub_memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

static inline void *mp3stub_memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

#define memcpy(d, s, n)  mp3stub_memcpy((d), (s), (n))
#define memmove(d, s, n) mp3stub_memmove((d), (s), (n))
#define memset(d, c, n)  mp3stub_memset((d), (c), (n))
#endif
