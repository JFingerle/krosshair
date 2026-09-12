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

static void parse_dynamic_cfg(const char* path, struct dynamic_push_constants* pc)
{
        /* zero everything except quad_ndc_min/size (caller sets those) */
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
                int n = sscanf(line, "%63s %f %f %f %f", name, &a1, &a2, &a3, &a4);
                if (n < 2) continue;

                if (strcmp(name, "invert") == 0) {
                        pc->invert_str = a1;
                } else if (strcmp(name, "dodge") == 0) {
                        pc->dodge_str = a1;
                        if (n >= 5) { pc->dodge_r = a2; pc->dodge_g = a3; pc->dodge_b = a4; }
                } else if (strcmp(name, "burn") == 0) {
                        pc->burn_str = a1;
                        if (n >= 5) { pc->burn_r = a2; pc->burn_g = a3; pc->burn_b = a4; }
                } else if (strcmp(name, "complement") == 0) {
                        pc->complement_str = a1;
                } else if (strcmp(name, "lumainvert") == 0) {
                        pc->lumainvert_str = a1;
                } else if (strcmp(name, "huerotate") == 0) {
                        pc->huerotate_str = a1;
                        if (n >= 3) pc->huerotate_angle = a2;
                } else if (strcmp(name, "saturate") == 0) {
                        pc->saturate_str = a1;
                        if (n >= 3) pc->saturate_amount = a2;
                } else if (strcmp(name, "opacity") == 0) {
                        pc->opacity = a1;
                }
        }
        fclose(f);
}



/*
 * Set up the quad vertices for rendering.
 *   uv_top / uv_bottom: vertical UV range (0..1 for full texture,
 *   or a sub-range for atlas frame selection)
 */
