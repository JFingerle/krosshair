/*
 * Unit tests for the overlay render path in src/render.c.
 *
 * wait_draw_slot_ready, ensure_game_fb_copy, record_framebuffer_copy,
 * transition_swapchain_for_render, advance_anim_frame,
 * upload_device_memory, ensure_quad_buffers, record_dynamic_mask_draw
 * and submit_overlay_draw are exposed via the RENDER_API macro in
 * UNIT_TEST builds (see tests/test_render_api.h); render_swapchain_display
 * is already exported and drives all of them.
 *
 * The Vulkan surface is a recording stub vtable: every call returns
 * success and the tests assert on what the render path decided to do
 * (barriers, buffer/image sizes, upload payloads, submit topology).
 * No real driver is involved, so this suite validates the layer's logic.
 *
 * A deterministic built-in crosshair (50x50) is used: HOME is pointed
 * at an empty directory so no crosshair image file can ever be found.
 */
#define _GNU_SOURCE

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "../include/krosshair.h"
#include "test_render_api.h"

/*
 * default_crosshair.h *defines* these at file scope (external linkage);
 * including it here would duplicate the definitions already compiled from
 * src/. Declare them extern instead.
 */
extern const uint32_t default_crosshair_width;
extern const uint32_t default_crosshair_height;
extern const uint8_t default_crosshair_data[];
extern const size_t default_crosshair_size;

/* The shared upload memory the stub MapMemory hands out. */
static unsigned char mem_pool[65536];

typedef struct {
    VkQueue queue;
    VkFence fence;
    uint32_t wait_count;
    VkSemaphore wait[4];
    uint32_t signal_count;
    VkSemaphore signal[4];
    uint32_t cmd_count;
    VkCommandBuffer cmd;
} submit_rec_t;

/* Everything the stub vtable records about one test run. */
typedef struct {
    uint64_t handle_seq;

    /* injectable results */
    VkResult wait_fences_result;
    VkResult submit_result;
    VkResult end_cmd_result;

    /* physical device memory (two types: host-visible, device-local) */
    VkPhysicalDeviceMemoryProperties mem_props;

    /* device memory */
    int n_alloc_memory;
    VkDeviceSize last_alloc_size;
    int n_free_memory;
    int n_bind_buffer_memory;
    int n_bind_image_memory;
    int n_map_memory;
    VkDeviceSize last_map_size;
    int n_flush;
    int n_unmap;

    /* buffers (request order) */
    int n_create_buffer;
    VkBufferUsageFlags created_usage[16];
    VkDeviceSize created_size[16];
    int n_destroy_buffer;

    /* images (creation order) */
    int n_create_image;
    VkExtent2D created_extent[16];
    int n_destroy_image;
    int n_create_image_view;
    int n_destroy_image_view;

    /* descriptor sets */
    int n_alloc_desc_sets;
    int n_update_desc_sets;
    int n_free_desc_sets;

    /* command buffers, fences, semaphores */
    int n_reset_cmd;
    int n_begin_cmd;
    int n_end_cmd;
    int n_allocate_cmd;
    int n_free_cmd;
    int n_create_fence;
    int n_destroy_fence;
    int n_wait_fences;
    VkFence waited_fences[8];
    uint64_t wait_timeouts[8];
    int n_reset_fences;
    int n_create_sem;
    int n_destroy_sem;

    /* render pass + draw commands */
    int n_begin_rp;
    int n_end_rp;
    int n_bind_pipeline;
    VkPipeline bound_pipeline;
    int n_bind_vbufs;
    VkBuffer bound_vbuf;
    int n_bind_ibuf;
    VkBuffer bound_ibuf;
    VkIndexType bound_idx_type;
    int n_set_viewport;
    VkViewport viewport;
    int n_set_scissor;
    VkRect2D scissor;
    int n_bind_desc_sets;
    int n_push_constants;
    size_t push_size;
    const void* push_ptr;
    int n_draw_indexed;
    uint32_t draw_index_count;

    /* image barriers, in order */
    int n_barriers;
    VkImageMemoryBarrier barriers[64];
    VkPipelineStageFlags barrier_src[64];
    VkPipelineStageFlags barrier_dst[64];

    /* copies */
    int n_copy_image;
    VkImage copy_image_src;
    VkImage copy_image_dst;
    VkImageCopy copy_image_region;
    int n_copy_buf2img;
    VkImage copy_buf2img_image;
    VkBufferImageCopy copy_buf2img_region;

    /* submits, ring of 4 */
    int n_submit;
    submit_rec_t submits[4];

    /* set_device_loader_data */
    int set_loader_data_calls;
    void* set_loader_data_obj;
} render_stub_t;

static render_stub_t g_stub;
static device_data_t g_device;
static instance_data_t g_instance;
static queue_data_t g_graphics_queue;
static queue_data_t g_present_queue;
static swapchain_data_t g_swap;
/* backing storage for the four fence-ring slots (swapchain_data.draws
 * is an array of pointers) */
static krosshair_draw_t g_draws[4];

/* The next fake handle handed out by the stub. */
static void* next_handle(void)
{
    g_stub.handle_seq += 16;
    return (void*)(uintptr_t)g_stub.handle_seq;
}

/*
 * The stub vtable below: one function per device entry point the render
 * path can reach. Each records its call and returns the (default
 * successful) result; injectable failures live in g_stub.
 */

static VkResult stub_set_loader_data(VkDevice device, void* object)
{
    (void)device;
    g_stub.set_loader_data_calls++;
    g_stub.set_loader_data_obj = object;
    return VK_SUCCESS;
}

static VkResult stub_wait_fences(VkDevice device, uint32_t n,
                                  const VkFence* fences, VkBool32 waitAll,
                                  uint64_t timeout)
{
    (void)device;
    (void)waitAll;
    int i = g_stub.n_wait_fences < 8 ? g_stub.n_wait_fences : 7;
    if (n > 0)
        g_stub.waited_fences[i] = fences[0];
    g_stub.wait_timeouts[i] = timeout;
    g_stub.n_wait_fences++;
    return g_stub.wait_fences_result;
}

static VkResult stub_reset_fences(VkDevice device, uint32_t n,
                                  const VkFence* fences)
{
    (void)device;
    (void)n;
    (void)fences;
    g_stub.n_reset_fences++;
    return VK_SUCCESS;
}

static VkResult stub_allocate_cmd_buffers(VkDevice device,
                                          const VkCommandBufferAllocateInfo* info,
                                          VkCommandBuffer* pCommandBuffers)
{
    (void)device;
    (void)info;
    g_stub.n_allocate_cmd++;
    if (pCommandBuffers)
        *pCommandBuffers = (VkCommandBuffer)next_handle();
    return VK_SUCCESS;
}

static void stub_free_cmd_buffers(VkDevice device, VkCommandPool pool,
                                   uint32_t n, const VkCommandBuffer* buffers)
{
    (void)device;
    (void)pool;
    (void)n;
    (void)buffers;
    g_stub.n_free_cmd++;
}

static VkResult stub_create_semaphore(VkDevice device,
                                      const VkSemaphoreCreateInfo* info,
                                      const VkAllocationCallbacks* pAllocator,
                                      VkSemaphore* pSemaphore)
{
    (void)device;
    (void)info;
    (void)pAllocator;
    g_stub.n_create_sem++;
    if (pSemaphore)
        *pSemaphore = (VkSemaphore)next_handle();
    return VK_SUCCESS;
}

static void stub_destroy_semaphore(VkDevice device, VkSemaphore semaphore,
                                   const VkAllocationCallbacks* pAllocator)
{
    (void)device;
    (void)semaphore;
    (void)pAllocator;
    g_stub.n_destroy_sem++;
}

