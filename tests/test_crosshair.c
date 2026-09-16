/*
 * Unit tests for the device-independent parts of src/crosshair.c:
 * push-constant defaults and .cfg parsing, quad vertex setup, file
 * helpers (mtime, whole-file read, change detection), image path
 * resolution ($KROSSHAIR_IMG / crosshair-maker project dir), animation
 * state setup, and image decoding (static PNG, animated APNG atlas,
 * content sniffing for unknown extensions, garbage/missing files).
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "stb_image.h"
#include "test.h"
#include "../include/krosshair.h"

/* Exposed by src/crosshair.c under UNIT_TEST. */
extern void reset_dynamic_push_constants(struct dynamic_push_constants* pc);
extern void parse_dynamic_cfg(const char* path, struct dynamic_push_constants* pc);
extern char* get_crosshair_file(const char* path);
extern int get_file_mtime(const char* path, struct timespec* mtime);
extern unsigned char* read_file_whole(const char* path, const char* kind,
                                      size_t* len);
extern int image_file_changed(const char* current_path,
                              const struct timespec* current_mtime,
                              const char* new_path, const char* label,
                              int log_changes);
extern void setup_animation_state(swapchain_data_t* data, int frame_count,
                                  int frame_height, const int* delays_ms);
extern char* get_crosshair_path(void);
extern char* get_dynamic_mask_path(void);
extern char* get_dynamic_cfg_path(void);
extern int decode_crosshair_file(swapchain_data_t* data, const char* path,
                                 stbi_uc** out_pixels, int* out_width,
                                 int* out_height, VkDeviceSize* out_image_size);

/* ── in-memory PNG/APNG builders (same as tests/test_apng.c) ──── */

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
 * Build a complete 8-bit RGBA PNG (no acTL) in out. Returns the file
 * length. rgba: w*h*4 pixels, top-left origin.
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
 * IEND. Returns the file length.
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
                unsigned char fdat[4 + 7 + raw_len];
                be32(fdat + 0, i);
                size_t idat_len = zlib_store(raw, raw_len, fdat + 4);
                free(raw);
                off += put_chunk(out + off, "fdAT", fdat, (uint32_t)(4 + idat_len));
        }

        off += put_chunk(out + off, "IEND", NULL, 0);
        return off;
}

/* ── temp-file and environment helpers ────────────────────────── */

/*
 * Write buf to a per-PID temp file named /tmp/kh_xt_<pid>_<name>
 * (buf/len may be NULL/0 to create an empty file). Returns the path
 * from a static buffer, valid until the next call.
 */
static const char* write_tmp(const char* name, const void* buf, size_t len)
{
        static char path[300];
        snprintf(path, sizeof(path), "/tmp/kh_xt_%.4d_%s", (int)getpid(), name);
        FILE* f = fopen(path, "wb");
        if (f) {
                if (buf && len)
                        fwrite(buf, 1, len, f);
                fclose(f);
        }
        return path;
}

static void mkdir_p(const char* path)
{
        char buf[512];
        char* p;
        snprintf(buf, sizeof(buf), "%s", path);
        for (p = buf + 1; *p; p++) {
                if (*p == '/') {
                        *p = '\0';
                        (void)mkdir(buf, 0755);
                        *p = '/';
                }
        }
        (void)mkdir(buf, 0755);
}

static char fake_home[256];
static char proj_dir[512];

static void setup_fake_home(void)
{
        snprintf(fake_home, sizeof(fake_home), "/tmp/kh_home_%.4d", (int)getpid());
        snprintf(proj_dir, sizeof(proj_dir),
                 "%s/.config/crosshair-maker/projects", fake_home);
        mkdir_p(proj_dir);
}

static void touch_project_file(const char* name)
{
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", proj_dir, name);
        FILE* f = fopen(p, "w");
        if (f)
                fclose(f);
}

static void cleanup_fake_home(void)
{
        const char* names[] = {"current.apng", "current.gif", "current.png",
                               "current.dynamic.png", "current.dynamic.cfg"};
        char p[600];
        for (size_t i = 0; i < 5; i++) {
                snprintf(p, sizeof(p), "%s/%s", proj_dir, names[i]);
                (void)remove(p);
        }
        (void)rmdir(proj_dir);
        char mid[600];
        snprintf(mid, sizeof(mid), "%s/.config/crosshair-maker", fake_home);
        (void)rmdir(mid);
        snprintf(mid, sizeof(mid), "%s/.config", fake_home);
        (void)rmdir(mid);
        (void)rmdir(fake_home);
}

static char* save_env(const char* name)
{
        const char* v = getenv(name);
        return v ? strdup(v) : NULL;
}

