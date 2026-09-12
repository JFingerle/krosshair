/*
 * Animated PNG (APNG) decoding.
 *
 * Parses the APNG frame metadata (acTL/fcTL chunks), decodes and
 * composites each frame with stb_image, and returns them as a vertical
 * atlas with per-frame delays — the same layout the stb GIF loader
 * produces — so the crosshair module can treat animated crosshairs like
 * GIFs.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/stb_image.h"
#include "../include/krosshair.h"

static const unsigned char png_signature[8] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
};

enum {
        APNG_DISPOSE_NONE = 0,        /* keep canvas as-is            */
        APNG_DISPOSE_BACKGROUND = 1,  /* clear frame rect to 0,0,0,0  */
        APNG_DISPOSE_PREVIOUS = 2     /* restore pre-frame canvas     */
};

enum {
        APNG_BLEND_SOURCE = 0, /* frame fully replaces canvas pixels */
        APNG_BLEND_OVER = 1     /* Porter-Duff "over" alpha blend     */
};

/*
 * Read a big-endian uint32 from p[0..3].
 *
 * p: pointer to 4 consecutive bytes (e.g. a PNG chunk length or offset).
 * Returns the value in host byte order.
 */
static uint32_t apng_read_be32(const unsigned char* p)
{
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * Read a big-endian uint16 from p[0..1].
 *
 * p: pointer to 2 consecutive bytes (e.g. an fcTL delay numerator).
 * Returns the value in host byte order.
 */
static uint16_t apng_read_be16(const unsigned char* p)
{
        return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

/*
 * Write value v as a big-endian uint32 into p[0..3].
 *
 * p: pointer to 4 writable bytes; v: the value to encode.
 */
static void apng_write_be32(unsigned char* p, uint32_t v)
{
        p[0] = (v >> 24) & 0xff;
        p[1] = (v >> 16) & 0xff;
        p[2] = (v >> 8) & 0xff;
        p[3] = v & 0xff;
}

/* CRC32 used by PNG chunk checksums */
static uint32_t apng_crc32_table[256];
static int apng_crc32_table_ready = 0;

/*
 * Lazily build the 256-entry CRC32 lookup table (reflected polynomial
 * 0xEDB88320). No parameters; safe to call repeatedly — the table is
 * only generated once.
 */
static void apng_crc32_init(void)
{
        if (apng_crc32_table_ready) return;
        for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int j = 0; j < 8; j++)
                        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                apng_crc32_table[i] = c;
        }
        apng_crc32_table_ready = 1;
}

/*
 * Compute the CRC32 checksum over a byte range (PNG's algorithm).
 *
 * data: pointer to the bytes to checksum (typically chunk type + data);
 * len:  number of bytes.
 * Returns the 32-bit CRC.
 */
static uint32_t apng_crc32(const unsigned char* data, size_t len)
{
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; i++)
                crc = apng_crc32_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFF;
}

/*
 * Serialize one complete PNG chunk (length + 4-char type + data + CRC32)
 * into out, which must have room for 12 + data_len bytes.
 *
 * out:     destination buffer;
 * type:    4-character chunk type ("IHDR", "IDAT", "IEND", ...);
 * data:    chunk payload, or NULL when data_len is 0 (e.g. IEND);
 * data_len: payload length in bytes.
 * Returns the number of bytes written (12 + data_len).
 */
static size_t apng_write_chunk(unsigned char* out, const char* type,
                                const unsigned char* data, uint32_t data_len)
{
        apng_write_be32(out, data_len);
        memcpy(out + 4, type, 4);
        if (data && data_len)
                memcpy(out + 8, data, data_len);
        /* CRC covers type + data */
        uint32_t crc = apng_crc32(out + 4, 4 + data_len);
        apng_write_be32(out + 8 + data_len, crc);
        return 12 + data_len; /* length(4) + type(4) + data + crc(4) */
}

/*
 * Step to the next PNG chunk after *pos and advance *pos past it.
 *
 * data/len: file contents; *pos: offset of the chunk currently under
 *           inspection (must point at a chunk length field);
 * ctype/clen/cdata: set to the chunk's 4-char type, payload length, and
 *           payload pointer on success.
 * Returns 1 on success, 0 when no complete chunk remains in the file.
 */
static int apng_next_chunk(const unsigned char* data, size_t len,
                           size_t* pos, const unsigned char** ctype,
                           uint32_t* clen, const unsigned char** cdata)
{
        if (*pos + 12 > len) return 0;
        *clen = apng_read_be32(data + *pos);
        *ctype = data + *pos + 4;
        *cdata = data + *pos + 8;
        if (*pos + 12 + *clen > len) return 0;
        *pos += 12 + *clen;
        return 1;
}