static VkResult stub_create_fence(VkDevice device, const VkFenceCreateInfo* info,
                                  const VkAllocationCallbacks* pAllocator,
                                  VkFence* pFence)
{
    (void)device;
    (void)info;
    (void)pAllocator;
    g_stub.n_create_fence++;
    if (pFence)
        *pFence = (VkFence)next_handle();
    return VK_SUCCESS;
}

static void stub_destroy_fence(VkDevice device, VkFence fence,
                               const VkAllocationCallbacks* pAllocator)
{
    (void)device;
    (void)fence;
    (void)pAllocator;
    g_stub.n_destroy_fence++;
}

static VkResult stub_reset_cmd_buffer(VkCommandBuffer buffer,
                                      VkCommandBufferResetFlags flags)
{
    (void)buffer;
    (void)flags;
    g_stub.n_reset_cmd++;
    return VK_SUCCESS;
}

static VkResult stub_begin_cmd_buffer(VkCommandBuffer buffer,
                                      const VkCommandBufferBeginInfo* info)
{
    (void)buffer;
    (void)info;
    g_stub.n_begin_cmd++;
    return VK_SUCCESS;
}

static VkResult stub_end_cmd_buffer(VkCommandBuffer buffer)
{
    (void)buffer;
    g_stub.n_end_cmd++;
    return g_stub.end_cmd_result;
}

static void stub_cmd_begin_render_pass(VkCommandBuffer buffer,
                                       const VkRenderPassBeginInfo* info,
                                       VkSubpassContents contents)
{
    (void)buffer;
    (void)info;
    (void)contents;
    g_stub.n_begin_rp++;
}

static void stub_cmd_end_render_pass(VkCommandBuffer buffer)
{
    (void)buffer;
    g_stub.n_end_rp++;
}

static void stub_cmd_bind_pipeline(VkCommandBuffer buffer,
                                   VkPipelineBindPoint bind_point,
                                   VkPipeline pipeline)
{
    (void)buffer;
    (void)bind_point;
    g_stub.n_bind_pipeline++;
    g_stub.bound_pipeline = pipeline;
}

static void stub_cmd_bind_vertex_buffers(VkCommandBuffer buffer,
                                         uint32_t first_binding, uint32_t n,
                                         const VkBuffer* buffers,
                                         const VkDeviceSize* offsets)
{
    (void)buffer;
    (void)first_binding;
    (void)offsets;
    g_stub.n_bind_vbufs++;
    if (n > 0)
        g_stub.bound_vbuf = buffers[0];
}

static void stub_cmd_bind_index_buffer(VkCommandBuffer buffer, VkBuffer buffer2,
                                       VkDeviceSize offset, VkIndexType index_type)
{
    (void)buffer;
    (void)offset;
    g_stub.n_bind_ibuf++;
    g_stub.bound_ibuf = buffer2;
    g_stub.bound_idx_type = index_type;
}

static void stub_cmd_set_viewport(VkCommandBuffer buffer, uint32_t first,
                                  uint32_t n, const VkViewport* viewports)
{
    (void)buffer;
    (void)first;
    g_stub.n_set_viewport++;
    if (n > 0)
        g_stub.viewport = viewports[0];
}

static void stub_cmd_set_scissor(VkCommandBuffer buffer, uint32_t first,
                                 uint32_t n, const VkRect2D* scissors)
{
    (void)buffer;
    (void)first;
    g_stub.n_set_scissor++;
    if (n > 0)
        g_stub.scissor = scissors[0];
}

static void stub_cmd_bind_descriptor_sets(VkCommandBuffer buffer,
                                          VkPipelineBindPoint bind_point,
                                          VkPipelineLayout layout,
                                          uint32_t first_set, uint32_t n,
                                          const VkDescriptorSet* sets,
                                          uint32_t n_dynamic,
                                          const uint32_t* offsets)
{
    (void)buffer;
    (void)bind_point;
    (void)layout;
    (void)first_set;
    (void)sets;
    (void)n_dynamic;
    (void)offsets;
    g_stub.n_bind_desc_sets++;
}

static void stub_cmd_push_constants(VkCommandBuffer buffer,
                                    VkPipelineLayout layout,
                                    VkShaderStageFlags stage_flags,
                                    uint32_t offset, uint32_t size,
                                    const void* values)
{
    (void)buffer;
    (void)layout;
    (void)stage_flags;
    (void)offset;
    g_stub.n_push_constants++;
    g_stub.push_size = size;
    g_stub.push_ptr = values;
}

static void stub_cmd_draw_indexed(VkCommandBuffer buffer,
                                  uint32_t index_count, uint32_t instance_count,
                                  uint32_t first_index, int32_t vertex_offset,
                                  uint32_t first_instance)
{
    (void)buffer;
    (void)instance_count;
    (void)first_index;
    (void)vertex_offset;
    (void)first_instance;
    g_stub.n_draw_indexed++;
    g_stub.draw_index_count = index_count;
}

static void stub_cmd_pipeline_barrier(VkCommandBuffer buffer,
                                      VkPipelineStageFlags src_stage,
                                      VkPipelineStageFlags dst_stage,
                                      VkDependencyFlags dep, uint32_t n_mem,
                                      const VkMemoryBarrier* mem_barriers,
                                      uint32_t n_buffers,
                                      const VkBufferMemoryBarrier* buffers,
                                      uint32_t n_images,
                                      const VkImageMemoryBarrier* images)
{
    (void)buffer;
    (void)dep;
    (void)n_mem;
    (void)mem_barriers;
    (void)n_buffers;
    (void)buffers;
    for (uint32_t i = 0; i < n_images && g_stub.n_barriers < 64; i++) {
        g_stub.barriers[g_stub.n_barriers] = images[i];
        g_stub.barrier_src[g_stub.n_barriers] = src_stage;
        g_stub.barrier_dst[g_stub.n_barriers] = dst_stage;
        g_stub.n_barriers++;
    }
}

static void stub_cmd_copy_image(VkCommandBuffer buffer, VkImage src_image,
                                VkImageLayout src_layout, VkImage dst_image,
                                VkImageLayout dst_layout, uint32_t n_regions,
                                const VkImageCopy* regions)
{
    (void)buffer;
    (void)src_layout;
    (void)dst_layout;
    g_stub.n_copy_image++;
    g_stub.copy_image_src = src_image;
    g_stub.copy_image_dst = dst_image;
    if (n_regions > 0)
        g_stub.copy_image_region = regions[0];
}

static void stub_cmd_copy_buffer_to_image(VkCommandBuffer buffer, VkBuffer src,
                                          VkImage dst_image, VkImageLayout layout,
                                          uint32_t n_regions,
                                          const VkBufferImageCopy* regions)
{
    (void)buffer;
    (void)src;
    (void)layout;
    g_stub.n_copy_buf2img++;
    g_stub.copy_buf2img_image = dst_image;
    if (n_regions > 0)
        g_stub.copy_buf2img_region = regions[0];
}