static void restore_env(const char* name, char* saved)
{
        if (saved) {
                setenv(name, saved, 1);
                free(saved);
        } else {
                unsetenv(name);
        }
}

/* ── push constants ───────────────────────────────────────────── */

static void test_push_constants_defaults(void)
{
        struct dynamic_push_constants pc;
        memset(&pc, 0xAB, sizeof(pc));
        reset_dynamic_push_constants(&pc);

        CHECK(pc.invert_str == 0.0f);
        CHECK(pc.dodge_str == 0.0f && pc.dodge_r == 1.0f && pc.dodge_g == 1.0f && pc.dodge_b == 1.0f);
        CHECK(pc.burn_str == 0.0f && pc.burn_r == 1.0f && pc.burn_g == 1.0f && pc.burn_b == 1.0f);
        CHECK(pc.complement_str == 0.0f && pc.lumainvert_str == 0.0f);
        CHECK(pc.huerotate_str == 0.0f && pc.huerotate_angle == 180.0f);
        CHECK(pc.saturate_str == 0.0f && pc.saturate_amount == 0.0f);
        CHECK(pc.opacity == 1.0f);
}

static void test_parse_dynamic_cfg(void)
{
        const char* path = write_tmp("dyn.cfg", NULL, 0);
        {
                FILE* f = fopen(path, "w");
                const char* cfg =
                        "# comment line\n"
                        "invert 0.5\n"
                        "dodge 0.25 1 0.5 0.25\n"
                        "burn 0.75 0 1 0\n"
                        "complement 1\n"
                        "lumainvert 0.5\n"
                        "huerotate 0.6 90\n"
                        "saturate 0.8 1.5\n"
                        "opacity 0.9\n"
                        "unknownkey 0.5\n"
                        "shortline\n";
                fputs(cfg, f);
                fclose(f);
        }

        struct dynamic_push_constants pc;
        memset(&pc, 0, sizeof(pc));
        pc.quad_ndc_min[0] = 7.0f; /* must survive parsing */
        pc.quad_ndc_size[1] = 8.0f;
        parse_dynamic_cfg(path, &pc);

        CHECK(pc.invert_str == 0.5f);
        CHECK(pc.dodge_str == 0.25f && pc.dodge_r == 1.0f && pc.dodge_g == 0.5f && pc.dodge_b == 0.25f);
        CHECK(pc.burn_str == 0.75f && pc.burn_r == 0.0f && pc.burn_g == 1.0f && pc.burn_b == 0.0f);
        CHECK(pc.complement_str == 1.0f);
        CHECK(pc.lumainvert_str == 0.5f);
        CHECK(pc.huerotate_str == 0.6f && pc.huerotate_angle == 90.0f);
        CHECK(pc.saturate_str == 0.8f && pc.saturate_amount == 1.5f);
        CHECK(pc.opacity == 0.9f);
        CHECK(pc.quad_ndc_min[0] == 7.0f && pc.quad_ndc_size[1] == 8.0f);
        remove(path);
}

static void test_parse_dynamic_cfg_null_and_missing(void)
{
        struct dynamic_push_constants pc;
        memset(&pc, 0xAB, sizeof(pc));
        parse_dynamic_cfg(NULL, &pc);
        CHECK(pc.invert_str == 0.0f && pc.opacity == 1.0f && pc.saturate_amount == 0.0f);

        memset(&pc, 0xAB, sizeof(pc));
        parse_dynamic_cfg("/tmp/kh_xt_missing_dyn.cfg", &pc);
        CHECK(pc.dodge_str == 0.0f && pc.huerotate_angle == 180.0f);
        CHECK(pc.complement_str == 0.0f && pc.lumainvert_str == 0.0f);
}

/* ── vertex setup ─────────────────────────────────────────────── */

static void test_setup_vertices_even(void)
{
        vertex_t v[4];
        setup_vertices_uv(v, 100.0f, 100.0f, 10.0f, 20.0f, 1.0f, 0.0f, 1.0f);

        float w = (10.0f * 1.0f) / 100.0f;
        float h = (20.0f * 1.0f) / 100.0f;
        CHECK(v[0].pos.x == -w && v[0].pos.y == -h);
        CHECK(v[1].pos.x == w && v[1].pos.y == -h);
        CHECK(v[2].pos.x == w && v[2].pos.y == h);
        CHECK(v[3].pos.x == -w && v[3].pos.y == h);
        CHECK(v[0].tex_pos.x == 0.0f && v[0].tex_pos.y == 0.0f);
        CHECK(v[1].tex_pos.x == 1.0f && v[1].tex_pos.y == 0.0f);
        CHECK(v[2].tex_pos.x == 1.0f && v[2].tex_pos.y == 1.0f);
        CHECK(v[3].tex_pos.x == 0.0f && v[3].tex_pos.y == 1.0f);
}

