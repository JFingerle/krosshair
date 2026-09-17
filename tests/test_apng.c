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
#include "apng_fixture.h"

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
