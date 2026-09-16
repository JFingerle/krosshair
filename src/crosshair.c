/*
 * Crosshair image handling.
 *
 * Loads the user's crosshair image (PNG/APNG) and the optional dynamic
 * mask, decodes it, builds the per-swapchain crosshair draw data
 * (vertex buffers with UV coordinates, draw ring slots) and hot-reloads
 * everything when the files on disk change. Owns the stb_image
 * implementation (STB_IMAGE_IMPLEMENTATION lives in this translation
 * unit).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../include/stb_image.h"
#include "../include/default_crosshair.h"
#include "../include/krosshair.h"

/*
 * UNIT_TEST build: expose the pure (device-independent) helpers so the
 * unit tests in tests/test_crosshair.c can call them directly.
 */
#ifdef UNIT_TEST
#define CROSSHAIR_API
#else
#define CROSSHAIR_API static
#endif

/*
 * Reset dynamic push constants to safe defaults: every effect off,
 * identity colors, opacity 1. quad_ndc_min/size are left untouched —
 * the caller sets those from the canvas geometry.
 *
 * pc: push-constants struct written in place.
 */
CROSSHAIR_API void reset_dynamic_push_constants(struct dynamic_push_constants* pc)
{
        pc->invert_str      = 0.0f;
        pc->dodge_str       = 0.0f;
        pc->dodge_r = 1.0f; pc->dodge_g = 1.0f; pc->dodge_b = 1.0f;
        pc->burn_str        = 0.0f;
        pc->burn_r = 1.0f;  pc->burn_g = 1.0f;  pc->burn_b = 1.0f;
        pc->complement_str  = 0.0f;
        pc->lumainvert_str  = 0.0f;
        pc->huerotate_str   = 0.0f;
        pc->huerotate_angle = 180.0f;
        pc->saturate_str    = 0.0f;
        pc->saturate_amount = 0.0f;
        pc->opacity         = 1.0f;
}

/*
 * Parse a crosshair-maker dynamic .cfg file into shader push
 * constants, starting from safe defaults.
 *
 * path:  path to the .cfg file; NULL or a missing file simply leaves
 *        the defaults (all effects off, identity colors, opacity 1.0).
 * pc:    push-constants struct written in place. quad_ndc_min/size are
 *        not touched — the caller sets those from the canvas geometry.
 *
 * Each line is "name a1 [a2 a3 a4]"; unknown names, short lines, and
 * '#' comments are ignored.
 */
CROSSHAIR_API void parse_dynamic_cfg(const char* path, struct dynamic_push_constants* pc)
{
        reset_dynamic_push_constants(pc);

        if (!path) return;

        FILE* f = fopen(path, "r");
        if (!f) return;

        char line[512];
        while (fgets(line, sizeof(line), f)) {
                /* skip comments and blank lines */
                if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
                        continue;

                char name[64];
                float a1 = 0, a2 = 0, a3 = 0, a4 = 0;
                int parsed = sscanf(line, "%63s %f %f %f %f", name, &a1, &a2, &a3, &a4);
                if (parsed < 2) continue;

                if (strcmp(name, "invert") == 0) {
                        pc->invert_str = a1;
                } else if (strcmp(name, "dodge") == 0) {
                        pc->dodge_str = a1;
                        if (parsed >= 5) { pc->dodge_r = a2; pc->dodge_g = a3; pc->dodge_b = a4; }
                } else if (strcmp(name, "burn") == 0) {
                        pc->burn_str = a1;
                        if (parsed >= 5) { pc->burn_r = a2; pc->burn_g = a3; pc->burn_b = a4; }
                } else if (strcmp(name, "complement") == 0) {
                        pc->complement_str = a1;
                } else if (strcmp(name, "lumainvert") == 0) {
                        pc->lumainvert_str = a1;
                } else if (strcmp(name, "huerotate") == 0) {
                        pc->huerotate_str = a1;
                        if (parsed >= 3) pc->huerotate_angle = a2;
                } else if (strcmp(name, "saturate") == 0) {
                        pc->saturate_str = a1;
                        if (parsed >= 3) pc->saturate_amount = a2;
                } else if (strcmp(name, "opacity") == 0) {
                        pc->opacity = a1;
                }
        }
        fclose(f);
}



/*
 * Fill a centered quad (4 vertices, two triangles via indices[]) for
 * drawing a texture scaled to NDC, selecting the vertical UV slice
 * [uv_top, uv_bottom].
 *
 * vertices:       4 vertex_t slots, filled in place;
 * canvas_width/canvas_height: app (swapchain) size in pixels — the
 *               reference the texture size is scaled against;
 * tex_width/tex_height: crosshair texture size in pixels;
 * scale:            uniform scale factor (1.0 = native size);
 * uv_top/uv_bottom: top/bottom UV of the quad — 0..1 for a full
 *               texture, a sub-range to pick one frame row of an
 *               atlas.
 */
