/*
 * LZ4 block decompressor -- see lz4_dec.h for scope.
 *
 * ------------------------------------------------------------------
 * The format, in full. This is all of it; there is nothing else.
 * ------------------------------------------------------------------
 *
 * A block is a series of "sequences". Each sequence is:
 *
 *   1. One token byte:
 *        high nibble = number of literal bytes that follow   (0..15)
 *        low  nibble = length of the match, minus 4          (0..15)
 *
 *   2. If the literal-length nibble is 15, the real length is larger and
 *      continues in extra bytes: read bytes and add them to 15 until one of
 *      them is not 255. (So 15 means "15 + whatever follows"; a 255 byte means
 *      "add 255 and keep reading".)
 *
 *   3. That many literal bytes, copied straight from input to output.
 *
 *   4. A 2-byte little-endian match offset, 1..65535, measured backwards from
 *      the current output position.
 *
 *   5. If the match-length nibble is 15, extra bytes extend it the same way as
 *      in step 2. Finally add 4, because a match is never shorter than 4 bytes
 *      (that is the MINMATCH constant -- shorter matches are not worth
 *      encoding, so the format reclaims those values).
 *
 *   6. Copy that many bytes from `output - offset` to the current output
 *      position.
 *
 * The block ends after the literals of the last sequence: if the input is
 * exhausted right after step 3, there is no match to read and we are done.
 * That is the only end marker the format has.
 *
 * ------------------------------------------------------------------
 * Why the match copy must sometimes go one byte at a time
 * ------------------------------------------------------------------
 *
 * A match may overlap the region it is being copied into. If offset is 1 and
 * the length is 100, the encoding means "repeat the previous byte 100 times".
 * A block-move (memcpy/memmove) is wrong here: the copy has to see the bytes
 * it just produced. So when offset < length, the copy is a strict forward
 * byte-by-byte loop. When offset >= length, source and destination cannot
 * overlap and the fast word-wise copy is safe.
 */

#include "lz4_dec.h"

#include <string.h>

/* A match is stored as (real length - 4), so 4 is added back on decode. */
#define LZ4_MIN_MATCH 4u

/*
 * Sanity cap on any single length field. The largest buffer we will ever hand
 * this decoder is one band of the framebuffer, tens of kilobytes; 16 MB is far
 * above that and still far below where a uint32_t would wrap, so a length that
 * exceeds it is proof the input is corrupt.
 */
#define LZ4_MAX_LEN 0x01000000u

/*
 * Unaligned 32-bit load/store.
 *
 * The Cortex-M4 permits unaligned word accesses in hardware (they are only
 * faulted if SCB->CCR.UNALIGN_TRP is set, which we never set). Writing it as a
 * 4-byte memcpy is the portable way to say "load these four bytes as a word":
 * GCC recognises the pattern and emits a single LDR/STR, and the same source
 * still compiles correctly on the host for the test harness.
 */
static inline uint32_t load_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void store_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

/*
 * Copy n bytes from s to d, where the two regions are known not to overlap.
 * Words first, then the leftover bytes. This is used for literals and for
 * non-overlapping matches.
 */
static inline void copy_nonoverlapping(uint8_t *d, const uint8_t *s, uint32_t n)
{
    while (n >= 4u) {
        store_u32(d, load_u32(s));
        d += 4;
        s += 4;
        n -= 4u;
    }
    while (n--)
        *d++ = *s++;
}

/*
 * Read an extended length field (steps 2 and 5 above).
 *
 * `*len` comes in holding the nibble from the token. If it is not 15 there is
 * nothing to do. Otherwise keep adding input bytes until one is below 255.
 * Advances *ip past whatever it consumed.
 */
static inline int read_extended_length(const uint8_t **ip, const uint8_t *iend,
                                       uint32_t *len)
{
    uint32_t v = *len;
    uint8_t b;

    if (v != 15u)
        return 0;

    do {
        if (*ip >= iend)
            return LZ4_DEC_E_INPUT;
        b = *(*ip)++;
        v += b;
        if (v > LZ4_MAX_LEN)
            return LZ4_DEC_E_INPUT;
    } while (b == 255u);

    *len = v;
    return 0;
}

int lz4_decompress_block(const uint8_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_cap)
{
    const uint8_t *ip = src;
    const uint8_t *iend = src + src_len;
    uint8_t *op = dst;
    uint8_t *oend = dst + dst_cap;

    if (src_len == 0u)
        return LZ4_DEC_E_INPUT;

    for (;;) {
        uint32_t lit_len, match_len, offset;
        const uint8_t *match;
        uint8_t token;
        int rc;

        if (ip >= iend)
            return LZ4_DEC_E_INPUT;
        token = *ip++;

        /* ---- literals ---- */

        lit_len = (uint32_t) (token >> 4);
        rc = read_extended_length(&ip, iend, &lit_len);
        if (rc < 0)
            return rc;

        if ((uint32_t) (iend - ip) < lit_len)
            return LZ4_DEC_E_INPUT;
        if ((uint32_t) (oend - op) < lit_len)
            return LZ4_DEC_E_OUTPUT;

        copy_nonoverlapping(op, ip, lit_len);
        op += lit_len;
        ip += lit_len;

        /* The block ends after the last sequence's literals. */
        if (ip == iend)
            break;

        /* ---- match ---- */

        if ((uint32_t) (iend - ip) < 2u)
            return LZ4_DEC_E_INPUT;
        offset = (uint32_t) ip[0] | ((uint32_t) ip[1] << 8);
        ip += 2;

        /* Offset 0 is not encodable, and the match must lie inside the part of
         * the output we have already produced. */
        if (offset == 0u || (uint32_t) (op - dst) < offset)
            return LZ4_DEC_E_OFFSET;

        match_len = (uint32_t) (token & 0x0Fu);
        rc = read_extended_length(&ip, iend, &match_len);
        if (rc < 0)
            return rc;
        match_len += LZ4_MIN_MATCH;

        if ((uint32_t) (oend - op) < match_len)
            return LZ4_DEC_E_OUTPUT;

        match = op - offset;

        if (offset >= match_len) {
            /* Regions cannot touch: safe to move whole words. */
            copy_nonoverlapping(op, match, match_len);
            op += match_len;
        } else {
            /* Overlapping: this is a repeating pattern and the copy must read
             * bytes it has itself just written. */
            uint32_t n = match_len;
            while (n--)
                *op++ = *match++;
        }
    }

    return (int) (op - dst);
}
