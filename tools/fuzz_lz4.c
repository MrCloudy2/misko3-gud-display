/*
 * Host tool: correctness check for lz4_decompress_block().
 *
 * Two things are being proved here, and they are different:
 *
 *  1. VALID INPUT -- for data compressed by the reference liblz4 (both the
 *     fast and the high-compression encoder, since they emit different but
 *     equally legal sequences), our decoder must reproduce the original bytes
 *     exactly. Compared byte-for-byte against the source data.
 *
 *  2. INVALID INPUT -- for corrupted or truncated blocks, our decoder must
 *     either fail or produce something, but it must never write outside the
 *     destination buffer. That is checked with guard bands of a known
 *     byte pattern on both sides of the output buffer: if any guard byte
 *     changes, the decoder overran and the test fails.
 *
 * The second half matters because this code runs on a Cortex-M4 with no MMU:
 * a buffer overrun there silently corrupts whatever variable happens to live
 * next door. The bounds checks in lz4_dec.c are the only thing standing
 * between a bad USB packet and a wild write.
 *
 * Build and run:  make -C m1_lz4/host
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <lz4.h>
#include <lz4hc.h>

#include "../src/lz4_dec.h"

#define MAX_RAW   70000u
#define GUARD     64u
#define GUARD_BYTE 0xA5u

static uint8_t raw[MAX_RAW];
static uint8_t comp[MAX_RAW * 2];
static uint8_t out[GUARD + MAX_RAW + 512u + GUARD];

static uint32_t rng_state = 0xDEADBEEFu;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/*
 * Generate raw data with a chosen amount of structure.
 *
 * `style` selects how repetitive the data is, because the *shape* of the LZ4
 * sequences depends on it and different shapes exercise different branches:
 *   0  constant           -> long overlapping matches at offset 1
 *   1  short period       -> overlapping matches at offset 2..8
 *   2  long period        -> non-overlapping matches, the memcpy path
 *   3  random             -> all literals, and often incompressible
 *   4  mixed runs         -> a bit of everything, plus extended length codes
 */
static void make_raw(uint32_t len, int style)
{
    switch (style) {
    case 0:
        memset(raw, (int) (rnd() & 0xFF), len);
        break;
    case 1: {
        uint32_t period = 1u + (rnd() % 8u);
        uint8_t pat[8];
        for (uint32_t i = 0; i < period; i++)
            pat[i] = (uint8_t) rnd();
        for (uint32_t i = 0; i < len; i++)
            raw[i] = pat[i % period];
        break;
    }
    case 2: {
        uint32_t period = 64u + (rnd() % 4000u);
        for (uint32_t i = 0; i < len; i++)
            raw[i] = (i < period) ? (uint8_t) rnd() : raw[i - period];
        break;
    }
    case 3:
        for (uint32_t i = 0; i < len; i++)
            raw[i] = (uint8_t) rnd();
        break;
    default: {
        uint32_t i = 0;
        while (i < len) {
            uint32_t run = 1u + (rnd() % 300u);
            if (run > len - i)
                run = len - i;
            if (rnd() & 1u) {
                uint8_t v = (uint8_t) rnd();
                memset(raw + i, v, run);
            } else {
                for (uint32_t k = 0; k < run; k++)
                    raw[i + k] = (uint8_t) rnd();
            }
            i += run;
        }
        break;
    }
    }
}

static void guards_set(void)
{
    memset(out, GUARD_BYTE, sizeof(out));
}

static int guards_intact(uint32_t cap)
{
    for (uint32_t i = 0; i < GUARD; i++)
        if (out[i] != GUARD_BYTE)
            return 0;
    for (uint32_t i = 0; i < GUARD; i++)
        if (out[GUARD + cap + i] != GUARD_BYTE)
            return 0;
    return 1;
}