void setup_vertices_uv(vertex_t* vertices,
                               float canvas_width, float canvas_height,
                               float tex_width, float tex_height, float scale,
                               float uv_top, float uv_bottom)
{
        float width_ndc  = ((tex_width * scale) / canvas_width);
        float height_ndc = ((tex_height * scale) / canvas_height);

        /* Odd-sized canvases have no center pixel — the screen center
         * falls between two pixels. Shift the whole quad by half a
         * pixel in NDC (1 px = 2.0/canvas in NDC) to keep it on the
         * pixel grid. */
        float pixel_offset_x =
            (fmod(canvas_width, 2.0f) == 0) ? 0.0f : (1.0f / canvas_width);
        float pixel_offset_y =
            (fmod(canvas_height, 2.0f) == 0) ? 0.0f : (1.0f / canvas_height);

        vertices[0].pos =
            (vec2_t){-width_ndc + pixel_offset_x, -height_ndc + pixel_offset_y};
        vertices[0].tex_pos = (vec2_t){0.0f, uv_top};
        vertices[1].pos =
            (vec2_t){width_ndc + pixel_offset_x, -height_ndc + pixel_offset_y};
        vertices[1].tex_pos = (vec2_t){1.0f, uv_top};
        vertices[2].pos =
            (vec2_t){width_ndc + pixel_offset_x, height_ndc + pixel_offset_y};
        vertices[2].tex_pos = (vec2_t){1.0f, uv_bottom};
        vertices[3].pos =
            (vec2_t){-width_ndc + pixel_offset_x, height_ndc + pixel_offset_y};
        vertices[3].tex_pos = (vec2_t){0.0f, uv_bottom};
}

/*
 * setup_vertices_uv for the full texture (uv 0..1) — used for static
 * (non-atlas) crosshairs. Same parameters as setup_vertices_uv minus
 * the UV range.
 */
static void setup_vertices(vertex_t* vertices,
                            float canvas_width, float canvas_height,
                            float tex_width, float tex_height, float scale)
{
        setup_vertices_uv(vertices, canvas_width, canvas_height, tex_width,
                          tex_height, scale, 0.0f, 1.0f);
}

uint16_t indices[] = {0, 1, 2, 2, 3, 0};

/*
 * Expand a user-provided image path: a leading '~' is replaced by
 * $HOME (so both "~" and "~/crosshair.png" work); any other path is
 * returned unchanged.
 *
 * path: path from $KROSSHAIR_IMG (must be non-NULL).
 * Returns a malloc'd copy (caller frees), or NULL if HOME is unset.
 */
CROSSHAIR_API char* get_crosshair_file(const char* path)
{
        if (!path) return NULL;

        if (!(path[0] == '~')) return strdup(path);

        const char* home_dir = getenv("HOME");
        if (!home_dir) return NULL;

        const char* rest_str = path + 1;
        while (*rest_str && *rest_str != '/') rest_str++;

        char* expanded_str = malloc(strlen(home_dir) + strlen(rest_str) + 1);
        strcpy(expanded_str, home_dir);
        strcat(expanded_str, rest_str);
        return expanded_str;
}

/*
 * Fetch a file's modification time.
 *
 * path:  file path;
 * mtime: set to the file's mtime on success.
 * Returns 0 on success, -1 if stat() fails (e.g. file missing).
 */
CROSSHAIR_API int get_file_mtime(const char* path, struct timespec* mtime)
{
        struct stat st;
        if (stat(path, &st) != 0) return -1;
        *mtime = st.st_mtim;
        return 0;
}

/*
 * Read an entire file into a malloc'd buffer.
 *
 * Both in-memory decoders (stbi GIF, load_apng) take a 32-bit length, so
 * files larger than INT_MAX bytes are rejected here.
 *
 * path: file to read;
 * kind: short label for the error logs (e.g. "GIF");
 * len:  set to the file size in bytes on success.
 * Returns the buffer (caller frees), or NULL on open/size/alloc/short-
 * read failure (each failure is logged).
 */