void setup_vertices_uv(vertex_t* vertices,
                               float canvas_width, float canvas_height,
                               float tex_width, float tex_height, float scale,
                               float uv_top, float uv_bottom)
{
        float width_ndc  = ((tex_width * scale) / canvas_width);
        float height_ndc = ((tex_height * scale) / canvas_height);

        /* should fix even-length crosshairs */
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

static void setup_vertices(vertex_t* vertices,
                           float canvas_width, float canvas_height,
                           float tex_width, float tex_height, float scale)
{
        setup_vertices_uv(vertices, canvas_width, canvas_height, tex_width,
                          tex_height, scale, 0.0f, 1.0f);
}

uint16_t indices[] = {0, 1, 2, 2, 3, 0};

static char* get_crosshair_file(const char* path)
{
        if (!path) return NULL;

        if (!(path[0] == '~')) return strdup(path);

        const char* home_dir = NULL;

        if (path[1] == '/' || path[1] == '\0') {
                home_dir = getenv("HOME");
                if (!home_dir) return NULL;
        }

        const char* rest_str = path + 1;
        while (*rest_str && *rest_str != '/') rest_str++;

        char* expanded_str = malloc(strlen(home_dir) + strlen(rest_str) + 1);
        strcpy(expanded_str, home_dir);
        strcat(expanded_str, rest_str);
        return expanded_str;
}

static int get_file_mtime(const char* path, struct timespec* mtime)
{
        struct stat st;
        if (stat(path, &st) != 0) return -1;
        *mtime = st.st_mtim;
        return 0;
}

// malloc's returned string, free later
// returns crosshair-maker default if installed and no explicit KROSSHAIR_IMG set
// tries current.apng first, then current.gif, then current.png
static char* get_crosshair_path(void)
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

// malloc's returned string, free later
// returns the path to {stem}.dynamic.png, or NULL if absent
static char* get_dynamic_mask_path(void)
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

// malloc's returned string, free later
// returns the path to {stem}.dynamic.cfg, or NULL if absent
static char* get_dynamic_cfg_path(void)
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

/* ───────────────────── APNG loader ───────────────────── */


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
                int needs_reload = 0;

                if (using_file && data->crosshair_path) {
                        if (strcmp(data->crosshair_path, crosshair_path) != 0) {
                                KROSSHAIR_LOG("[KROSSHAIR] path changed, reloading\n");
                                needs_reload = 1;
                        } else {
                                struct timespec new_mtime;
                                if (get_file_mtime(crosshair_path, &new_mtime) == 0) {
                                        if (new_mtime.tv_sec != data->crosshair_mtime.tv_sec ||
                                            new_mtime.tv_nsec != data->crosshair_mtime.tv_nsec) {
                                                KROSSHAIR_LOG("[KROSSHAIR] mtime changed (%ld.%ld -> %ld.%ld), reloading\n",
                                                       data->crosshair_mtime.tv_sec,
                                                       data->crosshair_mtime.tv_nsec,
                                                       new_mtime.tv_sec,
                                                       new_mtime.tv_nsec);
                                                needs_reload = 1;
                                        }
                                }
                        }
                }

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
        int tex_channels;
        VkDeviceSize image_size;
        stbi_uc* pixels;

        if (using_file) {
                const char* ext = strrchr(crosshair_path, '.');
                int is_gif = ext && (strcasecmp(ext, ".gif") == 0);
                int is_apng = ext && (strcasecmp(ext, ".apng") == 0);

                /* .png files might also be APNG -- detect by checking for
                 * acTL chunk if the extension is .png */
                int is_png = ext && (strcasecmp(ext, ".png") == 0);

                if (is_gif) {
                        FILE* f = fopen(crosshair_path, "rb");
                        if (!f) {
                                KROSSHAIR_LOG("[KROSSHAIR_ERROR] failed to open GIF: %s\n",
                                              crosshair_path);
                                free(crosshair_path);
                                return;
                        }

                        fseek(f, 0, SEEK_END);
                        long file_len = ftell(f);
                        fseek(f, 0, SEEK_SET);

                        if (file_len <= 0 || file_len > (long)INT_MAX) {
                                KROSSHAIR_LOG("[KROSSHAIR_ERROR] invalid GIF file size: %ld\n",
                                              file_len);
                                fclose(f);
                                free(crosshair_path);
                                return;
                        }

                        unsigned char* file_buf = malloc((size_t)file_len);
                        if (!file_buf) {
                                KROSSHAIR_LOG("[KROSSHAIR_ERROR] failed to allocate GIF read buffer\n");
                                fclose(f);
                                free(crosshair_path);
                                return;
                        }

                        size_t bytes_read = fread(file_buf, 1, (size_t)file_len, f);
                        fclose(f);

                        if ((long)bytes_read != file_len) {
                                KROSSHAIR_LOG("[KROSSHAIR_ERROR] short read on GIF: %zu/%ld\n",
                                              bytes_read, file_len);
                                free(file_buf);
                                free(crosshair_path);
                                return;
                        }

                        int* delays = NULL;
                        int frames = 0;
                        int gif_len = (int)file_len;
                        stbi_uc* gif_data = stbi_load_gif_from_memory(
                            file_buf, gif_len, &delays, &tex_width, &tex_height,
                            &frames, &tex_channels, STBI_rgb_alpha);
                        free(file_buf);

                        if (!gif_data || frames < 1) {
                                KROSSHAIR_LOG("[KROSSHAIR_ERROR] failed to decode GIF: %s (%s)\n",
                                              crosshair_path,
                                              gif_data ? "no frames" : "decode error");
                                if (gif_data) stbi_image_free(gif_data);
                                if (delays) free(delays);
                                free(crosshair_path);
                                return;
                        }

                        /*
                         * Build a vertical texture atlas: all frames stacked
                         * top-to-bottom in a single image.  The UV coordinates
                         * are adjusted per-frame to select the right slice.
                         *
                         * gif_data from stbi is already laid out as
                         * [frame0][frame1]...[frameN] contiguously, each
                         * frame being (tex_width * tex_height * 4) bytes,
                         * which is exactly the atlas layout we need.
                         */
                        int frame_height = tex_height;
                        int atlas_height = tex_height * frames;
                        image_size = (VkDeviceSize)tex_width * atlas_height * 4;
                        pixels = gif_data;
                        /* tex_height now refers to the full atlas */
                        tex_height = atlas_height;

                        /* store animation state */
                        data->anim_frame_count  = frames;
                        data->anim_frame_height = frame_height;
                        data->anim_current_frame = 0;
                        clock_gettime(CLOCK_MONOTONIC, &data->anim_last_frame_time);

                        if (delays) {
                                data->anim_delays = malloc(sizeof(int) * frames);
                                for (int gi = 0; gi < frames; gi++) {
                                        /* stbi already converts GIF centisecond
                                         * delays to milliseconds internally
                                         * (10 * cs).  delay 0 means "as fast
                                         * as possible", default to ~100ms */
                                        data->anim_delays[gi] =
                                            delays[gi] > 0 ? delays[gi] : 100;
                                }
                                free(delays);
                        } else {
                                data->anim_delays = malloc(sizeof(int) * frames);
                                for (int gi = 0; gi < frames; gi++)
                                        data->anim_delays[gi] = 100;
                        }

                        KROSSHAIR_LOG("[KROSSHAIR] loaded GIF atlas: %dx%d (%d frames, frame_h=%d)\n",
                                      tex_width, atlas_height, frames, frame_height);
                } else if (is_apng || is_png) {
                        /* try to load as APNG; if it's a plain PNG the
                         * loader will return NULL (no acTL) and we fall
                         * through to stbi_load below */
                        FILE* f = fopen(crosshair_path, "rb");
                        if (!f) {
                                int err = errno;
                                if (!kh_msg_shown_load_fail) {
                                        const char* source = getenv("KROSSHAIR_IMG") ?
                                                "set via env var 'KROSSHAIR_IMG'" : "at default crosshair location";
                                        fprintf(stderr, "[KH] Cannot load crosshair image '%s' (%s): %s — falling back to the built-in crosshair\n",
                                                crosshair_path, source, strerror(err));
                                        kh_msg_shown_load_fail = 1;
                                }
                                KROSSHAIR_LOG("[KROSSHAIR_ERROR] failed to open: %s — falling back to built-in crosshair\n",
                                              crosshair_path);
                                free(crosshair_path);
                                crosshair_path = NULL;
                                goto fallback_to_built_in;
                        }

                        fseek(f, 0, SEEK_END);
                        long file_len = ftell(f);
                        fseek(f, 0, SEEK_SET);

                        unsigned char* file_buf = NULL;
                        if (file_len > 0 && file_len <= (long)INT_MAX) {
                                file_buf = malloc((size_t)file_len);
                                if (file_buf) {
                                        size_t bytes_read = fread(file_buf, 1, (size_t)file_len, f);
                                        if ((long)bytes_read != file_len) {
                                                free(file_buf);
                                                file_buf = NULL;
                                        }
                                }
                        }
                        fclose(f);

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
                                tex_width = apng_w;
                                int frame_height = apng_h;
                                int atlas_height = apng_h * apng_frames;
                                tex_height = atlas_height;
                                image_size = (VkDeviceSize)tex_width * atlas_height * 4;
                                pixels = (stbi_uc*)apng_data;

                                data->anim_frame_count   = apng_frames;
                                data->anim_frame_height  = frame_height;
                                data->anim_current_frame = 0;
                                clock_gettime(CLOCK_MONOTONIC, &data->anim_last_frame_time);

                                data->anim_delays = apng_delays;

                                KROSSHAIR_LOG("[KROSSHAIR] loaded APNG atlas: %dx%d (%d frames, frame_h=%d)\n",
                                              tex_width, atlas_height, apng_frames, frame_height);
                        } else {
                                /* not animated APNG (or single frame) --
                                 * fall back to regular stbi_load for
                                 * proper PNG handling */
                                if (apng_data) free(apng_data);
                                if (apng_delays) free(apng_delays);

                                pixels = stbi_load(crosshair_path, &tex_width, &tex_height,
                                                   &tex_channels, STBI_rgb_alpha);
                                image_size = tex_width * tex_height * 4;
                        }
                } else {
                        pixels = stbi_load(crosshair_path, &tex_width, &tex_height,
                                           &tex_channels, STBI_rgb_alpha);
                        image_size = tex_width * tex_height * 4;
                }

                if (!pixels) {
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
                        free(crosshair_path);
                        crosshair_path = NULL;
                        goto fallback_to_built_in;
                }

                data->descriptor_set = create_image_with_desc(
                    data, tex_width, tex_height, VK_FORMAT_R8G8B8A8_SRGB,
                    &data->crosshair_image, &data->crosshair_mem,
                    &data->crosshair_image_view);

                upload_image_data(
                    device_data, cmd_buffer, pixels, image_size, tex_width,
                    tex_height, &data->crosshair_upload_buffer,
                    &data->crosshair_upload_buffer_mem, data->crosshair_image);
                stbi_image_free(pixels);

                data->crosshair_path = crosshair_path;
                get_file_mtime(crosshair_path, &data->crosshair_mtime);
                KROSSHAIR_LOG("[KROSSHAIR] loaded crosshair from: %s (mtime %ld.%ld)\n",
                              crosshair_path,
                              data->crosshair_mtime.tv_sec,
                              data->crosshair_mtime.tv_nsec);
                if (!kh_msg_shown_file_load) {
                        const char* reason = getenv("KROSSHAIR_IMG") ?
                                "Set via env var 'KROSSHAIR_IMG'" :
                                "Default crosshair location";
                        fprintf(stderr, "[KH] Loading crosshair from file '%s'. Reason: %s\n",
                                crosshair_path, reason);
                        kh_msg_shown_file_load = 1;
                }
        } else {
                fallback_to_built_in:
                if (!kh_msg_shown_built_in) {
                        fprintf(stderr, "[KH] Using built-in crosshair. Load a different crosshair by setting env var 'KROSSHAIR_IMG' to a transparent PNG file.\n");
                        kh_msg_shown_built_in = 1;
                }
                free(crosshair_path);
                tex_width            = default_crosshair_width;
                tex_height           = default_crosshair_height;
                image_size           = tex_width * tex_height * 4;

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
 * Load the single dynamic mask + parse the config file.
 * Both are optional — if no mask file exists, nothing happens.
 */
void ensure_swapchain_dynamic_mask(swapchain_data_t* data,
                                          VkCommandBuffer cmd_buffer)
{
        device_data_t* device_data = data->device_data;

        char* mpath = get_dynamic_mask_path();
        int using_file = (mpath != NULL);

        /* ── check for mask image reload ──
         * Only entered when a mask was actually uploaded (uploaded != 0); the
         * set release below is a targeted FreeDescriptorSets gated on the set
         * handle itself (pool untouched), so other swapchains' sets are not
         * invalidated (same rule as the crosshair path). */
        if (data->dynamic_mask.uploaded) {
                int needs_reload = 0;

                if (using_file && data->dynamic_mask.path) {
                        if (strcmp(data->dynamic_mask.path, mpath) != 0) {
                                needs_reload = 1;
                        } else {
                                struct timespec new_mtime;
                                if (get_file_mtime(mpath, &new_mtime) == 0) {
                                        if (new_mtime.tv_sec != data->dynamic_mask.mtime.tv_sec ||
                                            new_mtime.tv_nsec != data->dynamic_mask.mtime.tv_nsec) {
                                                needs_reload = 1;
                                        }
                                }
                        }
                } else if (!using_file && data->dynamic_mask.path) {
                        shutdown_dynamic_mask(data);
                        data->dynamic_mask.uploaded = 0;
                        free(data->dynamic_mask.path);
                        data->dynamic_mask.path = NULL;
                        free(mpath);
                        return;
                }

                if (!needs_reload) {
                        free(mpath);
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
                free(mpath);
                return;
        }

        int tex_width, tex_height, tex_channels;
        stbi_uc* pixels = stbi_load(mpath, &tex_width, &tex_height,
                                    &tex_channels, STBI_rgb_alpha);
        if (!pixels) {
                KROSSHAIR_LOG("[KROSSHAIR_ERROR] failed to load dynamic mask: %s\n",
                              mpath);
                free(mpath);
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

        data->dynamic_mask.path = mpath;
        get_file_mtime(mpath, &data->dynamic_mask.mtime);
        data->dynamic_mask.tex_width = tex_width;

        setup_vertices(data->dynamic_mask.vertices,
                       (float)data->width, (float)data->height,
                       (float)tex_width, (float)tex_height, 1.0f);

        data->dynamic_mask.uploaded = 1;
        KROSSHAIR_LOG("[KROSSHAIR] loaded dynamic mask from: %s\n", mpath);

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

