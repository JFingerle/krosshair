/*
 * Unit tests for APNG decoding (src/apng.c, load_apng).
 *
 * Test inputs are synthesized in memory: a minimal PNG writer (stored
 * deflate blocks, no zlib dependency) plus APNG acTL/fcTL/fdAT chunks, so
 * the decoder is exercised without any fixture files.
 */
#define _GNU_SOURCE

#include <stdint.h>

#include "test.h"
#include "../include/krosshair.h"

/* ── in-memory PNG/APNG builders ─────────────────────────────── */

static const unsigned char png_signature[8] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
};

static uint32_t crc_table[256];
static int crc_table_ready = 0;

static void crc_init(void)
{
        if (crc_table_ready)
                return;
        for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int j = 0; j < 8; j++)
                        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                crc_table[i] = c;
        }
        crc_table_ready = 1;
}

static uint32_t crc32(const unsigned char* data, size_t len)
{
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; i++)
                crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFF;
}

static void be16(unsigned char* p, uint16_t v)
{
        p[0] = (v >> 8) & 0xff;
        p[1] = v & 0xff;
}

static void be32(unsigned char* p, uint32_t v)
{
        p[0] = (v >> 24) & 0xff;
        p[1] = (v >> 16) & 0xff;
        p[2] = (v >> 8) & 0xff;
        p[3] = v & 0xff;
}

/*
 * Serialize one PNG chunk (length + type + data + CRC) into out.
 * Returns the number of bytes written (12 + data_len).
 */
static size_t put_chunk(unsigned char* out, const char* type,
                        const unsigned char* data, uint32_t data_len)
{
        be32(out, data_len);
        memcpy(out + 4, type, 4);
        if (data && data_len)
                memcpy(out + 8, data, data_len);
        be32(out + 8 + data_len, crc32(out + 4, 4 + data_len));
        return 12 + data_len;
}

/*
 * Wrap raw bytes in a one-block stored zlib stream (len < 65535).
 * Header 0x78 0x9C: CM=8, CINFO=7, FLEVEL=3, FCHECK valid (0x789C % 31 == 0).
 * Returns total stream length.
 */
static size_t zlib_store(const unsigned char* data, size_t len, unsigned char* out)
{
        out[0] = 0x78;
        out[1] = 0x9C;
        out[2] = 0x01; /* BFINAL=1, BTYPE=00 (stored) */
        out[3] = (unsigned char)(len & 0xff);
        out[4] = (unsigned char)((len >> 8) & 0xff);
        out[5] = (unsigned char)(~len & 0xff);
        out[6] = (unsigned char)((~len >> 8) & 0xff);
        memcpy(out + 7, data, len);
        return 7 + len;
}

/*
 * Build a complete 8-bit RGBA PNG (no acTL) in out; out must hold
 * 8 + 25 + 12 + 7 + w*(4w+1) + 12 bytes. Returns the file length.
 * rgba: w*h*4 pixels, top-left origin.
 */
static size_t build_rgba_png(unsigned char* out, uint32_t w, uint32_t h,
                              const unsigned char* rgba)
{
        size_t raw_len = (size_t)w * h * (4 * w + 1);
        unsigned char* raw = malloc(raw_len);
        for (uint32_t y = 0; y < h; y++) {
                raw[y * (4 * w + 1)] = 0; /* filter: none */
                memcpy(raw + y * (4 * w + 1) + 1, rgba + (size_t)y * w * 4, w * 4);
        }

        size_t off = 0;
        memcpy(out + off, png_signature, 8);
        off += 8;

        unsigned char ihdr[13];
        be32(ihdr + 0, w);
        be32(ihdr + 4, h);
        ihdr[8] = 8;  /* bit depth */
        ihdr[9] = 6;  /* color type: RGBA */
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        off += put_chunk(out + off, "IHDR", ihdr, 13);

        unsigned char idat[7 + raw_len];
        size_t idat_len = zlib_store(raw, raw_len, idat);
        free(raw);
        off += put_chunk(out + off, "IDAT", idat, (uint32_t)idat_len);

        off += put_chunk(out + off, "IEND", NULL, 0);
        return off;
}

/*
 * Build a complete APNG in out: IHDR + acTL + (fcTL + fdAT) per frame +
 * IEND. Each frame is w*h*4 RGBA pixels (same size as the canvas). The
 * image data must live in fdAT chunks (a plain IDAT is only valid for a
 * first frame that has no fcTL).
 * Returns the file length.
 */
static size_t build_apng(unsigned char* out, uint32_t w, uint32_t h,
                          uint32_t n_frames,
                          const unsigned char* const* frame_px,
                          const uint16_t delay_nums[], const uint16_t delay_dens[])
{
        size_t off = 0;
        memcpy(out + off, png_signature, 8);
        off += 8;

        unsigned char ihdr[13];
        be32(ihdr + 0, w);
        be32(ihdr + 4, h);
        ihdr[8] = 8;
        ihdr[9] = 6;
        ihdr[10] = ihdr[11] = ihdr[12] = 0;
        off += put_chunk(out + off, "IHDR", ihdr, 13);

        unsigned char actl[8];
        be32(actl + 0, n_frames);
        be32(actl + 4, 0); /* play forever */
        off += put_chunk(out + off, "acTL", actl, 8);

        for (uint32_t i = 0; i < n_frames; i++) {
                unsigned char fctl[26];
                be32(fctl + 0, i);           /* sequence number */
                be32(fctl + 4, w);
                be32(fctl + 8, h);
                be32(fctl + 12, 0);          /* x offset */
                be32(fctl + 16, 0);          /* y offset */
                be16(fctl + 20, delay_nums[i]);
                be16(fctl + 22, delay_dens[i]);
                fctl[24] = 0;                /* dispose: none */
                fctl[25] = 0;                /* blend: source */
                off += put_chunk(out + off, "fcTL", fctl, 26);

                size_t raw_len = (size_t)w * h * (4 * w + 1);
                unsigned char* raw = malloc(raw_len);
                for (uint32_t y = 0; y < h; y++) {
                        raw[y * (4 * w + 1)] = 0;
                        memcpy(raw + y * (4 * w + 1) + 1,
                               frame_px[i] + (size_t)y * w * 4, w * 4);
                }
                /* fdAT: 4-byte sequence number, then IDAT-equivalent data */
                unsigned char fdat[4 + 7 + raw_len];
                be32(fdat + 0, i);
                size_t idat_len = zlib_store(raw, raw_len, fdat + 4);
                free(raw);
                off += put_chunk(out + off, "fdAT", fdat, (uint32_t)(4 + idat_len));
        }

        off += put_chunk(out + off, "IEND", NULL, 0);
        return off;
}