CROSSHAIR_API unsigned char* read_file_whole(const char* path, const char* kind,
                                      size_t* len)
{
        FILE* f = fopen(path, "rb");
        if (!f) {
                KROSSHAIR_LOG("[KROSSHAIR_ERROR] failed to open %s: %s\n",
                              kind, path);
                return NULL;
        }

        fseek(f, 0, SEEK_END);
        long file_len = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (file_len <= 0 || file_len > (long)INT_MAX) {
                fclose(f);
                KROSSHAIR_LOG("[KROSSHAIR_ERROR] invalid %s file size: %ld\n",
                              kind, file_len);
                return NULL;
        }

        unsigned char* buf = malloc((size_t)file_len);
        if (!buf) {
                fclose(f);
                KROSSHAIR_LOG("[KROSSHAIR_ERROR] failed to allocate %s read buffer\n",
                              kind);
                return NULL;
        }

        size_t bytes_read = fread(buf, 1, (size_t)file_len, f);
        fclose(f);

        if (bytes_read != (size_t)file_len) {
                free(buf);
                KROSSHAIR_LOG("[KROSSHAIR_ERROR] short read on %s: %zu/%ld\n",
                              kind, bytes_read, file_len);
                return NULL;
        }

        *len = (size_t)file_len;
        return buf;
}

/*
 * Check whether a file we already uploaded from needs reloading.
 *
 * current_path:    path the image was last loaded from;
 * current_mtime:   mtime recorded at that load;
 * new_path:        path the file should come from now;
 * label:           short name for the log lines ("crosshair"/"mask");
 * log_changes:     log when the path or mtime changed.
 * Returns 1 if the file moved or its mtime changed, 0 otherwise.
 */
CROSSHAIR_API int image_file_changed(const char* current_path,
                              const struct timespec* current_mtime,
                              const char* new_path,
                              const char* label, int log_changes)
{
        if (strcmp(current_path, new_path) != 0) {
                if (log_changes)
                        KROSSHAIR_LOG("[KROSSHAIR] %s path changed, reloading\n",
                                      label);
                return 1;
        }

        struct timespec new_mtime;
        if (get_file_mtime(new_path, &new_mtime) == 0 &&
            (new_mtime.tv_sec != current_mtime->tv_sec ||
             new_mtime.tv_nsec != current_mtime->tv_nsec)) {
                if (log_changes)
                        KROSSHAIR_LOG("[KROSSHAIR] %s mtime changed (%ld.%ld -> %ld.%ld), reloading\n",
                                      label,
                                      current_mtime->tv_sec, current_mtime->tv_nsec,
                                      new_mtime.tv_sec, new_mtime.tv_nsec);
                return 1;
        }

        return 0;
}

/*
 * Record animation state for a freshly loaded frame atlas: one frame is
 * drawn per tick, each after its delay in ms.
 *
 * data:        swapchain holding the anim_* fields (anim_delays must be
 *              NULL — on reload it is freed by shutdown_krosshair_image);
 * frame_count: number of frames stacked in the atlas;
 * frame_height: height of one frame in pixels;
 * delays_ms:   per-frame delay in milliseconds, owned by the caller
 *              (NULL gives a uniform 100 ms delay for every frame).
 *
 * Non-positive delays become 100 ms ("as fast as possible" in both
 * source formats is clamped to a sane tick).
 */
CROSSHAIR_API void setup_animation_state(swapchain_data_t* data, int frame_count,
                                  int frame_height, const int* delays_ms)
{
        data->anim_frame_count   = frame_count;
        data->anim_frame_height  = frame_height;
        data->anim_current_frame = 0;
        clock_gettime(CLOCK_MONOTONIC, &data->anim_last_frame_time);

        data->anim_delays = malloc(sizeof(int) * frame_count);
        for (int i = 0; i < frame_count; i++)
                data->anim_delays[i] = delays_ms && delays_ms[i] > 0
                    ? delays_ms[i] : 100;
}

/*
 * Resolve the crosshair image path for this session.
 *
 * Priority: $KROSSHAIR_IMG (expanded via get_crosshair_file), else the
 * crosshair-maker project dir (~/.config/crosshair-maker/projects/)
 * where current.apng, then current.gif, then current.png is tried in
 * order — animated formats first, first existing file wins.
 *
 * Returns a malloc'd path (caller frees), or NULL if no source exists
 * (caller then falls back to the built-in crosshair).
 */
CROSSHAIR_API char* get_crosshair_path(void)
{
        const char* explicit = getenv("KROSSHAIR_IMG");
        if (explicit)
                return get_crosshair_file(explicit);

        const char* home = getenv("HOME");
        if (!home)
                return NULL;

        char cm_path[4096];
        struct stat st;

        /* prefer animated formats first, then static PNG */
        snprintf(cm_path, sizeof(cm_path),
                 "%s/.config/crosshair-maker/projects/current.apng", home);
        if (stat(cm_path, &st) == 0)
                return strdup(cm_path);

        snprintf(cm_path, sizeof(cm_path),
                 "%s/.config/crosshair-maker/projects/current.gif", home);
        if (stat(cm_path, &st) == 0)
                return strdup(cm_path);

        snprintf(cm_path, sizeof(cm_path),
                 "%s/.config/crosshair-maker/projects/current.png", home);
        if (stat(cm_path, &st) == 0)
                return strdup(cm_path);

        return NULL;
}

