#define _GNU_SOURCE
#pragma once

#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include "dispatch.h"

#define K_NO_MEMORYTYPE 0xFFFFFFFF

#define vk_foreach_struct(__iter, __start)              \
        for (struct VkBaseOutStructure* __iter =        \
                 (struct VkBaseOutStructure*)(__start); \
             __iter; __iter = __iter->pNext)

#define vk_foreach_struct_const(__iter, __start)             \
        for (const struct VkBaseInStructure* __iter =        \
                 (const struct VkBaseInStructure*)(__start); \
             __iter; __iter = __iter->pNext)
#ifdef NDEBUG
#define KROSSHAIR_LOG(fmt, ...) ((void)0)
#else
#define KROSSHAIR_LOG(fmt, ...) \
        do { \
                FILE* _kf = fopen("/tmp/krosshair.log", "a"); \
                if (_kf) { \
                        fprintf(_kf, fmt, ##__VA_ARGS__); \
                        fclose(_kf); \
                } \
        } while (0)
#endif

#define VK_CHECK(expr)                        \
        do {                                  \
                VkResult __result = (expr);   \
                if (__result != VK_SUCCESS) { \
                        KROSSHAIR_LOG("[KROSSHAIR_ERROR] VK_CHECK failed: %d\n", __result); \
                }                             \
        } while (0)

typedef struct vec2 {
        float x;
        float y;
} vec2_t;

typedef struct vec3 {
        float x;
        float y;
        float z;
} vec3_t;

typedef struct vertex {
        vec2_t pos;
        vec2_t tex_pos;
} vertex_t;

typedef struct vk_object {
        uint64_t obj;
        void* data;
        const char* name;  // only really for debugging purposes
} vk_object_t;

#define MAX_VK_OBJECTS 512
typedef struct vk_object_map {
        vk_object_t data[MAX_VK_OBJECTS];
        size_t count;
} vk_object_map_t;

typedef struct instance_data {
        instance_dispatch_table_t vtable;
        VkInstance instance;
        uint32_t api_version;
        /* next-link destroy: the loader resolves vkDestroyInstance directly
         * (not via GetInstanceProcAddr), so vtable.DestroyInstance is commonly
         * NULL — capture the real chain pointer at CreateInstance */
        PFN_vkDestroyInstance chain_DestroyInstance;
} instance_data_t;

typedef struct queue_data {
        struct device_data* device;
        VkQueue queue;
        VkQueueFlags flags;
        uint32_t family_index;
} queue_data_t;

#define KROSSHAIR_MAX_SWAPCHAINS 4 /* must match descriptor pool maxSets */
typedef struct device_data {
        device_dispatch_table_t vtable;
        instance_data_t* instance;
        PFN_vkSetDeviceLoaderData set_device_loader_data;
        /* next-link destroy: the loader resolves vkDestroyDevice directly
         * (not via GetDeviceProcAddr), so vtable.DestroyDevice is commonly
         * NULL — capture the real chain pointer at CreateDevice */
        PFN_vkDestroyDevice chain_DestroyDevice;
        VkPhysicalDevice physical_device;
        VkDevice device;
        VkPhysicalDeviceProperties properties;
        struct queue_data* graphic_queue;
        struct queue_data* queues[16];  // 16 should be enough?
        uint32_t queue_count;
        uint32_t swapchain_count; /* live swapchains on this device */
        void* swapchains[KROSSHAIR_MAX_SWAPCHAINS]; /* live swapchains (one per
                                                 descriptor-set slot), cast to
                                                 swapchain_data_t* */

        /* stable GPU resources: created once per device, shared by all
         * swapchains of that device, destroyed only in overlay_DestroyDevice.
         * The descriptor pools are sized maxSets=KROSSHAIR_MAX_SWAPCHAINS with
         * descriptorCount scaled to match (pool total = maxSets × per-set
         * bindings). All set releases — reload and teardown — use targeted
         * FreeDescriptorSets, so they never invalidate other swapchains'
         * sets. */
        VkSampler crosshair_sampler;
        VkDescriptorSetLayout descriptor_layout;  /* 1 binding, immutable sampler */
        VkDescriptorPool descriptor_pool;         /* crosshair descriptor sets */
        VkCommandPool cmd_pool;
        VkPipelineLayout pipeline_layout;

        /* format-scoped: the pipeline is bound to the render pass, so all
         * three are (re)created together, only when the app image format
         * changes (VK_FORMAT_UNDEFINED = "not created yet") */
        VkFormat render_pass_format;
        VkRenderPass render_pass;
        VkPipeline pipeline;

        /* shader-based dynamic effect pipeline (format-scoped as well) */
        VkDescriptorSetLayout shader_desc_layout;
        VkDescriptorPool shader_desc_pool;
        VkPipelineLayout shader_pipeline_layout;
        VkPipeline shader_pipeline;
} device_data_t;

typedef struct command_buffer_data {
        device_data_t* device_data;
        VkCommandBufferLevel level;
        VkCommandBuffer cmd_buffer;
        queue_data_t* queue_data;
} cmd_buffer_data_t;

typedef struct krosshair_draw {
        VkCommandBuffer cmd_buffer;

        VkSemaphore crossengine_semaphore;
        VkSemaphore semaphore;
        VkFence fence;

        VkBuffer vertex_buffer;
        VkDeviceMemory vertex_buffer_mem;
        VkDeviceSize vertex_buffer_size;

        VkBuffer vertex_buffer2;       /* separate vertex buffer for dynamic mask */
        VkDeviceMemory vertex_buffer2_mem;
        VkDeviceSize vertex_buffer2_size;

        VkBuffer index_buffer;
        VkDeviceMemory index_buffer_mem;
        VkDeviceSize index_buffer_size;

        int vertex_buffer_initialized;
        int index_buffer_initialized;

        /* 1 while a submit with this slot's fence is in flight (or was
         * signaled and not yet reset) — the slot must not be reused until
         * the fence has been waited on and reset */
        int fence_submitted;
} krosshair_draw_t;

struct dynamic_push_constants {
        float quad_ndc_min[2];    /* 8 bytes  */
        float quad_ndc_size[2];   /* 8 bytes  */
        float invert_str;         /* 4        */
        float dodge_str;          /* 4        */
        float dodge_r, dodge_g, dodge_b; /* 12 */
        float burn_str;           /* 4        */
        float burn_r, burn_g, burn_b;    /* 12 */
        float complement_str;     /* 4        */
        float lumainvert_str;     /* 4        */
        float huerotate_str;      /* 4        */
        float huerotate_angle;    /* 4        */
        float saturate_str;       /* 4        */
        float saturate_amount;    /* 4        */
        float opacity;            /* 4        */
};

typedef struct swapchain_data {
        device_data_t* device_data;

        VkSwapchainKHR swapchain;
        uint32_t width, height;
        VkFormat format;

        uint32_t n_images;

        VkImage images[16];
        VkImageView image_views[16];
        VkFramebuffer framebuffers[16];

        /* crosshair descriptor set (allocated from the device-scoped
         * descriptor_pool; one live set per swapchain) */
        VkDescriptorSet descriptor_set;

        int crosshair_uploaded;
        VkImage crosshair_image;
        VkImageView crosshair_image_view;
        VkDeviceMemory crosshair_mem;
        VkBuffer crosshair_upload_buffer;
        VkDeviceMemory crosshair_upload_buffer_mem;

        char* crosshair_path;
        struct timespec crosshair_mtime;

        /* crosshair texture dimensions (for vertex setup) */
        int crosshair_tex_width;

        /* per-swapchain vertex data (avoids global shared across swapchains) */
        vertex_t vertices[4];

        /* animation state (GIF / APNG) */
        int anim_frame_count;
        int anim_frame_height;       /* height of a single frame */
        int* anim_delays;            /* delay per frame in ms */
        int anim_current_frame;
        struct timespec anim_last_frame_time;

        /* per-image fence ring: one draw object per swapchain image, so a
         * slot's fence/semaphores/cmd buffer are always idle by the time the
         * slot is reused (vulkan-tutorial pattern) */
        krosshair_draw_t* draws[16];

        /* ── single dynamic effect mask (optional) ── */
        struct {
                int uploaded;
                VkImage image;
                VkImageView image_view;
                VkDeviceMemory mem;
                VkBuffer upload_buffer;
                VkDeviceMemory upload_buffer_mem;
                char* path;
                struct timespec mtime;
                int tex_width;
                vertex_t vertices[4];
        } dynamic_mask;
        char* dynamic_cfg_path;
        struct timespec dynamic_cfg_mtime;
        struct dynamic_push_constants dynamic_pc;

        /* game framebuffer copy (for shader-based dynamic effects) */
        VkImage game_fb_image;
        VkImageView game_fb_image_view;
        VkDeviceMemory game_fb_mem;
        uint32_t game_fb_width, game_fb_height;

        /* descriptor set for dynamic mask + game_fb (allocated from the
         * device-scoped shader_desc_pool) */
        VkDescriptorSet shader_mask_desc_set;

} swapchain_data_t;

#define HKEY(obj)           ((uint64_t)(obj))
#define FIND_OBJ(type, obj) ((type*)(find_object_data(HKEY(obj))))

extern volatile int crosshair_visible;
extern pthread_mutex_t global_lock;
extern vk_object_map_t vk_obj_map;
extern uint16_t indices[6];

int vk_map_set(vk_object_map_t* map, uint64_t obj, void* data, const char* name);
int vk_map_get(vk_object_map_t* map, uint64_t obj, vk_object_t* obj_out);
void vk_map_print(vk_object_map_t* map);
void vk_map_delete(vk_object_map_t* map, uint64_t obj);
void init_input_thread(void);
void setup_vertices_uv(vertex_t* vertices, float canvas_width, float canvas_height, float tex_width, float tex_height, float scale, float uv_top, float uv_bottom);
uint32_t vk_memory_type(device_data_t* device_data, VkMemoryPropertyFlags properties, uint32_t type_bits);
void* find_object_data(uint64_t obj);
void map_object(uint64_t obj, void* data, const char* name);
void unmap_object(uint64_t obj);
VkLayerInstanceCreateInfo* get_instance_chain_info( const VkInstanceCreateInfo* p_create_info, VkLayerFunction func);
instance_data_t* new_instance_data(VkInstance instance);
device_data_t* new_device_data(VkDevice device, instance_data_t* instance);
cmd_buffer_data_t* new_cmd_buffer_data(VkCommandBuffer cmd_buffer, VkCommandBufferLevel level, device_data_t* device_data);
void create_or_resize_buffer(device_data_t* device_data, VkBuffer* buffer, VkDeviceMemory* buffer_mem, VkDeviceSize* buffer_size, size_t new_size, VkBufferUsageFlagBits usage);
void destroy_swapchain_data(swapchain_data_t* data);
void create_image(swapchain_data_t* data, VkDescriptorSet descriptor_set, uint32_t width, uint32_t height, VkFormat format, VkImage* image, VkDeviceMemory* image_mem, VkImageView* image_view);
VkDescriptorSet create_image_with_desc(swapchain_data_t* data, uint32_t width, uint32_t height, VkFormat format, VkImage* image, VkDeviceMemory* image_mem, VkImageView* image_view);
void upload_image_data(device_data_t* device_data, VkCommandBuffer cmd_buffer, void* pixels, VkDeviceSize upload_size, uint32_t width, uint32_t height, VkBuffer* upload_buffer, VkDeviceMemory* upload_buffer_mem, VkImage image);
unsigned char* load_apng(const unsigned char* file_data, size_t file_len, int* out_width, int* out_height, int* out_frames, int** out_delays);
void ensure_swapchain_crosshair(swapchain_data_t* data, VkCommandBuffer cmd_buffer);
void ensure_swapchain_dynamic_mask(swapchain_data_t* data, VkCommandBuffer cmd_buffer);
krosshair_draw_t* create_draw_slot(swapchain_data_t* data, uint32_t slot);
void destroy_draw(swapchain_data_t* data, krosshair_draw_t* draw);
krosshair_draw_t* render_swapchain_display( swapchain_data_t* data, queue_data_t* present_queue, const VkSemaphore* wait_semaphores, unsigned n_wait_semaphores, unsigned image_index);
void create_device_stable_resources(device_data_t* device_data);
void setup_swapchain_data(swapchain_data_t* data, const VkSwapchainCreateInfoKHR* pCreateInfo);
void device_map_queues(device_data_t* data, const VkDeviceCreateInfo* pCreateInfo);
void shutdown_krosshair_image(swapchain_data_t* data);
void shutdown_dynamic_mask(swapchain_data_t* data);