static void test_setup_vertices_odd_canvas(void)
{
        vertex_t v[4];
        setup_vertices_uv(v, 101.0f, 99.0f, 10.0f, 20.0f, 1.0f, 0.0f, 1.0f);

        float w = (10.0f * 1.0f) / 101.0f;
        float h = (20.0f * 1.0f) / 99.0f;
        float ox = 1.0f / 101.0f;
        float oy = 1.0f / 99.0f;
        CHECK(v[0].pos.x == -w + ox && v[0].pos.y == -h + oy);
        CHECK(v[1].pos.x == w + ox && v[1].pos.y == -h + oy);
        CHECK(v[2].pos.x == w + ox && v[2].pos.y == h + oy);
        CHECK(v[3].pos.x == -w + ox && v[3].pos.y == h + oy);
        CHECK(v[0].tex_pos.y == 0.0f && v[2].tex_pos.y == 1.0f);
}

static void test_setup_vertices_scale_and_uv_slice(void)
{
        vertex_t v[4];
        setup_vertices_uv(v, 100.0f, 100.0f, 10.0f, 20.0f, 2.0f, 0.25f, 0.75f);

        float w = (10.0f * 2.0f) / 100.0f;
        float h = (20.0f * 2.0f) / 100.0f;
        CHECK(v[0].pos.x == -w && v[0].pos.y == -h);
        CHECK(v[2].pos.x == w && v[2].pos.y == h);
        CHECK(v[0].tex_pos.y == 0.25f && v[1].tex_pos.y == 0.25f);
        CHECK(v[2].tex_pos.y == 0.75f && v[3].tex_pos.y == 0.75f);
        CHECK(v[0].tex_pos.x == 0.0f && v[1].tex_pos.x == 1.0f);
        CHECK(v[3].tex_pos.x == 0.0f && v[2].tex_pos.x == 1.0f);
}

/* ── path expansion ───────────────────────────────────────────── */

static void test_get_crosshair_file(void)
{
        char* home = save_env("HOME");
        setenv("HOME", "/home/user", 1);

        CHECK_PTR(get_crosshair_file(NULL), NULL);

        char* p = get_crosshair_file("/abs/path.png");
        CHECK_STR(p, "/abs/path.png");
        free(p);

        p = get_crosshair_file("~/cross.png");
        CHECK_STR(p, "/home/user/cross.png");
        free(p);

        p = get_crosshair_file("~");
        CHECK_STR(p, "/home/user");
        free(p);

        restore_env("HOME", home);
}

/* ── file helpers ─────────────────────────────────────────────── */

static void test_file_helpers(void)
{
        char* content = "hello world";
        const char* path = write_tmp("file.bin", content, strlen(content));

        struct timespec mtime;
        CHECK_EQ(get_file_mtime(path, &mtime), 0);
        CHECK_EQ(get_file_mtime("/tmp/kh_xt_missing_file.bin", &mtime), -1);

        size_t len = 0;
        unsigned char* buf = read_file_whole(path, "test", &len);
        CHECK(buf != NULL);
        CHECK_EQ(len, strlen(content));
        CHECK(buf && memcmp(buf, content, len) == 0);
        free(buf);

        buf = read_file_whole("/tmp/kh_xt_missing_file.bin", "test", &len);
        CHECK_PTR(buf, NULL);

        const char* empty_path = write_tmp("empty.bin", NULL, 0);
        buf = read_file_whole(empty_path, "test", &len);
        CHECK_PTR(buf, NULL);

        remove(path);
        remove(empty_path);
}

static void test_image_file_changed(void)
{
        char* content = "data";
        const char* path = write_tmp("change.bin", content, strlen(content));

        struct timespec now;
        CHECK_EQ(get_file_mtime(path, &now), 0);

        struct timespec old;
        old.tv_sec = now.tv_sec - 10;
        old.tv_nsec = now.tv_nsec;

        /* different path -> changed */
        CHECK_EQ(image_file_changed("/other/path.bin", &now, path, "x", 0), 1);
        /* same path, mtime differs -> changed */
        CHECK_EQ(image_file_changed(path, &old, path, "x", 0), 1);
        /* same path, mtime identical -> unchanged */
        CHECK_EQ(image_file_changed(path, &now, path, "x", 0), 0);
        /* same path, file vanished -> unchanged */
        remove(path);
        CHECK_EQ(image_file_changed(path, &old, path, "x", 0), 0);
}