/*
 * Path of the crosshair-maker dynamic mask
 * (~/.config/crosshair-maker/projects/current.dynamic.png).
 *
 * Returns a malloc'd path (caller frees), or NULL if the file does not
 * exist or HOME is unset.
 */
CROSSHAIR_API char* get_dynamic_mask_path(void)
{
        const char* home = getenv("HOME");
        if (!home) return NULL;

        char cm_path[4096];
        struct stat st;

        snprintf(cm_path, sizeof(cm_path),
                 "%s/.config/crosshair-maker/projects/current.dynamic.png",
                 home);
        if (stat(cm_path, &st) == 0)
                return strdup(cm_path);

        return NULL;
}

/*
 * Path of the crosshair-maker dynamic effect config
 * (~/.config/crosshair-maker/projects/current.dynamic.cfg).
 *
 * Returns a malloc'd path (caller frees), or NULL if the file does not
 * exist or HOME is unset.
 */
CROSSHAIR_API char* get_dynamic_cfg_path(void)
{
        const char* home = getenv("HOME");
        if (!home) return NULL;

        char cm_path[4096];
        struct stat st;

        snprintf(cm_path, sizeof(cm_path),
                 "%s/.config/crosshair-maker/projects/current.dynamic.cfg",
                 home);
        if (stat(cm_path, &st) == 0)
                return strdup(cm_path);

        return NULL;
}

/*
 * Decode a crosshair image file (GIF, APNG or PNG) into an RGBA pixel
 * buffer.
 *
 * Animated sources (GIF, multi-frame APNG) are laid out as a vertical
 * frame atlas — all frames stacked top to bottom in a single image — and
 * their per-frame delays are recorded on the swapchain via
 * setup_animation_state, so *out_height is the full atlas height for
 * animations.
 *
 * data:           swapchain to record the animation state in (only
 *                 touched for animated sources);
 * path:           image file to decode;
 * out_pixels:     set to the decoded RGBA pixels (free with
 *                 stbi_image_free), or NULL on failure;
 * out_width:      set to the texture width in pixels;
 * out_height:     set to the texture height in pixels (full atlas for
 *                 animations);
 * out_image_size: set to the pixel data size in bytes.
 *
 * Returns 1 if a valid image was decoded, 0 on failure (already logged).
 */
