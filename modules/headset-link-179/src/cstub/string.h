/* Freestanding <string.h> for minimp3 on bare-metal clang.
   minimp3 uses exactly memcpy / memmove / memset. Macro-renaming them before
   the compiler sees the names prevents clang from emitting calls to the
   hosted libc symbols; the static inline bodies keep the object file free of
   undefined externals.

   0.10.19: the original byte loops were the main cause of the very slow MP3
   probe (minimp3 moves large PCM/Huffman buffers continuously). The bodies
   now copy/fill in 32-bit words whenever both pointers are aligned. The
   may_alias typedef keeps the word accesses well-defined under -Oz. */
#ifndef CANOPUS_CSTUB_STRING_H
#define CANOPUS_CSTUB_STRING_H
#include <stddef.h>
#include <stdint.h>

typedef uint32_t mp3_word __attribute__((__may_alias__));

static inline void *mp3stub_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0) {
        while (n >= 4 && ((uintptr_t)d & 3u) != 0) {
            *d++ = *s++;
            n--;
        }
        size_t words = n >> 2;
        mp3_word *dw = (mp3_word *)d;
        const mp3_word *sw = (const mp3_word *)s;
        for (size_t i = 0; i < words; i++) dw[i] = sw[i];
        d += words << 2;
        s += words << 2;
        n &= 3;
    }
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static inline void *mp3stub_memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        /* Forward direction is overlap-safe; use the word path when aligned. */
        return mp3stub_memcpy(dst, src, n);
    }
    if (d > s) {
        /* Backward, byte-wise: word stores could clobber unread source bytes
           when the overlap is smaller than a word. */
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

static inline void *mp3stub_memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    uint32_t w = (uint32_t)(unsigned char)c;
    w |= w << 8;
    w |= w << 16;
    while (n >= 4 && ((uintptr_t)d & 3u) != 0) {
        *d++ = (unsigned char)c;
        n--;
    }
    size_t words = n >> 2;
    mp3_word *dw = (mp3_word *)d;
    for (size_t i = 0; i < words; i++) dw[i] = w;
    d += words << 2;
    n &= 3;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

#define memcpy(d, s, n)  mp3stub_memcpy((d), (s), (n))
#define memmove(d, s, n) mp3stub_memmove((d), (s), (n))
#define memset(d, c, n)  mp3stub_memset((d), (c), (n))
#endif