/* ── animation state ──────────────────────────────────────────── */

static void test_setup_animation_state(void)
{
        swapchain_data_t data;
        memset(&data, 0, sizeof(data));
        setup_animation_state(&data, 3, 2, NULL);
        CHECK_EQ(data.anim_frame_count, 3);
        CHECK_EQ(data.anim_frame_height, 2);
        CHECK_EQ(data.anim_current_frame, 0);
        CHECK(data.anim_delays != NULL);
        if (data.anim_delays) {
                CHECK_EQ(data.anim_delays[0], 100);
                CHECK_EQ(data.anim_delays[1], 100);
                CHECK_EQ(data.anim_delays[2], 100);
                free(data.anim_delays);
        }

        memset(&data, 0, sizeof(data));
        const int delays[] = { 150, 0, 300 };
        setup_animation_state(&data, 3, 2, delays);
        CHECK_EQ(data.anim_frame_count, 3);
        CHECK(data.anim_delays != NULL);
        if (data.anim_delays) {
                CHECK_EQ(data.anim_delays[0], 150);
                CHECK_EQ(data.anim_delays[1], 100); /* non-positive clamped */
                CHECK_EQ(data.anim_delays[2], 300);
                free(data.anim_delays);
        }
}

/* ── crosshair path resolution ────────────────────────────────── */

static void test_get_crosshair_path(void)
{
        char* home = save_env("HOME");
        char* img = save_env("KROSSHAIR_IMG");
        setup_fake_home();
        setenv("HOME", fake_home, 1);

        /* explicit absolute path */
        setenv("KROSSHAIR_IMG", "/abs/x.png", 1);
        char* p = get_crosshair_path();
        CHECK_STR(p, "/abs/x.png");
        free(p);

        /* explicit tilde path is expanded against HOME */
        setenv("KROSSHAIR_IMG", "~/y.png", 1);
        char expected[600];
        snprintf(expected, sizeof(expected), "%s/y.png", fake_home);
        p = get_crosshair_path();
        CHECK_STR(p, expected);
        free(p);

        /* no explicit path: apng > gif > png priority */
        unsetenv("KROSSHAIR_IMG");
        touch_project_file("current.apng");
        touch_project_file("current.gif");
        touch_project_file("current.png");
        p = get_crosshair_path();
        snprintf(expected, sizeof(expected), "%s/current.apng", proj_dir);
        CHECK_STR(p, expected);
        free(p);

        remove(expected);
        p = get_crosshair_path();
        snprintf(expected, sizeof(expected), "%s/current.gif", proj_dir);
        CHECK_STR(p, expected);
        free(p);

        remove(expected);
        p = get_crosshair_path();
        snprintf(expected, sizeof(expected), "%s/current.png", proj_dir);
        CHECK_STR(p, expected);
        free(p);

        remove(expected);
        p = get_crosshair_path();
        CHECK_PTR(p, NULL);

        cleanup_fake_home();
        restore_env("HOME", home);
        restore_env("KROSSHAIR_IMG", img);
}

static void test_dynamic_paths(void)
{
        char* home = save_env("HOME");
        setup_fake_home();
        setenv("HOME", fake_home, 1);

        char* p = get_dynamic_mask_path();
        CHECK_PTR(p, NULL);
        free(p);

        char expected[600];
        touch_project_file("current.dynamic.png");
        p = get_dynamic_mask_path();
        snprintf(expected, sizeof(expected), "%s/current.dynamic.png", proj_dir);
        CHECK_STR(p, expected);
        free(p);

        p = get_dynamic_cfg_path();
        CHECK_PTR(p, NULL);
        free(p);

        touch_project_file("current.dynamic.cfg");
        p = get_dynamic_cfg_path();
        snprintf(expected, sizeof(expected), "%s/current.dynamic.cfg", proj_dir);
        CHECK_STR(p, expected);
        free(p);

        cleanup_fake_home();
        restore_env("HOME", home);
}

/* ── image decoding ───────────────────────────────────────────── */

static void test_decode_static_png(void)
{
        static const unsigned char px[16] = {
                255, 0, 0, 255,   0, 255, 0, 255,
                0, 0, 255, 255,   255, 255, 0, 255
        };
        unsigned char png[256];
        size_t len = build_rgba_png(png, 2, 2, px);
        const char* path = write_tmp("static.png", png, len);

        swapchain_data_t data;
        memset(&data, 0, sizeof(data));
        stbi_uc* pixels = NULL;
        int w = 0, h = 0;
        VkDeviceSize size = 0;
        CHECK_EQ(decode_crosshair_file(&data, path, &pixels, &w, &h, &size), 1);
        CHECK_EQ(w, 2);
        CHECK_EQ(h, 2);
        CHECK_EQ(size, 16);
        CHECK(pixels != NULL);
        if (pixels) {
                CHECK(pixels[0] == 255 && pixels[1] == 0);
                CHECK(pixels[5] == 255 && pixels[6] == 0);
                stbi_image_free(pixels);
        }
        CHECK_EQ(data.anim_frame_count, 0); /* no animation state recorded */
        remove(path);
}