static VkResult stub_queue_submit(VkQueue queue, uint32_t n_submits,
                                  const VkSubmitInfo* submits, VkFence fence)
{
    /* record every submit of this call in ring order (the cross-engine
     * path issues two) */
    for (uint32_t i = 0; i < n_submits && g_stub.n_submit < 4; i++) {
        submit_rec_t* rec = &g_stub.submits[g_stub.n_submit % 4];
        memset(rec, 0, sizeof(*rec));
        rec->queue = queue;
        rec->fence = fence;
        const VkSubmitInfo* si = &submits[i];
        uint32_t w = si->waitSemaphoreCount < 4 ? si->waitSemaphoreCount : 4;
        for (uint32_t s = 0; s < w; s++)
            rec->wait[s] = si->pWaitSemaphores[s];
        rec->wait_count = si->waitSemaphoreCount;
        uint32_t sig =
            si->signalSemaphoreCount < 4 ? si->signalSemaphoreCount : 4;
        for (uint32_t s = 0; s < sig; s++)
            rec->signal[s] = si->pSignalSemaphores[s];
        rec->signal_count = si->signalSemaphoreCount;
        rec->cmd_count = si->commandBufferCount;
        rec->cmd = si->commandBufferCount > 0 ? si->pCommandBuffers[0]
                                               : VK_NULL_HANDLE;
        g_stub.n_submit++;
    }
    return g_stub.submit_result;
}

static VkResult stub_create_buffer(VkDevice device,
                                   const VkBufferCreateInfo* info,
                                   const VkAllocationCallbacks* pAllocator,
                                   VkBuffer* pBuffer)
{
    (void)device;
    (void)pAllocator;
    if (g_stub.n_create_buffer < 16) {
        g_stub.created_usage[g_stub.n_create_buffer] = info->usage;
        g_stub.created_size[g_stub.n_create_buffer] = info->size;
    }
    g_stub.n_create_buffer++;
    *pBuffer = (VkBuffer)next_handle();
    return VK_SUCCESS;
}

static void stub_destroy_buffer(VkDevice device, VkBuffer buffer,
                                const VkAllocationCallbacks* pAllocator)
{
    (void)device;
    (void)buffer;
    (void)pAllocator;
    g_stub.n_destroy_buffer++;
}

static void stub_get_buffer_memory_requirements(VkDevice device, VkBuffer buffer,
                                                VkMemoryRequirements* req)
{
    (void)device;
    (void)buffer;
    req->size = g_stub.created_size[g_stub.n_create_buffer - 1];
    req->alignment = 256;
    req->memoryTypeBits = 1; /* type 0: host-visible */
}

static VkResult stub_create_image(VkDevice device, const VkImageCreateInfo* info,
                                  const VkAllocationCallbacks* pAllocator,
                                  VkImage* pImage)
{
    (void)device;
    (void)pAllocator;
    if (g_stub.n_create_image < 16) {
        g_stub.created_extent[g_stub.n_create_image].width = info->extent.width;
        g_stub.created_extent[g_stub.n_create_image].height =
            info->extent.height;
    }
    g_stub.n_create_image++;
    *pImage = (VkImage)next_handle();
    return VK_SUCCESS;
}

static void stub_destroy_image(VkDevice device, VkImage image,
                               const VkAllocationCallbacks* pAllocator)
{
    (void)device;
    (void)image;
    (void)pAllocator;
    g_stub.n_destroy_image++;
}

static VkResult stub_create_image_view(VkDevice device,
                                       const VkImageViewCreateInfo* info,
                                       const VkAllocationCallbacks* pAllocator,
                                       VkImageView* pImageView)
{
    (void)device;
    (void)info;
    (void)pAllocator;
    g_stub.n_create_image_view++;
    *pImageView = (VkImageView)next_handle();
    return VK_SUCCESS;
}

static void stub_destroy_image_view(VkDevice device, VkImageView view,
                                    const VkAllocationCallbacks* pAllocator)
{
    (void)device;
    (void)view;
    (void)pAllocator;
    g_stub.n_destroy_image_view++;
}

static void stub_get_image_memory_requirements(VkDevice device, VkImage image,
                                               VkMemoryRequirements* req)
{
    (void)device;
    (void)image;
    req->size = 1024;
    req->alignment = 256;
    req->memoryTypeBits = 2; /* type 1: device-local */
}

static VkResult stub_allocate_memory(VkDevice device,
                                     const VkMemoryAllocateInfo* info,
                                     const VkAllocationCallbacks* pAllocator,
                                     VkDeviceMemory* pMemory)
{
    (void)device;
    (void)pAllocator;
    g_stub.n_alloc_memory++;
    g_stub.last_alloc_size = info->allocationSize;
    *pMemory = (VkDeviceMemory)next_handle();
    return VK_SUCCESS;
}

static void stub_free_memory(VkDevice device, VkDeviceMemory memory,
                             const VkAllocationCallbacks* pAllocator)
{
    (void)device;
    (void)memory;
    (void)pAllocator;
    g_stub.n_free_memory++;
}

static VkResult stub_bind_buffer_memory(VkDevice device, VkBuffer buffer,
                                        VkDeviceMemory memory,
                                        VkDeviceSize offset)
{
    (void)device;
    (void)buffer;
    (void)memory;
    (void)offset;
    g_stub.n_bind_buffer_memory++;
    return VK_SUCCESS;
}

static VkResult stub_bind_image_memory(VkDevice device, VkImage image,
                                       VkDeviceMemory memory, VkDeviceSize offset)
{
    (void)device;
    (void)image;
    (void)memory;
    (void)offset;
    g_stub.n_bind_image_memory++;
    return VK_SUCCESS;
}

/*
 * MapMemory hands out the shared mem_pool: every upload in these tests
 * (crosshair 10000 B, quad 64 B, index 12 B, game FB copy 1 KB) fits in
 * it, so the tests can inspect the exact bytes each upload path wrote.
 */
static VkResult stub_map_memory(VkDevice device, VkDeviceMemory memory,
                                VkDeviceSize offset, VkDeviceSize size,
                                VkMemoryMapFlags flags, void** ppData)
{
    (void)device;
    (void)memory;
    (void)offset;
    (void)flags;
    g_stub.n_map_memory++;
    g_stub.last_map_size = size;
    memset(mem_pool, 0, sizeof(mem_pool));
    *ppData = mem_pool;
    return VK_SUCCESS;
}

static void stub_unmap_memory(VkDevice device, VkDeviceMemory memory)
{
    (void)device;
    (void)memory;
    g_stub.n_unmap++;
}

static VkResult stub_flush_mapped_memory_ranges(VkDevice device, uint32_t n,
                                                const VkMappedMemoryRange* ranges)
{
    (void)device;
    (void)n;
    (void)ranges;
    g_stub.n_flush++;
    return VK_SUCCESS;
}

static VkResult stub_allocate_desc_sets(VkDevice device,
                                        const VkDescriptorSetAllocateInfo* info,
                                        VkDescriptorSet* pDescriptorSets)
{
    (void)device;
    (void)info;
    g_stub.n_alloc_desc_sets++;
    if (pDescriptorSets)
        *pDescriptorSets = (VkDescriptorSet)next_handle();
    return VK_SUCCESS;
}

static void stub_update_desc_sets(VkDevice device, uint32_t n_writes,
                                  const VkWriteDescriptorSet* writes,
                                  uint32_t n_copies,
                                  const VkCopyDescriptorSet* copies)
{
    (void)device;
    (void)writes;
    (void)n_copies;
    (void)copies;
    if (n_writes > 0)
        g_stub.n_update_desc_sets++;
}

static VkResult stub_free_desc_sets(VkDevice device, VkDescriptorPool pool,
                                    uint32_t n, const VkDescriptorSet* sets)
{
    (void)device;
    (void)pool;
    (void)n;
    (void)sets;
    g_stub.n_free_desc_sets++;
    return VK_SUCCESS;
}

static void stub_get_mem_props(VkPhysicalDevice physical_device,
                               VkPhysicalDeviceMemoryProperties* pProperties)
{
    (void)physical_device;
    *pProperties = g_stub.mem_props;
}