/* ── tests ───────────────────────────────────────────────────── */

static void test_plain_png_rejected(void)
{
        /* load_apng only accepts animated PNGs (needs an acTL chunk) */
        static const unsigned char px[4] = { 255, 0, 0, 255 };
        unsigned char png[256];
        size_t len = build_rgba_png(png, 1, 1, px);

        int w = 0, h = 0, frames = 0;
        int* delays = NULL;
        CHECK(load_apng(png, len, &w, &h, &frames, &delays) == NULL);
}

static void test_garbage_rejected(void)
{
        unsigned char garbage[32];
        memset(garbage, 0xAB, sizeof(garbage));

        int w = 0, h = 0, frames = 0;
        int* delays = NULL;
        CHECK(load_apng(garbage, sizeof(garbage), &w, &h, &frames, &delays) == NULL);
}

static void test_truncated_rejected(void)
{
        unsigned char sig[8];
        memcpy(sig, png_signature, 8);

        int w = 0, h = 0, frames = 0;
        int* delays = NULL;
        CHECK(load_apng(sig, 8, &w, &h, &frames, &delays) == NULL);
}

/*
 * Two full-canvas 2x2 frames: red, then green. Blend source + dispose none
 * means frame 2 fully replaces the canvas. Expected atlas rows: red, green.
 * Delays: 1/10 s = 100 ms and 2/10 s = 200 ms.
 */
static void test_two_frames_composite(void)
{
        static const unsigned char red[8]   = { 255, 0, 0, 255,  255, 0, 0, 255 };
        static const unsigned char green[8] = { 0, 255, 0, 255,  0, 255, 0, 255 };
        const unsigned char* frames[2] = { red, green };

        uint16_t nums[2] = { 1, 2 };
        uint16_t dens[2] = { 10, 10 };
        unsigned char apng[512];
        size_t len = build_apng(apng, 2, 2, 2, frames, nums, dens);

        int w = 0, h = 0, frames_n = 0;
        int* delays = NULL;
        unsigned char* atlas = load_apng(apng, len, &w, &h, &frames_n, &delays);

        CHECK(atlas != NULL);
        if (atlas) {
                CHECK_EQ(w, 2);
                CHECK_EQ(h, 2);
                CHECK_EQ(frames_n, 2);
                CHECK(delays != NULL);
                if (delays) {
                        CHECK_EQ(delays[0], 100);
                        CHECK_EQ(delays[1], 200);
                }

                /* atlas row 0 = first composited frame (all red) */
                for (int i = 0; i < 8; i += 4) {
                        CHECK_EQ((int)atlas[i], 255);
                        CHECK_EQ((int)atlas[i + 1], 0);
                        CHECK_EQ((int)atlas[i + 2], 0);
                        CHECK_EQ((int)atlas[i + 3], 255);
                }
                /* atlas row 1 = second frame (all green) at offset w*h*4 */
                size_t row1 = (size_t)2 * 2 * 4;
                for (int i = 0; i < 8; i += 4) {
                        CHECK_EQ((int)atlas[row1 + i], 0);
                        CHECK_EQ((int)atlas[row1 + i + 1], 255);
                        CHECK_EQ((int)atlas[row1 + i + 2], 0);
                        CHECK_EQ((int)atlas[row1 + i + 3], 255);
                }
        }
        free(atlas);
        free(delays);
}

/* A zero delay denominator means centiseconds: 5/0 = 5 cs = 50 ms. */
static void test_delay_den_zero_means_centiseconds(void)
{
        static const unsigned char blue[4] = { 0, 0, 255, 255 };
        const unsigned char* frames[1] = { blue };
        uint16_t nums[1] = { 5 };
        uint16_t dens[1] = { 0 };

        unsigned char apng[512];
        size_t len = build_apng(apng, 1, 1, 1, frames, nums, dens);

        int w = 0, h = 0, frames_n = 0;
        int* delays = NULL;
        unsigned char* atlas = load_apng(apng, len, &w, &h, &frames_n, &delays);

        CHECK(atlas != NULL);
        if (atlas) {
                CHECK_EQ(w, 1);
                CHECK_EQ(h, 1);
                CHECK_EQ(frames_n, 1);
                CHECK(delays != NULL);
                if (delays)
                        CHECK_EQ(delays[0], 50);
        }
        free(atlas);
        free(delays);
}

int main(void)
{
        crc_init();
        fprintf(stderr, "# test_apng\n");
        test_plain_png_rejected();
        test_garbage_rejected();
        test_truncated_rejected();
        test_two_frames_composite();
        test_delay_den_zero_means_centiseconds();
        fprintf(stderr, "  -> %d passed, %d failed\n", test_passed, test_failed);
        return test_failed ? 1 : 0;
}