int main(void)
{
    uint32_t valid_cases = 0, corrupt_cases = 0, rejected = 0;

    /* ---- part 1: valid blocks must round-trip exactly ---- */

    for (uint32_t iter = 0; iter < 20000u; iter++) {
        /* Bias towards small lengths, where the end-of-block rules bite, but
         * still cover full band-sized buffers. */
        uint32_t len = (iter % 4u == 0u)
                     ? (rnd() % 64u) + 1u
                     : (rnd() % MAX_RAW) + 1u;
        int style = (int) (iter % 5u);
        int hc = (int) (iter % 3u == 0u);

        make_raw(len, style);

        int clen = hc
            ? LZ4_compress_HC((const char *) raw, (char *) comp,
                              (int) len, (int) sizeof(comp), 9)
            : LZ4_compress_default((const char *) raw, (char *) comp,
                                   (int) len, (int) sizeof(comp));
        if (clen <= 0) {
            fprintf(stderr, "compression failed at iter %u len %u\n", iter, len);
            return 1;
        }

        guards_set();
        int got = lz4_decompress_block(comp, (uint32_t) clen, out + GUARD, len);

        if (got != (int) len) {
            fprintf(stderr, "iter %u style %d hc %d len %u: got %d\n",
                    iter, style, hc, len, got);
            return 1;
        }
        if (memcmp(out + GUARD, raw, len) != 0) {
            fprintf(stderr, "iter %u style %d hc %d len %u: content mismatch\n",
                    iter, style, hc, len);
            return 1;
        }
        if (!guards_intact(len)) {
            fprintf(stderr, "iter %u: guard bytes clobbered on valid input\n",
                    iter);
            return 1;
        }
        valid_cases++;
    }

    /*
     * ---- part 1b: an output buffer one byte too small must be refused ----
     *
     * This is the case that protects us if the host ever lies about `length`
     * in GUD_REQ_SET_BUFFER, or if a band boundary is miscomputed.
     */
    for (uint32_t iter = 0; iter < 2000u; iter++) {
        uint32_t len = (rnd() % 8192u) + 16u;
        make_raw(len, (int) (iter % 5u));

        int clen = LZ4_compress_default((const char *) raw, (char *) comp,
                                        (int) len, (int) sizeof(comp));
        if (clen <= 0)
            continue;

        guards_set();
        int got = lz4_decompress_block(comp, (uint32_t) clen, out + GUARD, len - 1u);
        if (got >= 0) {
            fprintf(stderr, "undersized output accepted at iter %u\n", iter);
            return 1;
        }
        if (!guards_intact(len - 1u)) {
            fprintf(stderr, "undersized output overran at iter %u\n", iter);
            return 1;
        }
    }

    /* ---- part 2: corrupt input must not overrun ---- */

    for (uint32_t iter = 0; iter < 40000u; iter++) {
        uint32_t len = (rnd() % 20000u) + 16u;
        make_raw(len, (int) (iter % 5u));

        int clen = LZ4_compress_default((const char *) raw, (char *) comp,
                                        (int) len, (int) sizeof(comp));
        if (clen <= 0)
            continue;

        /* Three ways to break a block: truncate it, flip bytes in it, or
         * both. Truncation is what a short USB transfer looks like; byte
         * flips are what line noise looks like. */
        uint32_t use = (uint32_t) clen;
        if (rnd() & 1u)
            use = 1u + (rnd() % (uint32_t) clen);

        uint32_t flips = rnd() % 8u;
        for (uint32_t f = 0; f < flips; f++)
            comp[rnd() % use] = (uint8_t) rnd();

        guards_set();
        int got = lz4_decompress_block(comp, use, out + GUARD, len);

        if (!guards_intact(len)) {
            fprintf(stderr, "GUARD CLOBBERED: iter %u clen %d use %u got %d\n",
                    iter, clen, use, got);
            return 1;
        }
        if (got > (int) len) {
            fprintf(stderr, "over-long result: iter %u got %d cap %u\n",
                    iter, got, len);
            return 1;
        }
        if (got < 0)
            rejected++;
        corrupt_cases++;
    }

    /*
     * ---- part 3: in-place decompression ----
     *
     * The device has 131,072 bytes of RAM and currently spends 81,920 of them
     * on two band buffers -- one for the compressed bytes as they arrive, one
     * for the decompressed pixels. Decompressing *in place* would need only
     * one, freeing enough RAM to roughly double the band size, which halves
     * the number of rectangles per frame and therefore the per-rectangle
     * control-transfer overhead.
     *
     * The trick: put the compressed block at the END of the output buffer and
     * decode forwards into the front of it. Both pointers move forwards, and
     * the write pointer must never overtake the read pointer.
     *
     * Why it holds for this decoder. Per sequence, the input advances by
     * 1 (token) + 2 (offset) + literals + any extended-length bytes, while the
     * output advances by literals + match. Since a match is at least 4 bytes,
     * output outruns input by at least 4 - 3 = 1 byte per sequence, so the gap
     * closes monotonically and is largest at the very end -- where it equals
     * exactly (decompressed size - compressed size). Placing the block at
     * dst + size + margin - csize therefore keeps the write pointer behind the
     * read pointer throughout, for any margin >= 0.
     *
     * That argument is only sound because this decoder copies exactly as many
     * bytes as it was asked to. The reference LZ4 decoder uses "wildcopy",
     * overshooting by up to 32 bytes, which is precisely why upstream defines
     * LZ4_DECOMPRESS_INPLACE_MARGIN as (csize >> 8) + 32. We use the same
     * margin anyway: it costs a few hundred bytes and it means the buffer is
     * still correctly sized if this ever gets swapped for the reference
     * implementation.
     *
     * The literal copy can overlap when the two pointers get close. That is
     * safe here because the destination is always *below* the source, and
     * copy_nonoverlapping() reads each word before writing it, so a forward
     * copy with dst < src is correct even when the ranges touch.
     */
    uint32_t inplace_cases = 0, inplace_tight = 0;

    for (uint32_t iter = 0; iter < 20000u; iter++) {
        uint32_t len = (iter % 4u == 0u)
                     ? (rnd() % 512u) + 1u
                     : (rnd() % MAX_RAW) + 1u;
        int style = (int) (iter % 5u);
        int hc = (int) (iter % 3u == 0u);

        make_raw(len, style);

        int clen = hc
            ? LZ4_compress_HC((const char *) raw, (char *) comp,
                              (int) len, (int) sizeof(comp), 9)
            : LZ4_compress_default((const char *) raw, (char *) comp,
                                   (int) len, (int) sizeof(comp));
        if (clen <= 0)
            continue;

        uint32_t margin = ((uint32_t) clen >> 8) + 32u;
        uint32_t total = len + margin;
        if (total + 2u * GUARD > sizeof(out))
            continue;

        /* Guard the whole allocation, then place the compressed block flush
         * against the end of the buffer, exactly as the device will. */
        memset(out, GUARD_BYTE, sizeof(out));
        uint8_t *dst = out + GUARD;
        uint8_t *src = dst + total - (uint32_t) clen;
        memcpy(src, comp, (size_t) clen);

        int got = lz4_decompress_block(src, (uint32_t) clen, dst, len);

        if (got != (int) len) {
            fprintf(stderr, "in-place iter %u style %d hc %d len %u clen %d: "
                            "got %d\n", iter, style, hc, len, clen, got);
            return 1;
        }
        if (memcmp(dst, raw, len) != 0) {
            fprintf(stderr, "in-place iter %u: content mismatch (len %u, "
                            "clen %d, margin %u)\n", iter, len, clen, margin);
            return 1;
        }
        if (!guards_intact(total)) {
            fprintf(stderr, "in-place iter %u: guards clobbered\n", iter);
            return 1;
        }

        /* How close did the two pointers actually get? If the decoder ever
         * needed the margin, incompressible data is where it would show. */
        if ((uint32_t) clen + 64u >= len)
            inplace_tight++;
        inplace_cases++;
    }

    printf("lz4_dec: %u valid blocks round-tripped byte-exact\n", valid_cases);
    printf("lz4_dec: %u corrupt blocks, %u rejected, 0 buffer overruns\n",
           corrupt_cases, rejected);
    printf("lz4_dec: %u in-place blocks byte-exact (%u of them barely "
           "compressible)\n", inplace_cases, inplace_tight);
    printf("PASS\n");
    return 0;
}