typedef struct apng_frame_info {
        uint32_t width, height;
        uint32_t x_offset, y_offset;
        uint16_t delay_num, delay_den;
        uint8_t dispose_op, blend_op;
        /* raw compressed data (IDAT content or fdAT content minus seq) */
        unsigned char* idat_data;
        size_t idat_size;
        size_t idat_capacity;
} apng_frame_info_t;

/*
 * Append more compressed image data to a frame's IDAT buffer, growing
 * the buffer (doubled realloc) as needed.
 *
 * f:    frame whose IDAT data is being accumulated;
 * data: new bytes to append (an IDAT payload, or fdAT payload minus its
 *       4-byte sequence number);
 * len:  number of bytes to append.
 */
static void apng_frame_append_idat(apng_frame_info_t* f,
                                    const unsigned char* data, size_t len)
{
        if (f->idat_size + len > f->idat_capacity) {
                size_t new_cap = (f->idat_capacity == 0) ? 4096 : f->idat_capacity;
                while (new_cap < f->idat_size + len)
                        new_cap *= 2;
                f->idat_data = realloc(f->idat_data, new_cap);
                f->idat_capacity = new_cap;
        }
        memcpy(f->idat_data + f->idat_size, data, len);
        f->idat_size += len;
}

/*
 * Build a minimal valid PNG in memory for a single APNG frame,
 * then decode it with stbi.  Returns RGBA pixels (caller must
 * stbi_image_free), or NULL on failure.
 *
 * f:        frame to decode (dimensions + accumulated IDAT data);
 * ihdr_raw: the 13 bytes of the original IHDR *data* (no
 *           length/type/crc) — bit depth, color type, compression,
 *           filter and interlace come from it; width/height are
 *           overwritten with the frame's own size;
 * out_w/out_h: set to the decoded pixel dimensions on success.
 */
static stbi_uc* apng_decode_frame(const apng_frame_info_t* f,
                                    const unsigned char* ihdr_raw,
                                    int* out_w, int* out_h)
{
        /* build a new IHDR with the frame's dimensions */
        unsigned char ihdr[13];
        memcpy(ihdr, ihdr_raw, 13);
        apng_write_be32(ihdr + 0, f->width);
        apng_write_be32(ihdr + 4, f->height);

        /* total PNG size: sig(8) + IHDR chunk(25) + IDAT chunk(12+data) + IEND(12) */
        size_t png_size = 8 + 25 + (12 + f->idat_size) + 12;
        unsigned char* png = malloc(png_size);
        if (!png) return NULL;

        size_t off = 0;
        memcpy(png, png_signature, 8);
        off += 8;
        off += apng_write_chunk(png + off, "IHDR", ihdr, 13);
        off += apng_write_chunk(png + off, "IDAT", f->idat_data, (uint32_t)f->idat_size);
        off += apng_write_chunk(png + off, "IEND", NULL, 0);

        int w, h, c;
        stbi_uc* pixels = stbi_load_from_memory(png, (int)off, &w, &h, &c, 4);
        free(png);

        if (pixels) {
                *out_w = w;
                *out_h = h;
        }
        return pixels;
}

/*
 * Alpha-composite the src frame RGBA pixels over the dst canvas using
 * the standard Porter-Duff "over" operator (straight alpha, no
 * premultiplication). Pixels outside the canvas are skipped.
 *
 * dst:     destination canvas (rgba, canvas_w x canvas_h) — updated in place;
 * src:     source frame pixels (rgba, fw x fh);
 * canvas_w/canvas_h: canvas dimensions;
 * x_off/y_off: top-left position of the frame on the canvas;
 * fw/fh:   frame dimensions.
 */
static void apng_blend_over(unsigned char* dst, const unsigned char* src,
                             uint32_t canvas_w, uint32_t canvas_h,
                             uint32_t x_off, uint32_t y_off,
                             uint32_t fw, uint32_t fh)
{
        for (uint32_t y = 0; y < fh; y++) {
                if (y + y_off >= canvas_h) break;
                for (uint32_t x = 0; x < fw; x++) {
                        if (x + x_off >= canvas_w) break;
                        size_t di = ((y + y_off) * canvas_w + (x + x_off)) * 4;
                        size_t si = (y * fw + x) * 4;
                        uint8_t sa = src[si + 3];
                        if (sa == 255) {
                                memcpy(dst + di, src + si, 4);
                        } else if (sa > 0) {
                                uint8_t da = dst[di + 3];
                                /* standard "over" blend */
                                for (int k = 0; k < 3; k++) {
                                        int sv = src[si + k] * sa;
                                        int dv = dst[di + k] * da * (255 - sa) / 255;
                                        int oa = sa + da * (255 - sa) / 255;
                                        dst[di + k] = oa ? (uint8_t)((sv + dv) / oa) : 0;
                                }
                                dst[di + 3] = (uint8_t)(sa + da * (255 - sa) / 255);
                        }
                }
        }
}