/*
 * Reset all state to a fresh device with two memory types (host-visible
 * + coherent as type 0, device-local as type 1), one 16x16 swapchain with
 * three images and four pre-created draw slots (command buffers,
 * semaphores, fences), and the fake stable device-scoped resources the
 * render path assumes vkCreateDevice created.
 */
static void reset_stub(void)
{
    memset(&g_stub, 0, sizeof(g_stub));
    g_stub.wait_fences_result = VK_SUCCESS;
    g_stub.submit_result = VK_SUCCESS;
    g_stub.end_cmd_result = VK_SUCCESS;

    memset(&g_device, 0, sizeof(g_device));
    memset(&g_instance, 0, sizeof(g_instance));
    memset(&g_graphics_queue, 0, sizeof(g_graphics_queue));
    memset(&g_present_queue, 0, sizeof(g_present_queue));
    memset(&g_swap, 0, sizeof(g_swap));

    g_stub.mem_props.memoryTypeCount = 2;
    g_stub.mem_props.memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    g_stub.mem_props.memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    g_instance.instance = (VkInstance)next_handle();
    g_instance.vtable.GetPhysicalDeviceMemoryProperties = stub_get_mem_props;

    g_device.instance = &g_instance;
    g_device.device = (VkDevice)next_handle();
    g_device.physical_device = (VkPhysicalDevice)next_handle();
    g_device.cmd_pool = (VkCommandPool)next_handle();
    g_device.properties.limits.nonCoherentAtomSize = 64;
    g_device.set_device_loader_data = stub_set_loader_data;

    /* stable device-scoped resources, faked as pre-created */
    g_device.crosshair_sampler = (VkSampler)next_handle();
    g_device.descriptor_layout = (VkDescriptorSetLayout)next_handle();
    g_device.descriptor_pool = (VkDescriptorPool)next_handle();
    g_device.pipeline_layout = (VkPipelineLayout)next_handle();
    g_device.render_pass = (VkRenderPass)next_handle();
    g_device.pipeline = (VkPipeline)next_handle();
    g_device.shader_desc_layout = (VkDescriptorSetLayout)next_handle();
    g_device.shader_desc_pool = (VkDescriptorPool)next_handle();
    g_device.shader_pipeline_layout = (VkPipelineLayout)next_handle();
    g_device.shader_pipeline = (VkPipeline)next_handle();

    /* device vtable */
    g_device.vtable.WaitForFences = stub_wait_fences;
    g_device.vtable.ResetFences = stub_reset_fences;
    g_device.vtable.AllocateCommandBuffers = stub_allocate_cmd_buffers;
    g_device.vtable.FreeCommandBuffers = stub_free_cmd_buffers;
    g_device.vtable.CreateSemaphore = stub_create_semaphore;
    g_device.vtable.DestroySemaphore = stub_destroy_semaphore;
    g_device.vtable.CreateFence = stub_create_fence;
    g_device.vtable.DestroyFence = stub_destroy_fence;
    g_device.vtable.ResetCommandBuffer = stub_reset_cmd_buffer;
    g_device.vtable.BeginCommandBuffer = stub_begin_cmd_buffer;
    g_device.vtable.EndCommandBuffer = stub_end_cmd_buffer;
    g_device.vtable.CmdBeginRenderPass = stub_cmd_begin_render_pass;
    g_device.vtable.CmdEndRenderPass = stub_cmd_end_render_pass;
    g_device.vtable.CmdBindPipeline = stub_cmd_bind_pipeline;
    g_device.vtable.CmdBindVertexBuffers = stub_cmd_bind_vertex_buffers;
    g_device.vtable.CmdBindIndexBuffer = stub_cmd_bind_index_buffer;
    g_device.vtable.CmdSetViewport = stub_cmd_set_viewport;
    g_device.vtable.CmdSetScissor = stub_cmd_set_scissor;
    g_device.vtable.CmdBindDescriptorSets = stub_cmd_bind_descriptor_sets;
    g_device.vtable.CmdPushConstants = stub_cmd_push_constants;
    g_device.vtable.CmdDrawIndexed = stub_cmd_draw_indexed;
    g_device.vtable.CmdPipelineBarrier = stub_cmd_pipeline_barrier;
    g_device.vtable.CmdCopyImage = stub_cmd_copy_image;
    g_device.vtable.CmdCopyBufferToImage = stub_cmd_copy_buffer_to_image;
    g_device.vtable.QueueSubmit = stub_queue_submit;
    g_device.vtable.CreateBuffer = stub_create_buffer;
    g_device.vtable.DestroyBuffer = stub_destroy_buffer;
    g_device.vtable.GetBufferMemoryRequirements =
        stub_get_buffer_memory_requirements;
    g_device.vtable.CreateImage = stub_create_image;
    g_device.vtable.DestroyImage = stub_destroy_image;
    g_device.vtable.CreateImageView = stub_create_image_view;
    g_device.vtable.DestroyImageView = stub_destroy_image_view;
    g_device.vtable.GetImageMemoryRequirements =
        stub_get_image_memory_requirements;
    g_device.vtable.AllocateMemory = stub_allocate_memory;
    g_device.vtable.FreeMemory = stub_free_memory;
    g_device.vtable.BindBufferMemory = stub_bind_buffer_memory;
    g_device.vtable.BindImageMemory = stub_bind_image_memory;
    g_device.vtable.MapMemory = stub_map_memory;
    g_device.vtable.UnmapMemory = stub_unmap_memory;
    g_device.vtable.FlushMappedMemoryRanges = stub_flush_mapped_memory_ranges;
    g_device.vtable.AllocateDescriptorSets = stub_allocate_desc_sets;
    g_device.vtable.UpdateDescriptorSets = stub_update_desc_sets;
    g_device.vtable.FreeDescriptorSets = stub_free_desc_sets;

    /* queues: same family by default */
    g_graphics_queue.device = &g_device;
    g_graphics_queue.queue = (VkQueue)next_handle();
    g_graphics_queue.family_index = 0;
    g_device.graphic_queue = &g_graphics_queue;

    g_present_queue.device = &g_device;
    g_present_queue.queue = (VkQueue)next_handle();
    g_present_queue.family_index = 0;

    /* swapchain: 16x16, three images, four pre-created draw slots */
    g_swap.device_data = &g_device;
    g_swap.width = 16;
    g_swap.height = 16;
    g_swap.format = VK_FORMAT_B8G8R8A8_UNORM;
    g_swap.n_images = 3;
    for (uint32_t i = 0; i < g_swap.n_images; i++)
        g_swap.images[i] = (VkImage)next_handle();
    for (int i = 0; i < 4; i++) {
        memset(&g_draws[i], 0, sizeof(g_draws[i]));
        g_swap.draws[i] = &g_draws[i];
        g_draws[i].cmd_buffer = (VkCommandBuffer)next_handle();
        g_draws[i].semaphore = (VkSemaphore)next_handle();
        g_draws[i].crossengine_semaphore = (VkSemaphore)next_handle();
        g_draws[i].fence = (VkFence)next_handle();
    }
}

/* Monotonic time in milliseconds, for animation-timing tests. */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Store an absolute monotonic time (ms) into the animation base time. */
static void set_last_frame_time(swapchain_data_t* data, uint64_t ms)
{
    data->anim_last_frame_time.tv_sec = (time_t)(ms / 1000);
    data->anim_last_frame_time.tv_nsec = (long)(ms % 1000) * 1000000L;
}

/* No fence in flight yet; a signaled fence is waited+reset; a fence that
 * does not signal in time keeps the slot marked as submitted. */