CROSSHAIR_API int decode_crosshair_file(swapchain_data_t* data, const char* path,
                                 stbi_uc** out_pixels, int* out_width,
                                 int* out_height, VkDeviceSize* out_image_size)
{
        *out_pixels = NULL;

        const char* ext = strrchr(path, '.');
        int is_gif  = ext && (strcasecmp(ext, ".gif") == 0);
        int is_apng = ext && (strcasecmp(ext, ".apng") == 0);

        /* .png files might also be APNG -- detect by checking for
         * acTL chunk if the extension is .png */
        int is_png = ext && (strcasecmp(ext, ".png") == 0);

        if (is_gif) {
                size_t file_len = 0;
                unsigned char* file_buf =
                    read_file_whole(path, "GIF", &file_len);
                if (!file_buf)
                        return 0;

                int* delays = NULL;
                int frames = 0;
                int tex_channels;
                *out_pixels = stbi_load_gif_from_memory(
                    file_buf, (int)file_len, &delays, out_width,
                    out_height, &frames, &tex_channels, STBI_rgb_alpha);
                free(file_buf);

                if (!*out_pixels || frames < 1) {
                        KROSSHAIR_LOG("[KROSSHAIR_ERROR] failed to decode GIF: %s (%s)\n",
                                      path,
                                      *out_pixels ? "no frames" : "decode error");
                        if (*out_pixels) stbi_image_free(*out_pixels);
                        if (delays) free(delays);
                        *out_pixels = NULL;
                        return 0;
                }

                /*
                 * Build a vertical texture atlas: all frames stacked
                 * top-to-bottom in a single image.  The UV coordinates
                 * are adjusted per-frame to select the right slice.
                 *
                 * gif_data from stbi is already laid out as
                 * [frame0][frame1]...[frameN] contiguously, each
                 * frame being (width * frame_height * 4) bytes,
                 * which is exactly the atlas layout we need.
                 */
                int frame_height = *out_height;
                *out_height = frame_height * frames;
                *out_image_size = (VkDeviceSize)*out_width * *out_height * 4;

                /* stbi already converted GIF centisecond delays to ms */
                setup_animation_state(data, frames, frame_height, delays);
                free(delays);

                KROSSHAIR_LOG("[KROSSHAIR] loaded GIF atlas: %dx%d (%d frames, frame_h=%d)\n",
                              *out_width, *out_height, frames, frame_height);
        } else if (is_apng || is_png) {
                /* try to load as APNG; if it's a plain PNG the
                 * loader will return NULL (no acTL) and we fall
                 * through to stbi_load below */
                size_t file_len = 0;
                unsigned char* file_buf =
                    read_file_whole(path, "APNG", &file_len);

                int* apng_delays = NULL;
                int apng_frames = 0;
                int apng_w = 0, apng_h = 0;
                unsigned char* apng_data = NULL;

                if (file_buf) {
                        apng_data = load_apng((const unsigned char*)file_buf,
                                              (size_t)file_len, &apng_w, &apng_h,
                                              &apng_frames, &apng_delays);
                        free(file_buf);
                }

                if (apng_data && apng_frames > 1) {
                        /* animated APNG -- same atlas approach as GIF */
                        *out_width  = apng_w;
                        int frame_height = apng_h;
                        *out_height = apng_h * apng_frames;
                        *out_image_size = (VkDeviceSize)*out_width * *out_height * 4;
                        *out_pixels = (stbi_uc*)apng_data;

                        setup_animation_state(data, apng_frames,
                                              frame_height, apng_delays);
                        free(apng_delays);

                        KROSSHAIR_LOG("[KROSSHAIR] loaded APNG atlas: %dx%d (%d frames, frame_h=%d)\n",
                                      *out_width, *out_height, apng_frames, frame_height);
                } else {
                        /* not animated APNG (or single frame) --
                         * fall back to regular stbi_load for
                         * proper PNG handling */
                        if (apng_data) free(apng_data);
                        if (apng_delays) free(apng_delays);

                        int tex_channels;
                        *out_pixels = stbi_load(path, out_width, out_height,
                                                &tex_channels, STBI_rgb_alpha);
                        *out_image_size = (VkDeviceSize)*out_width * *out_height * 4;
                }
        } else {
                int tex_channels;
                *out_pixels = stbi_load(path, out_width, out_height,
                                        &tex_channels, STBI_rgb_alpha);
                *out_image_size = (VkDeviceSize)*out_width * *out_height * 4;
        }

        return *out_pixels != NULL;
}

/*
 * Make sure this swapchain has an up-to-date crosshair image uploaded.
 *
 * If no image is uploaded yet (or one needs reloading because the path
 * or mtime of the file changed), it loads the crosshair (GIF, APNG or
 * PNG — animated sources become a vertical frame atlas), creates the
 * GPU image/view/descriptor set, encodes the pixel upload into
 * cmd_buffer, and builds the draw vertices. A missing or unreadable
 * file falls back to the built-in crosshair.
 *
 * data:        swapchain to keep the crosshair of;
 * cmd_buffer:  command buffer the pixel upload is encoded into.
 */