/*
 * Copy src frame RGBA pixels into the dst canvas at (x_off, y_off) with
 * no blending (PNG "source" blend operator) — src pixels fully replace
 * the canvas pixels they cover. Pixels outside the canvas are skipped.
 *
 * dst:     destination canvas (rgba, canvas_w x canvas_h) — updated in place;
 * src:     source frame pixels (rgba, fw x fh);
 * canvas_w/canvas_h: canvas dimensions;
 * x_off/y_off: top-left position of the frame on the canvas;
 * fw/fh:   frame dimensions.
 */
static void apng_blend_source(unsigned char* dst, const unsigned char* src,
                               uint32_t canvas_w, uint32_t canvas_h,
                               uint32_t x_off, uint32_t y_off,
                               uint32_t fw, uint32_t fh)
{
        for (uint32_t y = 0; y < fh; y++) {
                if (y + y_off >= canvas_h) break;
                size_t di = ((y + y_off) * canvas_w + x_off) * 4;
                size_t si = (y * fw) * 4;
                uint32_t copy_w = fw;
                if (x_off + fw > canvas_w) copy_w = canvas_w - x_off;
                memcpy(dst + di, src + si, copy_w * 4);
        }
}

/*
 * Clear a rectangular region of the canvas to transparent black
 * (rgba 0,0,0,0) — used to apply the "dispose to background" operator.
 * The rectangle is clamped to the canvas bounds.
 *
 * canvas:       destination canvas (rgba, canvas_w x canvas_h) — updated in place;
 * canvas_w/canvas_h: canvas dimensions;
 * x/y:          top-left corner of the region;
 * w/h:          region size.
 */
static void apng_clear_region(unsigned char* canvas, uint32_t canvas_w,
                               uint32_t canvas_h, uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h)
{
        for (uint32_t row = y; row < y + h && row < canvas_h; row++) {
                size_t off = (row * canvas_w + x) * 4;
                uint32_t cw = w;
                if (x + w > canvas_w) cw = canvas_w - x;
                memset(canvas + off, 0, cw * 4);
        }
}

/*
 * Convert a frame's delay, stored as a num/den fraction of a second,
 * into integer milliseconds. A zero denominator means 100 (centiseconds)
 * per the APNG spec; non-positive results clamp to 100 ms so animation
 * never stalls.
 */
static int apng_delay_ms(uint16_t delay_num, uint16_t delay_den)
{
        if (delay_den == 0)
                delay_den = 100;
        int delay_ms = (int)((uint32_t)delay_num * 1000 / delay_den);
        return delay_ms > 0 ? delay_ms : 100;
}

/*
 * Scan the chunk stream for the acTL chunk and return its frame count.
 * Returns 0 if the file has no acTL chunk (i.e. it is not an APNG).
 */
static uint32_t apng_count_frames(const unsigned char* file_data, size_t file_len)
{
        uint32_t num_frames = 0;
        size_t pos = 8; /* skip the 8-byte PNG signature */
        const unsigned char* ctype;
        const unsigned char* cdata;
        uint32_t clen;
        while (apng_next_chunk(file_data, file_len, &pos, &ctype, &clen, &cdata)) {
                if (memcmp(ctype, "acTL", 4) == 0 && clen >= 8)
                        num_frames = apng_read_be32(cdata);
                if (memcmp(ctype, "IEND", 4) == 0) break;
        }
        return num_frames;
}

/*
 * Walk the chunk stream, recording each frame's fcTL metadata and its
 * compressed image data (plain IDAT for a first frame without fcTL,
 * fdAT for all others).
 *
 * num_frames: frame count from acTL — frames[] must hold that many slots;
 * frames:     output array, filled in order of appearance.
 * Returns the number of frames actually collected (may be fewer than
 * num_frames for a truncated file, or < 1 if the file has no frames).
 */
