/*
 * tests/apng_fixture.h — in-memory PNG/APNG builders shared by the
 * APNG unit suite (tests/test_apng.c) and the end-to-end mock-ICD test
 * (tests/test_mock_icd.c).
 *
 * A minimal PNG writer (stored deflate blocks, no zlib dependency) plus
 * APNG acTL/fcTL/fdAT chunks, so fixtures can be synthesized without
 * any file on disk.
 */
#ifndef APNG_FIXTURE_H
#define APNG_FIXTURE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char png_signature[8] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
};

static uint32_t crc_table[256];
static int crc_table_ready = 0;

static inline void crc_init(void)
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

static inline uint32_t crc32(const unsigned char* data, size_t len)
{
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; i++)
                crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFF;
}

static inline void be16(unsigned char* p, uint16_t v)
{
        p[0] = (v >> 8) & 0xff;
        p[1] = v & 0xff;
}

static inline void be32(unsigned char* p, uint32_t v)
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
static inline size_t put_chunk(unsigned char* out, const char* type,
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
static inline size_t zlib_store(const unsigned char* data, size_t len,
                                unsigned char* out)
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
static inline size_t build_rgba_png(unsigned char* out, uint32_t w,
                                    uint32_t h, const unsigned char* rgba)
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
static inline size_t build_apng(unsigned char* out, uint32_t w, uint32_t h,
                                uint32_t n_frames,
                                const unsigned char* const* frame_px,
                                const uint16_t delay_nums[],
                                const uint16_t delay_dens[])
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

#endif /* APNG_FIXTURE_H */