void ensure_swapchain_crosshair(swapchain_data_t* data,
                                VkCommandBuffer cmd_buffer)
{
        device_data_t* device_data = data->device_data;

        static int kh_msg_shown_built_in = 0;
        static int kh_msg_shown_load_fail = 0;
        static int kh_msg_shown_file_load = 0;

        char* crosshair_path = get_crosshair_path();
        int using_file = (crosshair_path != NULL);

        if (data->crosshair_uploaded) {
                int needs_reload =
                    using_file && data->crosshair_path &&
                    image_file_changed(data->crosshair_path,
                                       &data->crosshair_mtime, crosshair_path,
                                       "crosshair", 1);

                if (!needs_reload) {
                        free(crosshair_path);
                        return;
                }

                KROSSHAIR_LOG("[KROSSHAIR] shutting down old crosshair image\n");

                shutdown_krosshair_image(data);
                data->crosshair_uploaded = 0;
                free(data->crosshair_path);
                data->crosshair_path = NULL;
        }

        int tex_width;
        int tex_height;
        VkDeviceSize image_size;
        stbi_uc* pixels = NULL;

        if (using_file &&
            decode_crosshair_file(data, crosshair_path, &pixels, &tex_width,
                                  &tex_height, &image_size)) {
                data->descriptor_set = create_image_with_desc(
                    data, tex_width, tex_height, VK_FORMAT_R8G8B8A8_SRGB,
                    &data->crosshair_image, &data->crosshair_mem,
                    &data->crosshair_image_view);

                upload_image_data(
                    device_data, cmd_buffer, pixels, image_size, tex_width,
                    tex_height, &data->crosshair_upload_buffer,
                    &data->crosshair_upload_buffer_mem, data->crosshair_image);
                stbi_image_free(pixels);

                /* the path's ownership moves to the swapchain so its
                 * mtime can be watched for hot-reloads */
                data->crosshair_path = crosshair_path;
                get_file_mtime(data->crosshair_path, &data->crosshair_mtime);
                KROSSHAIR_LOG("[KROSSHAIR] loaded crosshair from: %s (mtime %ld.%ld)\n",
                              data->crosshair_path,
                              data->crosshair_mtime.tv_sec,
                              data->crosshair_mtime.tv_nsec);
                if (!kh_msg_shown_file_load) {
                        const char* reason = getenv("KROSSHAIR_IMG") ?
                                "Set via env var 'KROSSHAIR_IMG'" :
                                "Default crosshair location";
                        fprintf(stderr, "[KH] Loading crosshair from file '%s'. Reason: %s\n",
                                data->crosshair_path, reason);
                        kh_msg_shown_file_load = 1;
                }
        } else {
                if (using_file) {
                        if (!kh_msg_shown_load_fail) {
                                const char* source = getenv("KROSSHAIR_IMG") ?
                                        "set via env var 'KROSSHAIR_IMG'" : "at default crosshair location";
                                fprintf(stderr, "[KH] Cannot load crosshair image '%s' (%s): not a valid image file — falling back to the built-in crosshair\n",
                                        crosshair_path, source);
                                kh_msg_shown_load_fail = 1;
                        }
                        KROSSHAIR_LOG(
                            "[KROSSHAIR_ERROR] failed to load crosshair "
                            "image — falling back to built-in crosshair.\n");
                }
                free(crosshair_path);

                /* no usable file — fall back to the built-in crosshair */
                if (!kh_msg_shown_built_in) {
                        fprintf(stderr, "[KH] Using built-in crosshair. Load a different crosshair by setting env var 'KROSSHAIR_IMG' to a transparent PNG file.\n");
                        kh_msg_shown_built_in = 1;
                }
                tex_width            = default_crosshair_width;
                tex_height           = default_crosshair_height;
                image_size           = (VkDeviceSize)tex_width * tex_height * 4;

                data->descriptor_set = create_image_with_desc(
                    data, tex_width, tex_height, VK_FORMAT_R8G8B8A8_SRGB,
                    &data->crosshair_image, &data->crosshair_mem,
                    &data->crosshair_image_view);

                upload_image_data(
                    device_data, cmd_buffer, (stbi_uc*)default_crosshair_data,
                    image_size, tex_width, tex_height,
                    &data->crosshair_upload_buffer,
                    &data->crosshair_upload_buffer_mem, data->crosshair_image);
        }

        data->crosshair_tex_width = tex_width;

        if (data->anim_frame_count > 1) {
                /* for animated crosshairs, use frame dimensions for quad size,
                 * UV selects first frame from atlas */
                float uv_step = 1.0f / (float)data->anim_frame_count;
                setup_vertices_uv(data->vertices,
                                  (float)data->width, (float)data->height,
                                  (float)tex_width,
                                  (float)data->anim_frame_height, 1.0f,
                                  0.0f, uv_step);
        } else {
                setup_vertices(data->vertices,
                               (float)data->width, (float)data->height,
                               (float)tex_width, (float)tex_height, 1.0f);
        }

        data->crosshair_uploaded = 1;
}

/*
 * Keep the dynamic mask and its .cfg push constants current for this
 * swapchain: reload the mask when the file changed (or was deleted),
 * encode a fresh upload into cmd_buffer, and re-parse the .cfg when
 * its mtime changes (or it appears / is removed). Both the mask and
 * the config are optional — if neither exists, nothing happens.
 *
 * data:        swapchain to keep the mask of;
 * cmd_buffer:  command buffer the mask upload is encoded into.
 */