static int apng_collect_frames(const unsigned char* file_data, size_t file_len,
                               uint32_t num_frames, apng_frame_info_t* frames)
{
        int frame_idx = -1; /* index into frames[], -1 until first fcTL */
        int seen_fcTL = 0;
        size_t pos = 8;
        const unsigned char* ctype;
        const unsigned char* cdata;
        uint32_t clen;
        while (apng_next_chunk(file_data, file_len, &pos, &ctype, &clen, &cdata)) {
                if (memcmp(ctype, "fcTL", 4) == 0 && clen >= 26) {
                        if (++frame_idx >= (int)num_frames) break;
                        seen_fcTL = 1;

                        apng_frame_info_t* f = &frames[frame_idx];
                        f->width      = apng_read_be32(cdata + 4);
                        f->height     = apng_read_be32(cdata + 8);
                        f->x_offset   = apng_read_be32(cdata + 12);
                        f->y_offset   = apng_read_be32(cdata + 16);
                        f->delay_num  = apng_read_be16(cdata + 20);
                        f->delay_den  = apng_read_be16(cdata + 22);
                        f->dispose_op = cdata[24];
                        f->blend_op   = cdata[25];
                } else if (memcmp(ctype, "IDAT", 4) == 0) {
                        /* An IDAT before any fcTL belongs to a first frame
                         * that has no fcTL (it uses the IHDR dimensions) */
                        if (!seen_fcTL)
                                apng_frame_append_idat(&frames[0], cdata, clen);
                } else if (memcmp(ctype, "fdAT", 4) == 0 && clen > 4) {
                        /* fdAT: first 4 bytes are the sequence number,
                         * the rest is IDAT-equivalent data */
                        if (frame_idx >= 0)
                                apng_frame_append_idat(&frames[frame_idx],
                                                       cdata + 4, clen - 4);
                } else if (memcmp(ctype, "IEND", 4) == 0) {
                        break;
                }
        }
        return frame_idx + 1;
}

/*
 * Decode and composite every frame in order, producing one fully-composited
 * canvas per frame in the atlas (vertical strip, one canvas per row), and
 * fill in the per-frame delays.
 *
 * The running canvas is composited frame over frame: each frame is first
 * blended in (blend_op), the result is copied into its atlas row, and then
 * the frame's dispose_op modifies the canvas for the next frame.
 * Frames without image data are skipped — their atlas row keeps the
 * canvas as it was.
 *
 * frames:      decoded frame metadata (num_frames entries);
 * num_frames:  frame count;
 * ihdr_data:   the 13 IHDR data bytes (bit depth / color type / filter);
 * canvas_w/canvas_h: per-frame canvas dimensions;
 * delays:      output, num_frames entries (milliseconds);
 * atlas:       output, num_frames * canvas_w * canvas_h * 4 bytes
 *              (caller-allocated, zero-filled).
 */
static void apng_compose_frames(const apng_frame_info_t* frames, int num_frames,
                                const unsigned char* ihdr_data,
                                uint32_t canvas_w, uint32_t canvas_h,
                                int* delays, unsigned char* atlas)
{
        size_t frame_stride = (size_t)canvas_w * canvas_h * 4;
        unsigned char* canvas = calloc(1, frame_stride);
        unsigned char* dispose_prev = NULL; /* pre-frame canvas for DISPOSE_PREVIOUS */
        if (!canvas) return; /* atlas stays zero-filled */

        for (int i = 0; i < num_frames; i++) {
                const apng_frame_info_t* f = &frames[i];

                delays[i] = apng_delay_ms(f->delay_num, f->delay_den);

                /* snapshot the canvas before compositing so DISPOSE_PREVIOUS
                 * can restore it after this frame is done */
                if (f->dispose_op == APNG_DISPOSE_PREVIOUS) {
                        if (!dispose_prev)
                                dispose_prev = malloc(frame_stride);
                        if (dispose_prev)
                                memcpy(dispose_prev, canvas, frame_stride);
                }

                /* frame has no image data: its atlas row keeps the canvas as-is */
                if (f->idat_size == 0) {
                        KROSSHAIR_LOG("[APNG] frame %d has no image data, skipping\n", i);
                        memcpy(atlas + i * frame_stride, canvas, frame_stride);
                        continue;
                }

                int fw, fh;
                stbi_uc* frame_pixels = apng_decode_frame(f, ihdr_data, &fw, &fh);
                if (!frame_pixels) {
                        KROSSHAIR_LOG("[APNG] failed to decode frame %d\n", i);
                        memcpy(atlas + i * frame_stride, canvas, frame_stride);
                        continue;
                }

                if (f->blend_op == APNG_BLEND_SOURCE)
                        apng_blend_source(canvas, frame_pixels, canvas_w, canvas_h,
                                          f->x_offset, f->y_offset,
                                          f->width, f->height);
                else /* APNG_BLEND_OVER */
                        apng_blend_over(canvas, frame_pixels, canvas_w, canvas_h,
                                        f->x_offset, f->y_offset,
                                        f->width, f->height);
                stbi_image_free(frame_pixels);

                /* copy the composited canvas into this frame's atlas row */
                memcpy(atlas + i * frame_stride, canvas, frame_stride);

                /* apply dispose_op — it only affects the canvas for the NEXT frame */
                switch (f->dispose_op) {
                case APNG_DISPOSE_BACKGROUND:
                        apng_clear_region(canvas, canvas_w, canvas_h,
                                          f->x_offset, f->y_offset,
                                          f->width, f->height);
                        break;
                case APNG_DISPOSE_PREVIOUS:
                        if (dispose_prev)
                                memcpy(canvas, dispose_prev, frame_stride);
                        break;
                default: /* APNG_DISPOSE_NONE: leave the canvas as-is */
                        break;
                }
        }

        free(canvas);
        free(dispose_prev);
}