static void test_wait_draw_slot_ready(void)
{
    reset_stub();
    krosshair_draw_t* draw = &g_draws[0];

    draw->fence_submitted = 0;
    CHECK_EQ(wait_draw_slot_ready(&g_device, draw, 0), 1);
    CHECK_EQ(g_stub.n_wait_fences, 0);
    CHECK_EQ(g_stub.n_reset_fences, 0);

    draw->fence_submitted = 1;
    CHECK_EQ(wait_draw_slot_ready(&g_device, draw, 0), 1);
    CHECK_EQ(g_stub.n_wait_fences, 1);
    CHECK(g_stub.waited_fences[0] == draw->fence);
    CHECK_EQ(g_stub.wait_timeouts[0], 100000000);
    CHECK_EQ(g_stub.n_reset_fences, 1);
    CHECK_EQ(draw->fence_submitted, 0);

    draw->fence_submitted = 1;
    g_stub.wait_fences_result = VK_TIMEOUT;
    CHECK_EQ(wait_draw_slot_ready(&g_device, draw, 0), 0);
    CHECK_EQ(g_stub.n_wait_fences, 2);
    CHECK_EQ(g_stub.n_reset_fences, 1);
    CHECK_EQ(draw->fence_submitted, 1);
}

/* No crosshair image or env override available: the built-in 50x50 crosshair
 * is uploaded once; a second call reuses everything. */
