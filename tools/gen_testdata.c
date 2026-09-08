/*
 * Host tool: build the LZ4 test corpus that M1 measures on the target.
 *
 * Why this exists rather than a hand-written array of bytes:
 *
 * The device must decode exactly what the Linux gud driver will send it. That
 * driver compresses with LZ4_compress_default() into a buffer no bigger than
 * the uncompressed data, one damage rectangle at a time, splitting a rectangle
 * into horizontal bands when it exceeds the device's declared max_buffer_size
 * (drivers/gpu/drm/gud/gud_pipe.c, gud_flush_damage()). So this tool:
 *
 *   1. Reads the 320x240 RGB565 images produced by make_images.py.
 *   2. Splits each into bands of BAND_LINES lines -- the same split the host
 *      will do once we declare max_buffer_size.
 *   3. Compresses each band with the reference liblz4, with the same output
 *      cap the kernel uses (dstCapacity == srcSize).
 *   4. Checks our own lz4_decompress_block() reproduces the original exactly.
 *   5. Also compresses the whole frame in one piece, purely to report what
 *      band-splitting costs us in ratio -- that number decides how large
 *      max_buffer_size should be, so it is worth measuring rather than
 *      guessing.
 *   6. Emits testdata.c / testdata.h for the target build.
 *
 * Build and run:  make -C m1_lz4/host
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>

#include <lz4.h>

#include "../src/lz4_dec.h"

#define W 320
#define H 240
#ifndef BANDL
#define BANDL 120                           /* see max_buffer_size discussion */
#endif
#define BAND_LINES BANDL
#define PITCH (W * 2)                       /* RGB565 -> 2 bytes per pixel    */
#define BAND_BYTES (BAND_LINES * PITCH)     /* 40960                          */
#define FRAME_BYTES (W * H * 2)             /* 153600                         */
#define MAX_BANDS ((H + BAND_LINES - 1) / BAND_LINES)

#define MAX_IMAGES 12

struct band_rec {
    uint32_t off;        /* offset into the emitted blob */
    uint32_t comp_len;   /* 0 == stored uncompressed     */
    uint32_t raw_len;
};

struct image_rec {
    char name[64];
    uint32_t num_bands;
    struct band_rec bands[MAX_BANDS];
    uint32_t comp_total;       /* sum over bands                       */
    uint32_t whole_frame_len;  /* same data compressed in one piece    */
    uint32_t hash;
};

static struct image_rec images[MAX_IMAGES];
static int num_images;

static uint8_t raw[FRAME_BYTES];
static uint8_t blob[MAX_IMAGES * FRAME_BYTES];
static uint32_t blob_len;
static uint8_t comp_tmp[FRAME_BYTES * 2];
static uint8_t check_buf[FRAME_BYTES];

/* FNV-1a, 32-bit. A cheap content hash so the target can prove the bytes it
 * decompressed are the bytes this tool compressed. */
static uint32_t fnv1a(uint32_t h, const uint8_t *p, uint32_t n)
{
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

/*
 * The corpus is ordered deliberately: easiest content first, hardest last, so
 * the target's report reads as a progression. Any extra .raw files found in
 * images/ (a real screenshot, say) are appended after these.
 */
static const char *preferred_order[] = {
    "solid", "flat_ui", "terminal_text", "gradient", "noisy_gradient",
};
#define NUM_PREFERRED ((int) (sizeof(preferred_order) / sizeof(preferred_order[0])))

static int load_raw(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "images/%s.raw", name);

    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    size_t n = fread(raw, 1, sizeof(raw), f);
    long extra = 0;
    if (n == sizeof(raw))
        extra = fgetc(f) == EOF ? 0 : 1;
    fclose(f);

    if (n != sizeof(raw) || extra) {
        fprintf(stderr, "%s: expected exactly %d bytes, got %zu%s\n",
                path, FRAME_BYTES, n, extra ? "+" : "");
        return -1;
    }
    return 1;
}