/*
 * Free a frames array allocated by calloc and each frame's IDAT buffer.
 */
static void apng_free_frame_infos(apng_frame_info_t* frames, uint32_t count)
{
        for (uint32_t i = 0; i < count; i++)
                free(frames[i].idat_data);
        free(frames);
}

/*
 * Load an APNG file and return a vertical atlas of fully-composited
 * frames, identical in layout to what stbi_load_gif_from_memory
 * produces.
 *
 * file_data/file_len: raw file contents in memory;
 * out_width/out_height: set to the per-frame (canvas) pixel dimensions;
 * out_frames:          set to the decoded frame count;
 * out_delays:          set to a malloc'd array of per-frame delays in ms
 *                      (free with free()).
 * Returns the RGBA atlas (free with free()) or NULL if the file is not
 * an animated PNG.
 */
unsigned char* load_apng(const unsigned char* file_data, size_t file_len,
                         int* out_width, int* out_height,
                         int* out_frames, int** out_delays)
{
        apng_crc32_init();

        /* signature + at least one IHDR chunk: 8 (sig) + 12 (hdr) + 13 (IHDR data) */
        if (file_len < 8 + 25 || memcmp(file_data, png_signature, 8) != 0) {
                KROSSHAIR_LOG("[APNG] not a PNG file\n");
                return NULL;
        }

        /* IHDR: the canvas the frames are composited into */
        if (apng_read_be32(file_data + 8) != 13 ||
            memcmp(file_data + 12, "IHDR", 4) != 0) {
                KROSSHAIR_LOG("[APNG] missing IHDR\n");
                return NULL;
        }
        const unsigned char* ihdr_data = file_data + 16;
        uint32_t canvas_w = apng_read_be32(ihdr_data);
        uint32_t canvas_h = apng_read_be32(ihdr_data + 4);

        uint32_t num_frames = apng_count_frames(file_data, file_len);
        if (num_frames == 0) {
                KROSSHAIR_LOG("[APNG] no acTL chunk or 0 frames (not an APNG)\n");
                return NULL;
        }

        apng_frame_info_t* frames = calloc(num_frames, sizeof(apng_frame_info_t));
        if (!frames) return NULL;

        int actual_frames = apng_collect_frames(file_data, file_len,
                                                num_frames, frames);
        if (actual_frames < 1) {
                KROSSHAIR_LOG("[APNG] no frames found\n");
                apng_free_frame_infos(frames, num_frames);
                return NULL;
        }
        if (actual_frames < (int)num_frames) {
                KROSSHAIR_LOG("[APNG] warning: expected %u frames but found %d\n",
                              num_frames, actual_frames);
        }

        size_t frame_stride = (size_t)canvas_w * canvas_h * 4;
        unsigned char* atlas = calloc(actual_frames, frame_stride);
        int* delays = malloc(sizeof(int) * actual_frames);
        if (!atlas || !delays) {
                free(atlas);
                free(delays);
                apng_free_frame_infos(frames, num_frames);
                return NULL;
        }

        apng_compose_frames(frames, actual_frames, ihdr_data,
                            canvas_w, canvas_h, delays, atlas);

        apng_free_frame_infos(frames, num_frames);

        *out_width  = (int)canvas_w;
        *out_height = (int)canvas_h;
        *out_frames = actual_frames;
        *out_delays = delays;

        KROSSHAIR_LOG("[APNG] decoded %d frames, canvas %ux%u\n",
                      actual_frames, canvas_w, canvas_h);

        return atlas;
}

/* ────────────────── end APNG loader ─────────────────── */