static void test_ensure_swapchain_crosshair_builtin(void)
{
    reset_stub();

    ensure_swapchain_crosshair(&g_swap, g_draws[0].cmd_buffer);
    CHECK_EQ(g_swap.crosshair_uploaded, 1);
    CHECK_EQ(g_swap.crosshair_tex_width, default_crosshair_width);
    CHECK(g_swap.crosshair_image != VK_NULL_HANDLE);
    CHECK(g_swap.crosshair_image_view != VK_NULL_HANDLE);
    CHECK(g_swap.descriptor_set != VK_NULL_HANDLE);
    CHECK_EQ(g_stub.n_alloc_desc_sets, 1);
    CHECK_EQ(g_stub.n_create_image, 1);
    CHECK_EQ(g_stub.created_extent[0].width, default_crosshair_width);
    CHECK_EQ(g_stub.created_extent[0].height, default_crosshair_height);
    CHECK_EQ(g_stub.n_create_buffer, 1);
    CHECK_EQ(g_stub.created_usage[0], VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CHECK_EQ(g_stub.created_size[0],
             default_crosshair_width * default_crosshair_height * 4);
    CHECK_EQ(g_stub.n_barriers, 2);
    CHECK_EQ(g_stub.n_copy_buf2img, 1);
    /* the upload wrote the built-in pixels */
    CHECK(memcmp(mem_pool, default_crosshair_data,
                 default_crosshair_width * default_crosshair_height * 4) == 0);

    /* second call: nothing is recreated */
    ensure_swapchain_crosshair(&g_swap, g_draws[0].cmd_buffer);
    CHECK_EQ(g_stub.n_create_image, 1);
    CHECK_EQ(g_stub.n_create_buffer, 1);
    CHECK_EQ(g_stub.n_copy_buf2img, 1);
}

/* Created at swapchain size, reused when the size is unchanged, and
 * destroyed+recreated when it changes. */
static void test_ensure_game_fb_copy(void)
{
    reset_stub();

    CHECK_EQ(g_swap.game_fb_image, VK_NULL_HANDLE);
    ensure_game_fb_copy(&g_swap);
    CHECK(g_swap.game_fb_image != VK_NULL_HANDLE);
    CHECK(g_swap.game_fb_image_view != VK_NULL_HANDLE);
    CHECK(g_swap.game_fb_mem != VK_NULL_HANDLE);
    CHECK_EQ(g_swap.game_fb_width, g_swap.width);
    CHECK_EQ(g_swap.game_fb_height, g_swap.height);
    CHECK_EQ(g_stub.n_create_image, 1);
    CHECK_EQ(g_stub.created_extent[0].width, g_swap.width);
    CHECK_EQ(g_stub.created_extent[0].height, g_swap.height);
    CHECK_EQ(g_stub.n_create_image_view, 1);
    CHECK_EQ(g_stub.n_alloc_memory, 1);
    CHECK_EQ(g_stub.n_bind_image_memory, 1);

    ensure_game_fb_copy(&g_swap);
    CHECK_EQ(g_stub.n_create_image, 1);
    CHECK_EQ(g_stub.n_free_memory, 0);
    CHECK_EQ(g_stub.n_destroy_image, 0);

    g_swap.width = 32;
    g_swap.height = 32;
    ensure_game_fb_copy(&g_swap);
    CHECK_EQ(g_stub.n_create_image, 2);
    CHECK_EQ(g_stub.created_extent[1].width, 32);
    CHECK_EQ(g_stub.created_extent[1].height, 32);
    CHECK_EQ(g_stub.n_destroy_image, 1);
    CHECK_EQ(g_stub.n_destroy_image_view, 1);
    CHECK_EQ(g_stub.n_free_memory, 1);
}

/* Encodes the four-barrier copy of the presented image into the game
 * framebuffer texture (sampled by the dynamic mask shader). */
static void test_record_framebuffer_copy(void)
{
    reset_stub();
    g_swap.game_fb_image = (VkImage)next_handle();
    g_swap.game_fb_image_view = (VkImageView)next_handle();
    VkCommandBuffer cmd = g_draws[1].cmd_buffer;

    record_framebuffer_copy(&g_swap, cmd, 1, &g_present_queue);

    CHECK_EQ(g_stub.n_barriers, 4);

    /* presented image: PRESENT_SRC -> TRANSFER_SRC (to copy out) */
    CHECK(g_stub.barriers[0].image == g_swap.images[1]);
    CHECK_EQ(g_stub.barriers[0].oldLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    CHECK_EQ(g_stub.barriers[0].newLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK_EQ(g_stub.barriers[0].srcAccessMask,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK_EQ(g_stub.barriers[0].dstAccessMask, VK_ACCESS_TRANSFER_READ_BIT);
    CHECK_EQ(g_stub.barrier_src[0], VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
    CHECK_EQ(g_stub.barrier_dst[0], VK_PIPELINE_STAGE_TRANSFER_BIT);

    /* game fb image: UNDEFINED -> TRANSFER_DST (copy target) */
    CHECK(g_stub.barriers[1].image == g_swap.game_fb_image);
    CHECK_EQ(g_stub.barriers[1].oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK_EQ(g_stub.barriers[1].newLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK_EQ(g_stub.barriers[1].dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT);
    CHECK_EQ(g_stub.barriers[1].srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
    CHECK_EQ(g_stub.barrier_src[1], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    /* game fb image: TRANSFER_DST -> SHADER_READ_ONLY (sampled) */
    CHECK(g_stub.barriers[2].image == g_swap.game_fb_image);
    CHECK_EQ(g_stub.barriers[2].newLayout,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK_EQ(g_stub.barriers[2].srcAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT);
    CHECK_EQ(g_stub.barriers[2].dstAccessMask, VK_ACCESS_SHADER_READ_BIT);
    CHECK_EQ(g_stub.barrier_dst[2], VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* presented image: TRANSFER_SRC -> COLOR_ATTACHMENT (for the render) */
    CHECK(g_stub.barriers[3].image == g_swap.images[1]);
    CHECK_EQ(g_stub.barriers[3].newLayout,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK_EQ(g_stub.barriers[3].srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT);
    CHECK_EQ(g_stub.barriers[3].dstAccessMask,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK_EQ(g_stub.barrier_dst[3],
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    CHECK_EQ(g_stub.n_copy_image, 1);
    CHECK(g_stub.copy_image_src == g_swap.images[1]);
    CHECK(g_stub.copy_image_dst == g_swap.game_fb_image);
    CHECK_EQ(g_stub.copy_image_region.extent.width, g_swap.width);
    CHECK_EQ(g_stub.copy_image_region.extent.height, g_swap.height);
}

/* Moves the presented image to COLOR_ATTACHMENT_OPTIMAL for the render
 * pass, transferring ownership from the present queue family. */
static void test_transition_swapchain_for_render(void)
{
    reset_stub();

    transition_swapchain_for_render(&g_swap, g_draws[0].cmd_buffer, 0,
                                    &g_present_queue);
    CHECK_EQ(g_stub.n_barriers, 1);
    CHECK(g_stub.barriers[0].image == g_swap.images[0]);
    CHECK_EQ(g_stub.barriers[0].oldLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    CHECK_EQ(g_stub.barriers[0].newLayout,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK_EQ(g_stub.barriers[0].srcAccessMask,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK_EQ(g_stub.barriers[0].dstAccessMask,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK_EQ(g_stub.barriers[0].srcQueueFamilyIndex,
             g_present_queue.family_index);
    CHECK_EQ(g_stub.barriers[0].dstQueueFamilyIndex,
             g_graphics_queue.family_index);
    CHECK_EQ(g_stub.barrier_src[0], VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
    CHECK_EQ(g_stub.barrier_dst[0], VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);

    /* cross-family present: the source family must be the present one */
    g_present_queue.family_index = 1;
    transition_swapchain_for_render(&g_swap, g_draws[0].cmd_buffer, 0,
                                    &g_present_queue);
    CHECK_EQ(g_stub.n_barriers, 2);
    CHECK_EQ(g_stub.barriers[1].srcQueueFamilyIndex, 1);
    CHECK_EQ(g_stub.barriers[1].dstQueueFamilyIndex, 0);
}

/* Animation clock: static crosshairs never advance; a timed animation
 * advances until its delay budget is spent, and wraps at the last frame. */
static void test_advance_anim_frame(void)
{
    reset_stub();
    swapchain_data_t* data = &g_swap;
    krosshair_draw_t* draw = &g_draws[0];

    /* static (single frame): no-op */
    data->anim_frame_count = 1;
    data->anim_delays = NULL;
    data->anim_current_frame = 0;
    draw->vertex_buffer_initialized = 1;
    advance_anim_frame(data, draw);
    CHECK_EQ(data->anim_current_frame, 0);
    CHECK_EQ(draw->vertex_buffer_initialized, 1);

    /* no delays: no-op */
    data->anim_frame_count = 3;
    data->anim_delays = NULL;
    advance_anim_frame(data, draw);
    CHECK_EQ(data->anim_current_frame, 0);

    /* 3 frames @ 50ms, 120ms elapsed: two advances (120, 70ms budgets) */
    static int delays[3] = {50, 50, 50};
    uint64_t now = now_ms();
    data->anim_frame_count = 3;
    data->anim_delays = delays;
    data->anim_current_frame = 0;
    data->anim_frame_height = 20;
    data->crosshair_tex_width = 50;
    set_last_frame_time(data, now - 120);
    draw->vertex_buffer_initialized = 1;
    vertex_t saved_vertices[4];
    memcpy(saved_vertices, data->vertices, sizeof(saved_vertices));
    advance_anim_frame(data, draw);
    CHECK_EQ(data->anim_current_frame, 2);
    CHECK_EQ(draw->vertex_buffer_initialized, 0);
    CHECK(memcmp(saved_vertices, data->vertices, sizeof(saved_vertices)) != 0);

    /* 25ms elapsed at 10ms/frame: two advances, wrapping 2 -> 0 -> 1 */
    static int fast[3] = {10, 10, 10};
    now = now_ms();
    data->anim_frame_count = 3;
    data->anim_delays = fast;
    data->anim_current_frame = 2;
    set_last_frame_time(data, now - 25);
    advance_anim_frame(data, draw);
    CHECK_EQ(data->anim_current_frame, 1);
}

/* Copies the given bytes into the mapped upload memory and flushes it. */
static void test_upload_device_memory(void)
{
    reset_stub();
    VkDeviceMemory mem = (VkDeviceMemory)next_handle();
    unsigned char payload[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    upload_device_memory(&g_device, mem, payload, sizeof(payload));

    CHECK_EQ(g_stub.n_map_memory, 1);
    CHECK_EQ(g_stub.last_map_size, VK_WHOLE_SIZE);
    CHECK_EQ(g_stub.n_flush, 1);
    CHECK_EQ(g_stub.n_unmap, 1);
    CHECK(memcmp(mem_pool, payload, sizeof(payload)) == 0);
}

/* Lazily creates the per-draw-slot vertex+index buffers (host-visible,
 * atom-aligned) and uploads the current quad; skips when both exist. */
static void test_ensure_quad_buffers(void)
{
    reset_stub();
    krosshair_draw_t* draw = &g_draws[0];

    ensure_quad_buffers(&g_device, &g_swap, draw);

    CHECK_EQ(draw->vertex_buffer_size, 64);
    CHECK_EQ(draw->vertex_buffer_initialized, 1);
    CHECK_EQ(draw->index_buffer_size, 64); /* 12 rounded to atom 64 */
    CHECK_EQ(draw->index_buffer_initialized, 1);
    CHECK_EQ(g_stub.n_create_buffer, 2);
    CHECK_EQ(g_stub.created_usage[0], VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    CHECK_EQ(g_stub.created_usage[1], VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    CHECK_EQ(g_stub.created_size[0], 64);
    CHECK_EQ(g_stub.created_size[1], 64); /* 12 bytes aligned to atom 64 */
    CHECK_EQ(g_stub.n_alloc_memory, 2);
    CHECK_EQ(g_stub.last_alloc_size, 64);
    CHECK_EQ(g_stub.n_bind_buffer_memory, 2);
    CHECK_EQ(g_stub.n_map_memory, 2);
    /* last upload was the index buffer */
    CHECK(memcmp(mem_pool, indices, sizeof(indices)) == 0);

    /* both sized: second call is a no-op */
    ensure_quad_buffers(&g_device, &g_swap, draw);
    CHECK_EQ(g_stub.n_create_buffer, 2);
    CHECK_EQ(g_stub.n_destroy_buffer, 0);
    CHECK_EQ(g_stub.n_map_memory, 2);

    /* a too-small vertex buffer is replaced */
    draw->vertex_buffer_size = 32;
    ensure_quad_buffers(&g_device, &g_swap, draw);
    CHECK_EQ(g_stub.n_create_buffer, 3);
    CHECK_EQ(g_stub.n_destroy_buffer, 1);
    CHECK_EQ(g_stub.n_free_memory, 1);
    CHECK_EQ(draw->vertex_buffer_size, 64);
}

/* Gate: only draws when the mask is uploaded, the shader pipeline exists,
 * and the game framebuffer is available. Uploads the mask quad once,
 * updates the desc set, pushes the mask push constants, draws 6 indices. */
static void test_record_dynamic_mask_draw(void)
{
    reset_stub();
    swapchain_data_t* data = &g_swap;
    krosshair_draw_t* draw = &g_draws[0];

    data->dynamic_mask.uploaded = 0;
    record_dynamic_mask_draw(data, draw);
    CHECK_EQ(g_stub.n_bind_pipeline, 0);
    CHECK_EQ(g_stub.n_draw_indexed, 0);

    data->dynamic_mask.uploaded = 1;
    data->dynamic_mask.image_view = (VkImageView)next_handle();
    data->dynamic_mask.vertices[0].pos = (vec2_t){1.0f, 2.0f};
    data->dynamic_mask.vertices[1].pos = (vec2_t){2.0f, 2.0f};
    data->dynamic_mask.vertices[2].pos = (vec2_t){2.0f, 3.0f};
    data->dynamic_mask.vertices[3].pos = (vec2_t){1.0f, 3.0f};

    /* no shader pipeline: no draw */
    record_dynamic_mask_draw(data, draw);
    CHECK_EQ(g_stub.n_bind_pipeline, 0);

    g_device.shader_pipeline = (VkPipeline)next_handle();

    /* no game fb: no draw */
    record_dynamic_mask_draw(data, draw);
    CHECK_EQ(g_stub.n_bind_pipeline, 0);

    data->game_fb_image_view = (VkImageView)next_handle();

    record_dynamic_mask_draw(data, draw);
    CHECK_EQ(g_stub.n_create_buffer, 1);
    CHECK_EQ(g_stub.created_usage[0], VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    CHECK_EQ(g_stub.n_alloc_desc_sets, 1);
    CHECK_EQ(g_stub.n_update_desc_sets, 1);
    CHECK_EQ(g_stub.n_bind_pipeline, 1);
    CHECK(g_stub.bound_pipeline == g_device.shader_pipeline);
    CHECK_EQ(g_stub.n_bind_vbufs, 1);
    CHECK(g_stub.bound_vbuf == draw->vertex_buffer2);
    CHECK_EQ(g_stub.n_bind_desc_sets, 1);
    CHECK_EQ(g_stub.n_push_constants, 1);
    CHECK_EQ(g_stub.push_size, sizeof(struct dynamic_push_constants));
    CHECK(g_stub.push_ptr == &data->dynamic_pc);
    CHECK_EQ(g_stub.n_draw_indexed, 1);
    CHECK_EQ(g_stub.draw_index_count, 6);
    CHECK(memcmp(mem_pool, data->dynamic_mask.vertices,
                 sizeof(data->dynamic_mask.vertices)) == 0);
    CHECK(data->dynamic_pc.quad_ndc_min[0] == 1.0f);
    CHECK(data->dynamic_pc.quad_ndc_min[1] == 2.0f);
    CHECK(data->dynamic_pc.quad_ndc_size[0] == 1.0f);
    CHECK(data->dynamic_pc.quad_ndc_size[1] == 1.0f);

    /* second frame: mask vertices re-uploaded, but the buffer and desc
     * set are reused */
    record_dynamic_mask_draw(data, draw);
    CHECK_EQ(g_stub.n_create_buffer, 1);
    CHECK_EQ(g_stub.n_alloc_desc_sets, 1);
    CHECK_EQ(g_stub.n_update_desc_sets, 1);
    CHECK_EQ(g_stub.n_map_memory, 2);
    CHECK_EQ(g_stub.n_draw_indexed, 2);
}

/* Same-family: one submit on the graphics queue carrying the app's wait
 * semaphores. Cross-family: two submits (present queue first, then the
 * graphics queue through the crossengine semaphore). */
static void test_submit_overlay_draw(void)
{
    reset_stub();
    krosshair_draw_t* draw = &g_draws[0];
    VkSemaphore app_sem[2] = {(VkSemaphore)next_handle(),
                              (VkSemaphore)next_handle()};

    VkResult rc = submit_overlay_draw(&g_device, draw, &g_present_queue,
                                      app_sem, 2);
    CHECK_EQ(rc, 0);
    CHECK_EQ(draw->fence_submitted, 1);
    CHECK_EQ(g_stub.n_submit, 1);
    const submit_rec_t* rec = &g_stub.submits[0];
    CHECK(rec->queue == g_graphics_queue.queue);
    CHECK(rec->fence == draw->fence);
    CHECK_EQ(rec->wait_count, 2);
    CHECK(rec->wait[0] == app_sem[0]);
    CHECK(rec->wait[1] == app_sem[1]);
    CHECK_EQ(rec->signal_count, 1);
    CHECK(rec->signal[0] == draw->semaphore);
    CHECK_EQ(rec->cmd_count, 1);
    CHECK(rec->cmd == draw->cmd_buffer);
    CHECK_EQ(g_stub.n_destroy_fence, 0); /* fence lives until the slot dies */
    CHECK_EQ(g_stub.n_free_cmd, 0);

    /* failed submit: -1, slot left unsubmitted */
    reset_stub();
    draw->fence_submitted = 0;
    g_stub.submit_result = VK_ERROR_DEVICE_LOST;
    rc = submit_overlay_draw(&g_device, draw, &g_present_queue, app_sem, 0);
    CHECK_EQ(rc, -1);
    CHECK_EQ(draw->fence_submitted, 0);
    CHECK_EQ(g_stub.n_submit, 1);

    /* cross-family present queue with no app semaphores: two submits */
    reset_stub();
    g_present_queue.family_index = 1;
    rc = submit_overlay_draw(&g_device, draw, &g_present_queue, NULL, 0);
    CHECK_EQ(rc, 0);
    CHECK_EQ(draw->fence_submitted, 1);
    CHECK_EQ(g_stub.n_submit, 2);
    CHECK(g_stub.submits[0].queue == g_present_queue.queue);
    CHECK(g_stub.submits[0].fence == VK_NULL_HANDLE);
    CHECK_EQ(g_stub.submits[0].wait_count, 0);
    CHECK_EQ(g_stub.submits[0].signal_count, 1);
    CHECK(g_stub.submits[0].signal[0] == draw->crossengine_semaphore);
    CHECK_EQ(g_stub.submits[1].queue, g_graphics_queue.queue);
    CHECK(g_stub.submits[1].fence == draw->fence);
    CHECK_EQ(g_stub.submits[1].wait_count, 1);
    CHECK(g_stub.submits[1].wait[0] == draw->crossengine_semaphore);
    CHECK(g_stub.submits[1].signal[0] == draw->semaphore);
    CHECK(g_stub.submits[1].cmd == draw->cmd_buffer);
}

/* The full single-family frame: builds the crosshair (built-in 50x50),
 * transitions the image, encodes the quad draw and submits. */
static void test_render_swapchain_display_full(void)
{
    reset_stub();
    crosshair_visible = 1;
    VkSemaphore image_sem = (VkSemaphore)next_handle();

    krosshair_draw_t* draw = render_swapchain_display(&g_swap, &g_present_queue,
                                                      &image_sem, 1, 0);
    CHECK(draw == &g_draws[0]);

    /* command buffer lifecycle */
    CHECK_EQ(g_stub.n_reset_cmd, 1);
    CHECK_EQ(g_stub.n_begin_cmd, 1);
    CHECK_EQ(g_stub.n_end_cmd, 1);

    /* crosshair: 50x50 image + view + desc set + upload buffer + copy */
    CHECK_EQ(g_swap.crosshair_uploaded, 1);
    CHECK_EQ(g_swap.crosshair_tex_width, 50);
    CHECK(g_swap.descriptor_set != VK_NULL_HANDLE);
    CHECK(g_swap.crosshair_image != VK_NULL_HANDLE);
    CHECK_EQ(g_stub.n_create_image, 1);
    CHECK_EQ(g_stub.created_extent[0].width, 50);
    CHECK_EQ(g_stub.created_extent[0].height, 50);
    CHECK_EQ(g_stub.n_create_image_view, 1);
    CHECK_EQ(g_stub.n_alloc_desc_sets, 1);
    CHECK_EQ(g_stub.n_create_buffer, 3); /* upload + vertex + index */
    CHECK_EQ(g_stub.created_usage[0], VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CHECK_EQ(g_stub.created_usage[1], VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    CHECK_EQ(g_stub.created_usage[2], VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    CHECK_EQ(g_stub.created_size[0], 50 * 50 * 4);
    CHECK_EQ(g_stub.n_copy_buf2img, 1);
    CHECK_EQ(g_stub.copy_buf2img_region.imageExtent.width, 50);
    CHECK_EQ(g_stub.copy_buf2img_region.imageExtent.height, 50);
    /* the last upload in the frame was the quad's index buffer */
    CHECK(memcmp(mem_pool, indices, sizeof(indices)) == 0);

    /* barriers: 2 upload transitions + 1 swapchain transition */
    CHECK_EQ(g_stub.n_barriers, 3);
    CHECK_EQ(g_stub.barriers[0].oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK_EQ(g_stub.barriers[0].newLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK_EQ(g_stub.barriers[1].newLayout,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(g_stub.barriers[2].image == g_swap.images[0]);
    CHECK_EQ(g_stub.barriers[2].oldLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    CHECK_EQ(g_stub.barriers[2].newLayout,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    /* the quad draw */
    CHECK_EQ(g_stub.n_begin_rp, 1);
    CHECK_EQ(g_stub.n_end_rp, 1);
    CHECK_EQ(g_stub.n_bind_pipeline, 1);
    CHECK(g_stub.bound_pipeline == g_device.pipeline);
    CHECK_EQ(g_stub.n_bind_vbufs, 1);
    CHECK(g_stub.bound_vbuf == draw->vertex_buffer);
    CHECK_EQ(g_stub.n_bind_ibuf, 1);
    CHECK(g_stub.bound_ibuf == draw->index_buffer);
    CHECK_EQ(g_stub.bound_idx_type, VK_INDEX_TYPE_UINT16);
    CHECK_EQ(g_stub.n_set_viewport, 1);
    CHECK_EQ(g_stub.viewport.width, g_swap.width);
    CHECK_EQ(g_stub.viewport.height, g_swap.height);
    CHECK_EQ(g_stub.n_set_scissor, 1);
    CHECK_EQ(g_stub.scissor.extent.width, g_swap.width);
    CHECK_EQ(g_stub.scissor.extent.height, g_swap.height);
    CHECK_EQ(g_stub.n_bind_desc_sets, 1);
    CHECK_EQ(g_stub.n_draw_indexed, 1);
    CHECK_EQ(g_stub.draw_index_count, 6);

    /* the submit */
    CHECK_EQ(g_stub.n_submit, 1);
    CHECK(g_stub.submits[0].queue == g_graphics_queue.queue);
    CHECK(g_stub.submits[0].fence == draw->fence);
    CHECK_EQ(g_stub.submits[0].wait_count, 1);
    CHECK(g_stub.submits[0].wait[0] == image_sem);
    CHECK(g_stub.submits[0].signal[0] == draw->semaphore);
    CHECK(g_stub.submits[0].cmd == draw->cmd_buffer);
    CHECK_EQ(draw->fence_submitted, 1);

    /* second frame, same image: no new crosshair/quad, one more submit */
    draw = render_swapchain_display(&g_swap, &g_present_queue, &image_sem, 1, 0);
    CHECK(draw == &g_draws[0]);
    CHECK_EQ(g_stub.n_create_image, 1);
    CHECK_EQ(g_stub.n_create_buffer, 3);
    CHECK_EQ(g_stub.n_submit, 2);
    /* the in-flight fence from frame 1 was waited on before re-recording */
    CHECK_EQ(g_stub.n_wait_fences, 1);
    CHECK_EQ(g_stub.n_reset_fences, 1);
}

/* Hidden crosshair: the frame is skipped before any device call. */
static void test_render_swapchain_display_hidden(void)
{
    reset_stub();
    crosshair_visible = 0;

    krosshair_draw_t* draw = render_swapchain_display(&g_swap, &g_present_queue,
                                                      NULL, 0, 0);
    CHECK(draw == NULL);
    CHECK_EQ(g_stub.n_begin_cmd, 0);
    CHECK_EQ(g_stub.n_submit, 0);
}

/* Cross-family present: the frame adds a queue-ownership return barrier
 * and submits through the present queue first. */
static void test_render_swapchain_display_cross_engine(void)
{
    reset_stub();
    crosshair_visible = 1;
    g_present_queue.family_index = 1;

    /* no app semaphores: submit_overlay_draw takes the two-submit
     * cross-engine path */
    krosshair_draw_t* draw = render_swapchain_display(&g_swap, &g_present_queue,
                                                       NULL, 0, 0);
    CHECK(draw == &g_draws[0]);

    /* 2 upload + 1 transition + 1 queue-ownership return barrier */
    CHECK_EQ(g_stub.n_barriers, 4);
    CHECK(g_stub.barriers[3].image == g_swap.images[0]);
    CHECK_EQ(g_stub.barriers[3].srcQueueFamilyIndex, 0);
    CHECK_EQ(g_stub.barriers[3].dstQueueFamilyIndex, 1);

    CHECK_EQ(g_stub.n_submit, 2);
    CHECK(g_stub.submits[0].queue == g_present_queue.queue);
    CHECK(g_stub.submits[0].fence == VK_NULL_HANDLE);
    CHECK(g_stub.submits[0].signal[0] == draw->crossengine_semaphore);
    CHECK(g_stub.submits[1].queue == g_graphics_queue.queue);
    CHECK(g_stub.submits[1].fence == draw->fence);
    CHECK(g_stub.submits[1].wait[0] == draw->crossengine_semaphore);
    CHECK(g_stub.submits[1].cmd == draw->cmd_buffer);
}

/* EndCommandBuffer failure aborts the frame before the submit. */
static void test_render_swapchain_display_end_failure(void)
{
    reset_stub();
    crosshair_visible = 1;
    g_stub.end_cmd_result = VK_ERROR_DEVICE_LOST;

    krosshair_draw_t* draw = render_swapchain_display(&g_swap, &g_present_queue,
                                                      NULL, 0, 0);
    CHECK(draw == NULL);
    CHECK_EQ(g_stub.n_end_cmd, 1);
    CHECK_EQ(g_stub.n_submit, 0);
}

/* Submit failure (same family) aborts the frame; the command buffer was
 * already encoded. */
static void test_render_swapchain_display_submit_failure(void)
{
    reset_stub();
    crosshair_visible = 1;
    g_stub.submit_result = VK_ERROR_DEVICE_LOST;
    VkSemaphore image_sem = (VkSemaphore)next_handle();

    krosshair_draw_t* draw = render_swapchain_display(&g_swap, &g_present_queue,
                                                       &image_sem, 1, 0);
    CHECK(draw == NULL);
    CHECK_EQ(g_stub.n_submit, 1);
}

int main(void)
{
    /* Deterministic built-in crosshair: point HOME at an empty dir so
     * no crosshair image or dynamic mask file can ever be found. */
    char home_dir[] = "/tmp/kh_render_test_XXXXXX";
    if (!mkdtemp(home_dir))
        return 1;
    setenv("HOME", home_dir, 1);
    unsetenv("KROSSHAIR_IMG");

    fprintf(stderr, "# test_render\n");

    test_wait_draw_slot_ready();
    test_ensure_swapchain_crosshair_builtin();
    test_ensure_game_fb_copy();
    test_record_framebuffer_copy();
    test_transition_swapchain_for_render();
    test_advance_anim_frame();
    test_upload_device_memory();
    test_ensure_quad_buffers();
    test_record_dynamic_mask_draw();
    test_submit_overlay_draw();
    test_render_swapchain_display_full();
    test_render_swapchain_display_hidden();
    test_render_swapchain_display_cross_engine();
    test_render_swapchain_display_end_failure();
    test_render_swapchain_display_submit_failure();

    fprintf(stderr, "  -> %d passed, %d failed\n", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