static int process(const char *name)
{
    int rc = load_raw(name);
    if (rc <= 0)
        return rc;

    struct image_rec *im = &images[num_images];
    snprintf(im->name, sizeof(im->name), "%s", name);
    im->hash = fnv1a(2166136261u, raw, FRAME_BYTES);

    /* Whole-frame reference compression -- reported, never sent. */
    int whole = LZ4_compress_default((const char *) raw, (char *) comp_tmp,
                                     FRAME_BYTES, FRAME_BYTES);
    im->whole_frame_len = whole > 0 ? (uint32_t) whole : FRAME_BYTES;

    uint32_t nb = 0, total = 0;
    char detail[256] = "";

    for (uint32_t off = 0; off < FRAME_BYTES; off += BAND_BYTES) {
        uint32_t raw_len = FRAME_BYTES - off;
        if (raw_len > BAND_BYTES)
            raw_len = BAND_BYTES;

        /*
         * Same call, and crucially the same output cap, that gud_prep_flush()
         * makes: dstCapacity == srcSize. If the data does not compress, LZ4
         * returns 0 and the kernel falls back to sending the band
         * uncompressed. We record that case as comp_len == 0.
         */
        int clen = LZ4_compress_default((const char *) raw + off,
                                        (char *) comp_tmp,
                                        (int) raw_len, (int) raw_len);

        struct band_rec *b = &im->bands[nb];
        b->raw_len = raw_len;
        b->off = blob_len;

        if (clen > 0) {
            b->comp_len = (uint32_t) clen;
            memcpy(blob + blob_len, comp_tmp, (size_t) clen);
            blob_len += (uint32_t) clen;

            /* Verify our decoder against the reference compressor. */
            int got = lz4_decompress_block(blob + b->off, b->comp_len,
                                           check_buf, raw_len);
            if (got != (int) raw_len) {
                fprintf(stderr, "\n%s band %u: decode returned %d, want %u\n",
                        im->name, nb, got, raw_len);
                return -1;
            }
            if (memcmp(check_buf, raw + off, raw_len) != 0) {
                fprintf(stderr, "\n%s band %u: decoded bytes differ\n",
                        im->name, nb);
                return -1;
            }
            total += (uint32_t) clen;
        } else {
            b->comp_len = 0;                 /* incompressible: stored raw */
            memcpy(blob + blob_len, raw + off, raw_len);
            blob_len += raw_len;
            total += raw_len;
        }

        char part[32];
        snprintf(part, sizeof(part), "%s%u", nb ? "/" : "",
                 b->comp_len ? b->comp_len : b->raw_len);
        strncat(detail, part, sizeof(detail) - strlen(detail) - 1);
        nb++;
    }

    im->num_bands = nb;
    im->comp_total = total;

    printf("%-18s %8u %7.1fx %8u %7.1fx  %+5.1f%%  %s\n",
           im->name,
           total, (double) FRAME_BYTES / total,
           im->whole_frame_len, (double) FRAME_BYTES / im->whole_frame_len,
           100.0 * ((double) total / im->whole_frame_len - 1.0),
           detail);

    num_images++;
    return 1;
}