void ensure_swapchain_dynamic_mask(swapchain_data_t* data,
                                          VkCommandBuffer cmd_buffer)
{
        device_data_t* device_data = data->device_data;

        char* mask_path = get_dynamic_mask_path();
        int using_file = (mask_path != NULL);

        /* ── check for mask image reload ──
         * Only entered when a mask was actually uploaded (uploaded != 0); the
         * set release below is a targeted FreeDescriptorSets gated on the set
         * handle itself (pool untouched), so other swapchains' sets are not
         * invalidated (same rule as the crosshair path). */
        if (data->dynamic_mask.uploaded) {
                int needs_reload = 0;

                if (using_file && data->dynamic_mask.path) {
                        needs_reload = image_file_changed(
                            data->dynamic_mask.path, &data->dynamic_mask.mtime,
                            mask_path, "mask", 0);
                } else if (!using_file && data->dynamic_mask.path) {
                        shutdown_dynamic_mask(data);
                        data->dynamic_mask.uploaded = 0;
                        free(data->dynamic_mask.path);
                        data->dynamic_mask.path = NULL;
                        free(mask_path);
                        return;
                }

                if (!needs_reload) {
                        free(mask_path);
                        /* still check cfg for hot-reload */
                        goto check_cfg;
                }

                shutdown_dynamic_mask(data);
                data->dynamic_mask.uploaded = 0;
                free(data->dynamic_mask.path);
                data->dynamic_mask.path = NULL;
                /* free THIS swapchain's mask set (targeted, not a bulk pool reset) —
                 * other swapchains' sets (up to maxSets=4) are untouched */
                if (data->shader_mask_desc_set != VK_NULL_HANDLE) {
                        VkDescriptorSet sets[] = {data->shader_mask_desc_set};
                        device_data->vtable.FreeDescriptorSets(
                            device_data->device, device_data->shader_desc_pool,
                            1, sets);
                        data->shader_mask_desc_set = VK_NULL_HANDLE;
                }
        }

        if (!using_file) {
                free(mask_path);
                return;
        }

        int tex_width, tex_height, tex_channels;
        stbi_uc* pixels = stbi_load(mask_path, &tex_width, &tex_height,
                                    &tex_channels, STBI_rgb_alpha);
        if (!pixels) {
                KROSSHAIR_LOG("[KROSSHAIR_ERROR] failed to load dynamic mask: %s\n",
                              mask_path);
                free(mask_path);
                return;
        }

        VkDeviceSize image_size = (VkDeviceSize)tex_width * tex_height * 4;

        /* no main-pool descriptor set: the mask is drawn via the shader
         * pipeline using shader_mask_desc_set, which references the mask's
         * image view directly */
        create_image(data, VK_NULL_HANDLE, tex_width, tex_height,
                     VK_FORMAT_R8G8B8A8_SRGB, &data->dynamic_mask.image,
                     &data->dynamic_mask.mem, &data->dynamic_mask.image_view);

        upload_image_data(
            device_data, cmd_buffer, pixels, image_size, tex_width,
            tex_height, &data->dynamic_mask.upload_buffer,
            &data->dynamic_mask.upload_buffer_mem, data->dynamic_mask.image);
        stbi_image_free(pixels);

        data->dynamic_mask.path = mask_path;
        get_file_mtime(mask_path, &data->dynamic_mask.mtime);
        data->dynamic_mask.tex_width = tex_width;

        setup_vertices(data->dynamic_mask.vertices,
                       (float)data->width, (float)data->height,
                       (float)tex_width, (float)tex_height, 1.0f);

        data->dynamic_mask.uploaded = 1;
        KROSSHAIR_LOG("[KROSSHAIR] loaded dynamic mask from: %s\n", mask_path);

check_cfg:
        /* ── check for config file reload ── */
        {
                char* cfg_path = get_dynamic_cfg_path();
                int cfg_exists = (cfg_path != NULL);

                if (cfg_exists && data->dynamic_cfg_path) {
                        struct timespec new_mtime;
                        if (get_file_mtime(cfg_path, &new_mtime) == 0) {
                                if (new_mtime.tv_sec != data->dynamic_cfg_mtime.tv_sec ||
                                    new_mtime.tv_nsec != data->dynamic_cfg_mtime.tv_nsec) {
                                        parse_dynamic_cfg(cfg_path, &data->dynamic_pc);
                                        data->dynamic_cfg_mtime = new_mtime;
                                        KROSSHAIR_LOG("[KROSSHAIR] reloaded dynamic cfg: %s\n", cfg_path);
                                }
                        }
                } else if (cfg_exists) {
                        /* first time loading cfg */
                        parse_dynamic_cfg(cfg_path, &data->dynamic_pc);
                        get_file_mtime(cfg_path, &data->dynamic_cfg_mtime);
                        data->dynamic_cfg_path = cfg_path;
                        cfg_path = NULL; /* don't free, ownership transferred */
                        KROSSHAIR_LOG("[KROSSHAIR] loaded dynamic cfg: %s\n", data->dynamic_cfg_path);
                } else if (!cfg_exists && data->dynamic_cfg_path) {
                        /* cfg file was removed */
                        parse_dynamic_cfg(NULL, &data->dynamic_pc);
                        free(data->dynamic_cfg_path);
                        data->dynamic_cfg_path = NULL;
                }

                free(cfg_path);
        }
}