static void test_decode_animated_apng(void)
{
        static const unsigned char red[8] = { 255, 0, 0, 255,  255, 0, 0, 255 };
        static const unsigned char green[8] = { 0, 255, 0, 255,  0, 255, 0, 255 };
        const unsigned char* frames[2] = { red, green };
        uint16_t nums[2] = { 1, 2 };
        uint16_t dens[2] = { 10, 10 };
        unsigned char apng[512];
        size_t len = build_apng(apng, 2, 2, 2, frames, nums, dens);
        const char* path = write_tmp("anim.apng", apng, len);

        swapchain_data_t data;
        memset(&data, 0, sizeof(data));
        stbi_uc* pixels = NULL;
        int w = 0, h = 0;
        VkDeviceSize size = 0;
        CHECK_EQ(decode_crosshair_file(&data, path, &pixels, &w, &h, &size), 1);
        CHECK_EQ(w, 2);
        CHECK_EQ(h, 4); /* vertical atlas: 2 frames x 2 px */
        CHECK_EQ(size, 32);
        CHECK_EQ(data.anim_frame_count, 2);
        CHECK_EQ(data.anim_frame_height, 2);
        CHECK(data.anim_delays != NULL);
        if (data.anim_delays) {
                CHECK_EQ(data.anim_delays[0], 100);
                CHECK_EQ(data.anim_delays[1], 200);
                free(data.anim_delays);
        }
        if (pixels)
                stbi_image_free(pixels);
        remove(path);
}

static void test_decode_garbage_and_missing(void)
{
        unsigned char garbage[32];
        memset(garbage, 0xAB, sizeof(garbage));

        struct decode_case {
                const char* name;
        };
        const struct decode_case cases[] = { { "bad.png" }, { "bad.gif" }, { "bad.bin" } };
        for (size_t i = 0; i < 3; i++) {
                const char* path = write_tmp(cases[i].name, garbage, sizeof(garbage));
                stbi_uc* pixels = NULL;
                int w = 0, h = 0;
                VkDeviceSize size = 0;
                CHECK_EQ(decode_crosshair_file(NULL, path, &pixels, &w, &h, &size), 0);
                CHECK_PTR(pixels, NULL);
                remove(path);
        }

        stbi_uc* pixels = NULL;
        int w = 0, h = 0;
        VkDeviceSize size = 0;
        CHECK_EQ(decode_crosshair_file(NULL, "/tmp/kh_xt_missing.png",
                                       &pixels, &w, &h, &size), 0);
        CHECK_PTR(pixels, NULL);
}

static void test_decode_content_sniffing(void)
{
        /* unknown extension: decoded by content, not by name */
        static const unsigned char px[8] = { 255, 0, 0, 255,  0, 0, 255, 255 };
        unsigned char png[256];
        size_t len = build_rgba_png(png, 1, 2, px);
        const char* path = write_tmp("image.bin", png, len);

        stbi_uc* pixels = NULL;
        int w = 0, h = 0;
        VkDeviceSize size = 0;
        CHECK_EQ(decode_crosshair_file(NULL, path, &pixels, &w, &h, &size), 1);
        CHECK_EQ(w, 1);
        CHECK_EQ(h, 2);
        if (pixels)
                stbi_image_free(pixels);
        remove(path);
}

int main(void)
{
        crc_init();
        fprintf(stderr, "# test_crosshair\n");
        test_push_constants_defaults();
        test_parse_dynamic_cfg();
        test_parse_dynamic_cfg_null_and_missing();
        test_setup_vertices_even();
        test_setup_vertices_odd_canvas();
        test_setup_vertices_scale_and_uv_slice();
        test_get_crosshair_file();
        test_file_helpers();
        test_image_file_changed();
        test_setup_animation_state();
        test_get_crosshair_path();
        test_dynamic_paths();
        test_decode_static_png();
        test_decode_animated_apng();
        test_decode_garbage_and_missing();
        test_decode_content_sniffing();
        fprintf(stderr, "  -> %d passed, %d failed\n", test_passed, test_failed);
        return test_failed ? 1 : 0;
}