int main(void)
{
    printf("%-18s %8s %8s %8s %8s  %6s  %s\n",
           "image", "banded", "ratio", "1 piece", "ratio", "cost",
           "band sizes");

    for (int i = 0; i < NUM_PREFERRED; i++) {
        int rc = process(preferred_order[i]);
        if (rc < 0)
            return 1;
        if (rc == 0)
            fprintf(stderr, "warning: images/%s.raw missing "
                            "(run make_images.py)\n", preferred_order[i]);
    }

    /* Pick up anything else in images/, e.g. a real screenshot. */
    DIR *d = opendir("images");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && num_images < MAX_IMAGES) {
            char base[64];
            size_t len = strlen(e->d_name);
            if (len < 5 || len - 4 >= sizeof(base) ||
                strcmp(e->d_name + len - 4, ".raw") != 0)
                continue;
            memcpy(base, e->d_name, len - 4);
            base[len - 4] = '\0';

            int known = 0;
            for (int i = 0; i < NUM_PREFERRED; i++)
                if (strcmp(base, preferred_order[i]) == 0)
                    known = 1;
            if (known)
                continue;

            if (process(base) < 0)
                return 1;
        }
        closedir(d);
    }

    if (num_images == 0) {
        fprintf(stderr, "no images found; run: python3 make_images.py\n");
        return 1;
    }

    FILE *fc = fopen("../src/testdata.c", "w");
    FILE *fh = fopen("../src/testdata.h", "w");
    if (!fc || !fh) {
        perror("open output");
        return 1;
    }

    fprintf(fh,
        "/* Generated by m1_lz4/host/gen_testdata.c -- do not edit by hand. */\n"
        "#ifndef TESTDATA_H\n"
        "#define TESTDATA_H\n\n"
        "#include <stdint.h>\n\n"
        "#define LZ4_TEST_WIDTH       %u\n"
        "#define LZ4_TEST_HEIGHT      %u\n"
        "#define LZ4_TEST_BAND_LINES  %u\n"
        "#define LZ4_TEST_BAND_BYTES  %uu\n"
        "#define LZ4_TEST_FRAME_BYTES %uu\n"
        "#define LZ4_TEST_MAX_BANDS   %u\n\n"
        "/* comp_len == 0 means the band is stored uncompressed, which is what\n"
        " * the host does when LZ4 cannot shrink it. */\n"
        "struct lz4_test_band {\n"
        "    uint32_t off;\n"
        "    uint32_t comp_len;\n"
        "    uint32_t raw_len;\n"
        "};\n\n"
        "struct lz4_test_image {\n"
        "    const char *name;\n"
        "    uint32_t num_bands;\n"
        "    uint32_t comp_total;\n"
        "    uint32_t hash;      /* FNV-1a of the whole uncompressed frame */\n"
        "    const struct lz4_test_band *bands;\n"
        "};\n\n"
        "extern const uint8_t lz4_test_blob[];\n"
        "extern const struct lz4_test_image lz4_test_images[];\n"
        "extern const uint32_t lz4_test_num_images;\n\n"
        "#endif /* TESTDATA_H */\n",
        W, H, BAND_LINES, BAND_BYTES, FRAME_BYTES, MAX_BANDS);

    fprintf(fc,
        "/* Generated by m1_lz4/host/gen_testdata.c -- do not edit by hand.\n"
        " *\n"
        " * %u bytes of LZ4 blocks produced by liblz4's LZ4_compress_default(),\n"
        " * the same function the Linux gud driver uses. Lives in flash.\n"
        " */\n\n"
        "#include \"testdata.h\"\n\n"
        "const uint8_t lz4_test_blob[] = {\n", blob_len);

    for (uint32_t i = 0; i < blob_len; i++) {
        fprintf(fc, "%s0x%02x,", (i % 16 == 0) ? "    " : " ", blob[i]);
        if (i % 16 == 15 || i == blob_len - 1)
            fprintf(fc, "\n");
    }
    fprintf(fc, "};\n\n");

    for (int i = 0; i < num_images; i++) {
        fprintf(fc, "static const struct lz4_test_band bands_%s[] = {\n",
                images[i].name);
        for (uint32_t b = 0; b < images[i].num_bands; b++)
            fprintf(fc, "    { %8u, %6u, %6u },\n",
                    images[i].bands[b].off,
                    images[i].bands[b].comp_len,
                    images[i].bands[b].raw_len);
        fprintf(fc, "};\n\n");
    }

    fprintf(fc, "const struct lz4_test_image lz4_test_images[] = {\n");
    for (int i = 0; i < num_images; i++)
        fprintf(fc, "    { \"%s\", %u, %u, 0x%08xu, bands_%s },\n",
                images[i].name, images[i].num_bands, images[i].comp_total,
                images[i].hash, images[i].name);
    fprintf(fc, "};\n\n");

    fprintf(fc, "const uint32_t lz4_test_num_images = %u;\n", num_images);

    fclose(fc);
    fclose(fh);

    printf("\nblob = %u bytes of flash across %d images; "
           "wrote ../src/testdata.c and testdata.h\n", blob_len, num_images);
    return 0;
}