/*
 * Create one fence-ring slot: a dedicated cmd buffer, fence, and two
 * semaphores — one slot per swapchain image.  Slots are created once in
 * setup_swapchain_data and destroyed in destroy_swapchain_data; on a fence
 * timeout the frame is skipped instead of destroying a slot while its
 * submit is still in flight.  Each slot has its own cmd buffer (never
 * shared across slots), allocated from the device-scoped cmd_pool.
 *
 * data: swapchain the slot belongs to (also stored in
 *       data->draws[slot]);
 * slot: slot index (one slot per swapchain image).
 */
krosshair_draw_t* create_draw_slot(swapchain_data_t* data, uint32_t slot)
{
        device_data_t* device_data = data->device_data;

        krosshair_draw_t* draw = malloc(sizeof(*draw));
        memset(draw, 0, sizeof(*draw));

        VkCommandBufferAllocateInfo cmd_buffer_info = {};
        cmd_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_buffer_info.commandPool        = device_data->cmd_pool;
        cmd_buffer_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_buffer_info.commandBufferCount = 1;
        VK_CHECK(device_data->vtable.AllocateCommandBuffers(
            device_data->device, &cmd_buffer_info, &draw->cmd_buffer));
        VK_CHECK(device_data->set_device_loader_data(device_data->device,
                                                      draw->cmd_buffer));

        VkSemaphoreCreateInfo sem_info = {};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(device_data->vtable.CreateSemaphore(
            device_data->device, &sem_info, NULL, &draw->semaphore));
        VK_CHECK(device_data->vtable.CreateSemaphore(
            device_data->device, &sem_info, NULL,
            &draw->crossengine_semaphore));

        VkFenceCreateInfo fence_info = {};
        fence_info.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(device_data->vtable.CreateFence(
            device_data->device, &fence_info, NULL, &draw->fence));

        data->draws[slot] = draw;
        return draw;
}

/*
 * Tear down everything a draw ring slot owns: vertex/index buffers and
 * their memory, both semaphores, the fence, and the slot's command
 * buffer. Called from destroy_swapchain_data, which has already put
 * the device idle so no submit can still reference these.
 *
 * data: swapchain the slot belongs to (device data comes from it);
 * draw: slot to destroy; NULL is a no-op.
 */
void destroy_draw(swapchain_data_t* data, krosshair_draw_t* draw)
{
        if (!draw) return;

        device_data_t* device_data = data->device_data;

        if (draw->vertex_buffer != VK_NULL_HANDLE) {
                device_data->vtable.DestroyBuffer(device_data->device,
                                                  draw->vertex_buffer, NULL);
        }
        if (draw->vertex_buffer_mem != VK_NULL_HANDLE) {
                device_data->vtable.FreeMemory(device_data->device,
                                               draw->vertex_buffer_mem, NULL);
        }
        if (draw->vertex_buffer2 != VK_NULL_HANDLE) {
                device_data->vtable.DestroyBuffer(device_data->device,
                                                  draw->vertex_buffer2, NULL);
        }
        if (draw->vertex_buffer2_mem != VK_NULL_HANDLE) {
                device_data->vtable.FreeMemory(device_data->device,
                                               draw->vertex_buffer2_mem, NULL);
        }
        if (draw->index_buffer != VK_NULL_HANDLE) {
                device_data->vtable.DestroyBuffer(device_data->device,
                                                  draw->index_buffer, NULL);
        }
        if (draw->index_buffer_mem != VK_NULL_HANDLE) {
                device_data->vtable.FreeMemory(device_data->device,
                                               draw->index_buffer_mem, NULL);
        }
        if (draw->semaphore != VK_NULL_HANDLE) {
                device_data->vtable.DestroySemaphore(device_data->device,
                                                     draw->semaphore, NULL);
        }
        if (draw->crossengine_semaphore != VK_NULL_HANDLE) {
                device_data->vtable.DestroySemaphore(device_data->device,
                                                     draw->crossengine_semaphore, NULL);
        }
        if (draw->fence != VK_NULL_HANDLE) {
                device_data->vtable.DestroyFence(device_data->device,
                                                 draw->fence, NULL);
        }
        if (draw->cmd_buffer != VK_NULL_HANDLE) {
                device_data->vtable.FreeCommandBuffers(device_data->device,
                                                       device_data->cmd_pool,
                                                       1, &draw->cmd_buffer);
        }

        free(draw);
}

