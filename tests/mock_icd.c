/*
 * tests/mock_icd.c — a minimal Vulkan ICD for end-to-end layer testing.
 *
 * Loaded by the real Vulkan loader (via VK_DRIVER_FILES + mock_icd.json)
 * beneath the krosshair layer. It implements just enough of the driver
 * surface for the layer's full instance/device/swapchain/render/present
 * path to run, and counts the calls it receives so tests/test_mock_icd.c
 * can assert on them through the exported mock_icd_get_stats() entry point.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vk_icd.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

/*
 * The loader requires ICD handles to be pointers to real objects whose
 * first 8 bytes hold ICD_LOADER_MAGIC (see vk_icd.h); the loader overwrites
 * that slot with its dispatch data, so the objects must stay alive for the
 * process lifetime.
 */
struct mock_instance_struct {
        VK_LOADER_DATA loader_data;
        uint32_t dummy;
};

struct mock_physical_device_struct {
        VK_LOADER_DATA loader_data;
        uint32_t dummy;
};

struct mock_device_struct {
        VK_LOADER_DATA loader_data;
        uint32_t dummy;
};

struct mock_queue_struct {
        VK_LOADER_DATA loader_data;
        uint32_t dummy;
};

static struct mock_instance_struct mock_instance_obj;
static struct mock_physical_device_struct mock_physical_device_obj;
static struct mock_device_struct mock_device_obj;
static struct mock_queue_struct mock_queue_obj;

#define MOCK_INSTANCE ((VkInstance)&mock_instance_obj)
#define MOCK_PHYS     ((VkPhysicalDevice)&mock_physical_device_obj)
#define MOCK_DEVICE   ((VkDevice)&mock_device_obj)
#define MOCK_QUEUE    ((VkQueue)&mock_queue_obj)

/*
 * Initialize the loader-magic slot of the statically allocated ICD objects
 * before the loader touches them.
 */
__attribute__((constructor))
static void mock_icd_init(void)
{
        set_loader_magic_value(&mock_instance_obj);
        set_loader_magic_value(&mock_physical_device_obj);
        set_loader_magic_value(&mock_device_obj);
        set_loader_magic_value(&mock_queue_obj);
}

/*
 * Forward declaration: the instance proc-addr table below answers this
 * entry point so the loader can resolve it via vk_icdGetInstanceProcAddr.
 */
VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion);

/*
 * Forward declaration: the device proc-addr table below exports this so
 * the loader can fill the ICD device dispatch slot for DestroyDevice;
 * a missing slot becomes a NULL dispatch entry that segfaults at
 * teardown.
 */
static void mock_destroy_device(VkDevice device,
                                const VkAllocationCallbacks* allocator);

static uint64_t handle_counter = 0xAAAA0000;
static uint32_t swapchain_image_count = 3;
static struct mock_object* g_object_list;

/* call counters read by the test via mock_icd_get_stats() */
static int stats_submits     = 0;
static int stats_cmdbufs     = 0;
static int stats_presents    = 0;
static int stats_swapchains  = 0;
static int stats_acquires    = 0;
static int stats_queue_query = 0;
static VkDeviceSize stats_max_alloc = 0;

/*
 * Return the per-category call counts. Any pointer may be NULL to skip
 * that counter.
 */
int mock_icd_get_stats(int* submits, int* cmdbufs, int* presents,
                        int* swapchains, int* acquires, int* queue_queries,
                        VkDeviceSize* max_alloc)
{
        if (submits) *submits = stats_submits;
        if (cmdbufs) *cmdbufs = stats_cmdbufs;
        if (presents) *presents = stats_presents;
        if (swapchains) *swapchains = stats_swapchains;
        if (acquires) *acquires = stats_acquires;
        if (queue_queries) *queue_queries = stats_queue_query;
        if (max_alloc) *max_alloc = stats_max_alloc;
        return 0;
}

/*
 * Backing object for every device handle the mock hands out. The
 * loader's pfnSetDeviceLoaderData (vkSetDeviceDispatch) writes 16
 * bytes of dispatch data into the first 16 bytes of the object the
 * layer passes it (queues, command buffers), so every handle must be
 * a real heap object at least 16 bytes long; a fake integer handle
 * makes that write corrupt memory.
 */
struct mock_object {
        uint64_t magic; /* ICD_LOADER_MAGIC until the loader overwrites it */
        uint64_t serial; /* unique serial (handle_counter) for debugging */
        uint64_t size;   /* allocation size (memory objects only) */
        struct mock_object* next; /* g_object_list link */
        void* mapped;         /* mapped backing (memory objects only) */
};

/*
 * Allocate a fresh backing object for a new device handle.
 *
 * Returns: the new object, or NULL on allocation failure.
 */
static void* mock_new_object(void)
{
        struct mock_object* obj = calloc(1, sizeof(*obj));
        if (!obj)
                return NULL;
        obj->serial = ++handle_counter;
        set_loader_magic_value(obj);
        obj->next = g_object_list;
        g_object_list = obj;
        return obj;
}

/*
 * Release a handle's backing object, mirroring the driver freeing the
 * object on its Destroy or FreeMemory call. The handle must have come
 * from mock_new_object (i.e. be on g_object_list); anything else (the
 * fake integer handles the mock hands out for swapchain images) is
 * ignored, so double-destroy and foreign pointers can never free wild
 * memory.
 */
static void mock_free_object(struct mock_object* obj)
{
        if (!obj)
                return;
        struct mock_object** pp = &g_object_list;
        while (*pp && *pp != obj)
                pp = &(*pp)->next;
        if (*pp != obj)
                return;
        *pp = obj->next;
        if (obj->mapped)
                free(obj->mapped);
        free(obj);
}

/*
 * Generic "create one handle" implementation; covers every
 * Create* command whose output is a single handle:
 * (VkDevice, const Vk*CreateInfo*, const VkAllocationCallbacks*,
 *  Vk*handle*).
 */
static VkResult mock_create_one(VkDevice device, const void* info,
                                const VkAllocationCallbacks* alloc, void* out)
{
        (void)device;
        (void)info;
        (void)alloc;
        if (!out)
                return VK_ERROR_INITIALIZATION_FAILED;
        *(void**)out = mock_new_object();
        return VK_SUCCESS;
}

/*
 * Small stub for every (device, create-info, allocator, single-handle)
 * creation command; the stub hands out a fresh backing object.
 */
#define DEF_CREATE(NAME)                                                \
        static VkResult mock_Create##NAME(VkDevice device,              \
                                    const Vk##NAME##CreateInfo* info, \
                                    const VkAllocationCallbacks* alloc,\
                                    Vk##NAME* out)                    \
        {                                                             \
                return mock_create_one(device, info, alloc, (void*)out); \
        }

/*
 * Buffer creation records the requested size so GetBufferMemoryRequirements
 * can report it; the layer sizes its crosshair upload buffer from that
 * value, which the test uses to tell a custom image from the built-in
 * one.
 */
static VkResult mock_CreateBuffer(VkDevice device,
                                  const VkBufferCreateInfo* info,
                                  const VkAllocationCallbacks* alloc,
                                  VkBuffer* out)
{
        struct mock_object* obj = (struct mock_object*)mock_new_object();
        if (!out || !obj)
                return VK_ERROR_INITIALIZATION_FAILED;
        obj->size = (uint64_t)info->size;
        *out = (VkBuffer)obj;
        return VK_SUCCESS;
}

DEF_CREATE(CommandPool)
DEF_CREATE(DescriptorPool)
DEF_CREATE(DescriptorSetLayout)
DEF_CREATE(Fence)
DEF_CREATE(Framebuffer)
DEF_CREATE(Image)
DEF_CREATE(ImageView)
DEF_CREATE(PipelineLayout)
DEF_CREATE(RenderPass)
DEF_CREATE(Sampler)
DEF_CREATE(Semaphore)
DEF_CREATE(ShaderModule)

/*
 * CreateGraphicsPipelines takes a VkPipelineCreateInfo array; the layer
 * always creates exactly one pipeline per call.
 */
static VkResult mock_CreateGraphicsPipelines(VkDevice device,
                                             const VkGraphicsPipelineCreateInfo* info,
                                             const VkAllocationCallbacks* alloc,
                                             VkPipeline* pipelines)
{
        return mock_create_one(device, info, alloc, (void*)pipelines);
}

/*
 * Memory requirement stub: small size, trivial alignment, both of the
 * two advertised memory types available.
 */
static void mock_memory_requirements(uint64_t size,
                                     VkMemoryRequirements* req)
{
        req->size           = size ? size : 1024;
        req->alignment      = 1;
        req->memoryTypeBits = 0x3;
}

/*
 * Buffer memory requirements report the size recorded at CreateBuffer
 * time; the layer picks a memory type from memoryTypeBits, so both
 * advertised types must be set.
 */
static void mock_get_buffer_memory_requirements(VkDevice device,
                                                VkBuffer buffer,
                                                VkMemoryRequirements* req)
{
        (void)device;
        mock_memory_requirements(((struct mock_object*)buffer)->size, req);
}

/*
 * Image memory requirements stay fixed at 1024 so the crosshair upload
 * buffer (the only size-reflecting allocation) remains the largest single
 * device-memory allocation in the test.
 */
static void mock_get_image_memory_requirements(VkDevice device,
                                               VkImage image,
                                               VkMemoryRequirements* req)
{
        (void)device;
        (void)image;
        mock_memory_requirements(1024, req);
}

/*
 * Batch allocator for AllocateCommandBuffers / AllocateDescriptorSets:
 * fills every slot of the output array (the layer allocates draw-ring
 * command buffers and descriptor sets in bulk).
 */
static VkResult mock_allocate_command_buffers(
    VkDevice device, const VkCommandBufferAllocateInfo* info,
    VkCommandBuffer* buffers)
{
        (void)device;
        stats_cmdbufs += info->commandBufferCount;
        for (uint32_t i = 0; i < info->commandBufferCount; i++)
                buffers[i] = (VkCommandBuffer)mock_new_object();
        return VK_SUCCESS;
}

static VkResult mock_allocate_descriptor_sets(
    VkDevice device, const VkDescriptorSetAllocateInfo* info,
    VkDescriptorSet* sets)
{
        (void)device;
        for (uint32_t i = 0; i < info->descriptorSetCount; i++)
                sets[i] = (VkDescriptorSet)mock_new_object();
        return VK_SUCCESS;
}

static VkResult mock_allocate_memory(VkDevice device,
                                     const VkMemoryAllocateInfo* info,
                                     const VkAllocationCallbacks* alloc,
                                     VkDeviceMemory* memory)
{
        struct mock_object* obj;
        (void)device;
        (void)alloc;
        if (!memory)
                return VK_ERROR_INITIALIZATION_FAILED;
        obj = mock_new_object();
        if (!obj)
                return VK_ERROR_INITIALIZATION_FAILED;
        obj->size = info->allocationSize;
        if (info->allocationSize > stats_max_alloc)
                stats_max_alloc = info->allocationSize;
        *memory = (VkDeviceMemory)obj;
        return VK_SUCCESS;
}


/*
 * Shared destroy stub (vkDestroyX, vkFreeMemory): the second argument is
 * the handle being destroyed (not an allocator), so its backing object
 * is freed.
 */
static void mock_destroy_handle(VkDevice device, void* handle)
{
        (void)device;
        mock_free_object((struct mock_object*)handle);
}

/*
 * vkBindBufferMemory/vkBindImageMemory take (device, handle, memory,
 * offset, size); the mock has no real backing to bind, so the call is a
 * no-op.
 */
static void mock_bind_memory(VkDevice device, void* handle,
                             VkDeviceMemory memory, VkDeviceSize offset,
                             VkDeviceSize size)
{
        (void)device;
        (void)handle;
        (void)memory;
        (void)offset;
        (void)size;
}

/* Command recording / execution stubs (the layer only counts calls). */
static VkResult mock_begin_command_buffer(VkCommandBuffer cmd_buffer,
                                          const VkCommandBufferBeginInfo* info)
{
        (void)cmd_buffer;
        (void)info;
        return VK_SUCCESS;
}

static VkResult mock_end_command_buffer(VkCommandBuffer cmd_buffer)
{
        (void)cmd_buffer;
        return VK_SUCCESS;
}

static VkResult mock_reset_command_buffer(VkCommandBuffer cmd_buffer,
                                          VkCommandBufferResetFlags flags)
{
        (void)cmd_buffer;
        (void)flags;
        return VK_SUCCESS;
}

static void mock_cmd_noop(void) {}

static VkResult mock_wait_fences(VkDevice device, uint32_t fence_count,
                                 const VkFence* fences, VkBool32 wait_all,
                                 uint64_t timeout)
{
        (void)device;
        (void)fences;
        (void)wait_all;
        (void)timeout;
        return VK_SUCCESS;
}

static void mock_reset_fences(VkDevice device, uint32_t fence_count,
                              const VkFence* fences)
{
        (void)device;
        (void)fence_count;
        (void)fences;
}

static VkResult mock_queue_submit(VkDevice device, uint32_t submit_count,
                                  const VkSubmitInfo* submits, VkFence fence)
{
        (void)device;
        (void)fence;
        for (uint32_t i = 0; i < submit_count; i++) {
                stats_cmdbufs += (int)submits[i].commandBufferCount;
        }
        stats_submits++;
        return VK_SUCCESS;
}

static VkResult mock_queue_present(VkQueue queue,
                                   const VkPresentInfoKHR* present)
{
        (void)queue;
        /* pResults, if given, has one slot per swapchain (per spec) */
        if (present->pResults)
                for (uint32_t i = 0; i < present->swapchainCount; i++)
                        present->pResults[i] = VK_SUCCESS;
        stats_presents++;
        return VK_SUCCESS;
}

static VkResult mock_map_memory(VkDevice device, VkDeviceMemory memory,
                                VkDeviceSize offset, VkDeviceSize size,
                                VkFlags flags, void** pp_data)
{
        (void)device;
        (void)offset;
        (void)flags;
        size_t map_size = (size_t)size;
        if (map_size == (size_t)VK_WHOLE_SIZE) {
                /* the layer maps whole allocations; fall back to the
                 * size recorded at AllocateMemory time */
                struct mock_object* mem = (struct mock_object*)memory;
                map_size = (size_t)(mem && mem->size ? mem->size : 4096);
        }
        if (map_size == 0)
                map_size = 4096;
        /* the layer memcpy's vertex data into the mapping */
        struct mock_object* mem = (struct mock_object*)memory;
        if (pp_data && mem) {
                *pp_data = calloc(1, map_size);
                mem->mapped = *pp_data;
        }
        return VK_SUCCESS;
}

static void mock_unmap_memory(VkDevice device, VkDeviceMemory memory)
{
        (void)device;
        struct mock_object* mem = (struct mock_object*)memory;
        if (mem && mem->mapped) {
                free(mem->mapped);
                mem->mapped = NULL;
        }
}

static void mock_update_descriptor_sets(VkDevice device, uint32_t descriptor_write_count,
                                        const VkWriteDescriptorSet* p_descriptor_writes,
                                        uint32_t descriptor_copy_count,
                                        const VkCopyDescriptorSet* p_descriptor_copies)
{
        (void)device;
        (void)descriptor_write_count;
        (void)p_descriptor_writes;
        (void)descriptor_copy_count;
        (void)p_descriptor_copies;
}

/*
 * Swapchain support. CreateSwapchainKHR remembers the requested image
 * count; GetSwapchainImagesKHR answers the two-step (query, then fill)
 * pattern with fake VkImage handles.
 */
static VkResult mock_create_swapchain(VkDevice device,
                                      const VkSwapchainCreateInfoKHR* info,
                                      const VkAllocationCallbacks* alloc,
                                      VkSwapchainKHR* swapchain)
{
        stats_swapchains++;
        if (info->minImageCount)
                swapchain_image_count = info->minImageCount;
        return mock_create_one(device, info, alloc, (void*)swapchain);
}

static VkResult mock_get_swapchain_images(VkDevice device,
                                          VkSwapchainKHR swapchain,
                                          uint32_t* image_count,
                                          VkImage* images)
{
        (void)device;
        (void)swapchain;
        if (!images) {
                *image_count = swapchain_image_count;
                return VK_SUCCESS;
        }
        uint32_t n = *image_count;
        if (n > swapchain_image_count)
                n = swapchain_image_count;
        for (uint32_t i = 0; i < n; i++)
                images[i] = (VkImage)(uintptr_t)(0xBEEF0000 + i);
        return VK_SUCCESS;
}

static VkResult mock_acquire_next_image(VkDevice device,
                                        VkSwapchainKHR swapchain,
                                        uint64_t timeout, VkSemaphore semaphore,
                                        VkFence fence, uint32_t* image_index)
{
        (void)device;
        (void)swapchain;
        (void)timeout;
        (void)semaphore;
        (void)fence;
        stats_acquires++;
        *image_index = 0;
        return VK_SUCCESS;
}

static void mock_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                                   const VkAllocationCallbacks* alloc)
{
        (void)device;
        (void)alloc;
        mock_free_object((struct mock_object*)swapchain);
}

static VkResult mock_get_device_queue(VkDevice device,
                                      uint32_t family_index,
                                      uint32_t queue_index,
                                      VkQueue* queue)
{
        (void)device;
        (void)family_index;
        (void)queue_index;
        stats_queue_query++;
        *queue = MOCK_QUEUE;
        return VK_SUCCESS;
}

static VkResult mock_get_device_queue2(VkDevice device,
                                       const VkDeviceQueueInfo2* info,
                                       VkQueue* queue)
{
        return mock_get_device_queue(device, info->queueFamilyIndex,
                                     info->queueIndex, queue);
}

/* ------------------------------------------------------------------
 * Device-scoped entry points
 * ------------------------------------------------------------------ */

static PFN_vkVoidFunction mock_device_gpa(VkDevice device,
                                          const char* func_name)
{
        (void)device;
        if (!func_name)
                return NULL;

        if (!strcmp(func_name, "vkGetDeviceProcAddr"))
                return (PFN_vkVoidFunction)mock_device_gpa;
        if (!strcmp(func_name, "vkGetDeviceQueue"))
                return (PFN_vkVoidFunction)mock_get_device_queue;
        if (!strcmp(func_name, "vkGetDeviceQueue2"))
                return (PFN_vkVoidFunction)mock_get_device_queue2;
        if (!strcmp(func_name, "vkAllocateCommandBuffers"))
                return (PFN_vkVoidFunction)mock_allocate_command_buffers;
        if (!strcmp(func_name, "vkAllocateDescriptorSets"))
                return (PFN_vkVoidFunction)mock_allocate_descriptor_sets;
        if (!strcmp(func_name, "vkAllocateMemory"))
                return (PFN_vkVoidFunction)mock_allocate_memory;
        if (!strcmp(func_name, "vkBeginCommandBuffer"))
                return (PFN_vkVoidFunction)mock_begin_command_buffer;
        if (!strcmp(func_name, "vkBindBufferMemory"))
                return (PFN_vkVoidFunction)mock_bind_memory;
        if (!strcmp(func_name, "vkBindImageMemory"))
                return (PFN_vkVoidFunction)mock_bind_memory;
        if (!strcmp(func_name, "vkCmdBeginRenderPass"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdBindDescriptorSets"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdBindIndexBuffer"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdBindPipeline"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdBindVertexBuffers"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdCopyBufferToImage"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdCopyImage"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdDrawIndexed"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdEndRenderPass"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdPipelineBarrier"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdPushConstants"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdSetScissor"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCmdSetViewport"))
                return (PFN_vkVoidFunction)mock_cmd_noop;
        if (!strcmp(func_name, "vkCreateBuffer"))
                return (PFN_vkVoidFunction)mock_CreateBuffer;
        if (!strcmp(func_name, "vkCreateCommandPool"))
                return (PFN_vkVoidFunction)mock_CreateCommandPool;
        if (!strcmp(func_name, "vkCreateDescriptorPool"))
                return (PFN_vkVoidFunction)mock_CreateDescriptorPool;
        if (!strcmp(func_name, "vkCreateDescriptorSetLayout"))
                return (PFN_vkVoidFunction)mock_CreateDescriptorSetLayout;
        if (!strcmp(func_name, "vkCreateFence"))
                return (PFN_vkVoidFunction)mock_CreateFence;
        if (!strcmp(func_name, "vkCreateFramebuffer"))
                return (PFN_vkVoidFunction)mock_CreateFramebuffer;
        if (!strcmp(func_name, "vkCreateGraphicsPipelines"))
                return (PFN_vkVoidFunction)mock_CreateGraphicsPipelines;
        if (!strcmp(func_name, "vkCreateImage"))
                return (PFN_vkVoidFunction)mock_CreateImage;
        if (!strcmp(func_name, "vkCreateImageView"))
                return (PFN_vkVoidFunction)mock_CreateImageView;
        if (!strcmp(func_name, "vkCreatePipelineLayout"))
                return (PFN_vkVoidFunction)mock_CreatePipelineLayout;
        if (!strcmp(func_name, "vkCreateRenderPass"))
                return (PFN_vkVoidFunction)mock_CreateRenderPass;
        if (!strcmp(func_name, "vkCreateSampler"))
                return (PFN_vkVoidFunction)mock_CreateSampler;
        if (!strcmp(func_name, "vkCreateSemaphore"))
                return (PFN_vkVoidFunction)mock_CreateSemaphore;
        if (!strcmp(func_name, "vkCreateShaderModule"))
                return (PFN_vkVoidFunction)mock_CreateShaderModule;
        if (!strcmp(func_name, "vkCreateSwapchainKHR"))
                return (PFN_vkVoidFunction)mock_create_swapchain;
        if (!strcmp(func_name, "vkDestroyBuffer"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkDestroyDevice"))
                return (PFN_vkVoidFunction)mock_destroy_device;
        if (!strcmp(func_name, "vkDestroyFence"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkDestroyFramebuffer"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkDestroyImage"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkDestroyImageView"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkDestroyPipeline"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkDestroyRenderPass"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkDestroySemaphore"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkDestroyShaderModule"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkDestroySwapchainKHR"))
                return (PFN_vkVoidFunction)mock_destroy_swapchain;
        if (!strcmp(func_name, "vkDeviceWaitIdle"))
                return (PFN_vkVoidFunction)mock_wait_fences;
        if (!strcmp(func_name, "vkEndCommandBuffer"))
                return (PFN_vkVoidFunction)mock_end_command_buffer;
        if (!strcmp(func_name, "vkFlushMappedMemoryRanges"))
                return (PFN_vkVoidFunction)mock_wait_fences;
        if (!strcmp(func_name, "vkFreeCommandBuffers"))
                return (PFN_vkVoidFunction)mock_reset_fences;
        if (!strcmp(func_name, "vkFreeDescriptorSets"))
                return (PFN_vkVoidFunction)mock_reset_fences;
        if (!strcmp(func_name, "vkFreeMemory"))
                return (PFN_vkVoidFunction)mock_destroy_handle;
        if (!strcmp(func_name, "vkGetBufferMemoryRequirements"))
                return (PFN_vkVoidFunction)mock_get_buffer_memory_requirements;
        if (!strcmp(func_name, "vkGetImageMemoryRequirements"))
                return (PFN_vkVoidFunction)mock_get_image_memory_requirements;
        if (!strcmp(func_name, "vkGetSwapchainImagesKHR"))
                return (PFN_vkVoidFunction)mock_get_swapchain_images;
        if (!strcmp(func_name, "vkAcquireNextImageKHR"))
                return (PFN_vkVoidFunction)mock_acquire_next_image;
        if (!strcmp(func_name, "vkMapMemory"))
                return (PFN_vkVoidFunction)mock_map_memory;
        if (!strcmp(func_name, "vkUnmapMemory"))
                return (PFN_vkVoidFunction)mock_unmap_memory;
        if (!strcmp(func_name, "vkQueuePresentKHR"))
                return (PFN_vkVoidFunction)mock_queue_present;
        if (!strcmp(func_name, "vkQueueSubmit"))
                return (PFN_vkVoidFunction)mock_queue_submit;
        if (!strcmp(func_name, "vkResetCommandBuffer"))
                return (PFN_vkVoidFunction)mock_reset_command_buffer;
        if (!strcmp(func_name, "vkResetFences"))
                return (PFN_vkVoidFunction)mock_reset_fences;
        if (!strcmp(func_name, "vkUpdateDescriptorSets"))
                return (PFN_vkVoidFunction)mock_update_descriptor_sets;
        if (!strcmp(func_name, "vkWaitForFences"))
                return (PFN_vkVoidFunction)mock_wait_fences;
        fprintf(stderr, "[mock-icd] device gpa miss: %s\n", func_name);
        return NULL;
}

/* ------------------------------------------------------------------
 * Instance-scoped entry points
 * ------------------------------------------------------------------ */

static VkResult mock_create_instance(const VkInstanceCreateInfo* create_info,
                                     const VkAllocationCallbacks* allocator,
                                     VkInstance* instance)
{
        (void)create_info;
        (void)allocator;
        *instance = MOCK_INSTANCE;
        return VK_SUCCESS;
}

static void mock_destroy_instance(VkInstance instance,
                                  const VkAllocationCallbacks* allocator)
{
        (void)instance;
        (void)allocator;
}

static VkResult mock_enumerate_instance_extensions(
    const char* layer_name, uint32_t* property_count,
    VkExtensionProperties* properties)
{
        (void)layer_name;
        if (!properties) {
                *property_count = 0;
                return VK_SUCCESS;
        }
        *property_count = 0;
        return VK_SUCCESS;
}

static VkResult mock_enumerate_instance_layers(uint32_t* property_count,
                                               VkLayerProperties* properties)
{
        (void)properties;
        if (!properties)
                *property_count = 0;
        return VK_SUCCESS;
}

static VkResult mock_enumerate_physical_devices(VkInstance instance,
                                                uint32_t* count,
                                                VkPhysicalDevice* devices)
{
        (void)instance;
        if (!devices) {
                *count = 1;
                return VK_SUCCESS;
        }
        if (*count >= 1)
                devices[0] = MOCK_PHYS;
        *count = 1;
        return VK_SUCCESS;
}

static void mock_get_physical_device_properties(VkPhysicalDevice physical_device,
                                                VkPhysicalDeviceProperties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
        properties->apiVersion    = VK_MAKE_VERSION(1, 3, 0);
        properties->driverVersion = 1;
        properties->vendorID      = 0x1234;
        properties->deviceID      = 0x4321;
        properties->deviceType    = VK_PHYSICAL_DEVICE_TYPE_CPU;
        snprintf(properties->deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE,
                 "Mock ICD");

        VkPhysicalDeviceLimits* limits = &properties->limits;
        limits->maxImageDimension2D          = 4096;
        limits->maxImageDimension3D          = 512;
        limits->maxImageDimensionCube        = 4096;
        limits->maxImageArrayLayers          = 256;
        limits->minUniformBufferOffsetAlignment = 256;
        limits->nonCoherentAtomSize          = 64;
        limits->maxVertexInputAttributes     = 16;
        limits->maxVertexInputBindingStride  = 2048;
        limits->maxDrawIndirectCount         = 894784848;
        limits->maxBoundDescriptorSets       = 8;
        limits->maxPerStageDescriptorSamplers = 16;
        limits->maxPerStageDescriptorUniformBuffers = 16;
        limits->maxPerStageDescriptorStorageBuffers = 16;
        limits->maxPerStageDescriptorSampledImages  = 16;
        limits->maxDescriptorSetSamplers       = 16;
        limits->maxDescriptorSetUniformBuffers = 16;
        limits->maxDescriptorSetStorageBuffers = 16;
        limits->maxDescriptorSetSampledImages  = 16;
        limits->maxComputeWorkGroupCount[0]    = 65536;
        limits->maxComputeWorkGroupCount[1]    = 65536;
        limits->maxComputeWorkGroupCount[2]    = 65536;
        limits->maxComputeWorkGroupSize[0]     = 64;
        limits->maxComputeWorkGroupSize[1]     = 64;
        limits->maxComputeWorkGroupSize[2]     = 64;
        limits->maxComputeWorkGroupInvocations = 256;
}

static void mock_get_physical_device_features(VkPhysicalDevice physical_device,
                                              VkPhysicalDeviceFeatures* features)
{
        (void)physical_device;
        memset(features, 0, sizeof(*features));
}

static void mock_get_physical_device_queue_family_properties(
    VkPhysicalDevice physical_device, uint32_t* count,
    VkQueueFamilyProperties* properties)
{
        (void)physical_device;
        if (!properties) {
                *count = 1;
                return;
        }
        memset(properties, 0, sizeof(*properties));
        properties[0].queueFlags            = VK_QUEUE_GRAPHICS_BIT |
                                               VK_QUEUE_COMPUTE_BIT |
                                               VK_QUEUE_TRANSFER_BIT;
        properties[0].queueCount            = 1;
        properties[0].timestampValidBits    = 0;
        properties[0].minImageTransferGranularity = (VkExtent3D){ 8, 8, 1 };
        *count = 1;
}

static void mock_get_physical_device_memory_properties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
        properties->memoryTypeCount = 2;
        properties->memoryTypes[0].propertyFlags =
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        properties->memoryTypes[0].heapIndex = 0;
        properties->memoryTypes[1].propertyFlags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        properties->memoryTypes[1].heapIndex = 0;
        properties->memoryHeapCount = 1;
        properties->memoryHeaps[0].size  = 1ULL << 30;
        properties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

/*
 * The remaining core vkGetPhysicalDevice* queries. The loader resolves a
 * fixed set of "required entry points" from the ICD's instance proc-addr
 * table at CreateInstance time and skips the driver if any is missing, so a
 * 1.3 ICD must answer all of them; only Properties/Features/QueueFamily/
 * MemoryProperties carry data the layer actually reads, the rest are
 * zero-filled stubs.
 */
static void mock_get_physical_device_features2(VkPhysicalDevice physical_device,
                                               VkPhysicalDeviceFeatures2* features)
{
        (void)physical_device;
        memset(features, 0, sizeof(*features));
}

static void mock_get_physical_device_format_properties(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatProperties* properties)
{
        (void)physical_device;
        (void)format;
        memset(properties, 0, sizeof(*properties));
}

static void mock_get_physical_device_format_properties2(
    VkPhysicalDevice physical_device, VkFormatProperties2* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
}

/*
 * 1.0-style scalar query (this header has no VkImageFormatInfo struct).
 */
static VkResult mock_get_physical_device_image_format_properties(
    VkPhysicalDevice physical_device, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties* properties)
{
        (void)physical_device;
        (void)format;
        (void)type;
        (void)tiling;
        (void)usage;
        (void)flags;
        memset(properties, 0, sizeof(*properties));
        properties->maxExtent = (VkExtent3D){ 4096, 4096, 4096 };
        return VK_SUCCESS;
}

static VkResult mock_get_physical_device_image_format_properties2(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceImageFormatInfo2* info,
    VkImageFormatProperties2* properties)
{
        (void)physical_device;
        (void)info;
        memset(properties, 0, sizeof(*properties));
        properties->imageFormatProperties.maxExtent =
            (VkExtent3D){ 4096, 4096, 4096 };
        return VK_SUCCESS;
}

static void mock_get_physical_device_memory_properties2(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties2* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
        mock_get_physical_device_memory_properties(
            physical_device, &properties->memoryProperties);
}

static void mock_get_physical_device_properties2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceProperties2* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
        mock_get_physical_device_properties(physical_device, &properties->properties);
}

static void mock_get_physical_device_queue_family_properties2(
    VkPhysicalDevice physical_device, uint32_t* count,
    VkQueueFamilyProperties2* properties)
{
        uint32_t plain_count = 0;
        if (!properties) {
                mock_get_physical_device_queue_family_properties(
                    physical_device, &plain_count, NULL);
                *count = plain_count;
                return;
        }
        uint32_t n = *count;
        if (n > 1)
                n = 1;
        for (uint32_t i = 0; i < n; i++) {
                memset(&properties[i], 0, sizeof(properties[i]));
                mock_get_physical_device_queue_family_properties(
                    physical_device, &plain_count, &properties[i].queueFamilyProperties);
        }
        *count = n;
}

/*
 * The 1.0 vkGetPhysicalDeviceSparseImageFormatInfo/
 * VkSparseImageFormatInfo pair is absent from this header; declared
 * minimally here (layout-compatible) so the loader's entry-point probe
 * resolves.
 */
struct mock_sparse_image_format_info {
        VkImageType imageType;
        VkFormat format;
        VkImageUsageFlags usage;
        VkSparseImageFormatFlags flags;
        VkExtent3D blockSize;
};

static void mock_get_physical_device_sparse_image_format_info(
    VkPhysicalDevice physical_device, uint32_t* format_count,
    struct mock_sparse_image_format_info* properties)
{
        (void)physical_device;
        (void)properties;
        *format_count = 0;
}

static void mock_get_physical_device_sparse_image_format_properties(
    VkPhysicalDevice physical_device, uint32_t* property_count,
    VkSparseImageFormatProperties* properties)
{
        (void)physical_device;
        (void)properties;
        *property_count = 0;
}

static void mock_get_physical_device_subgroup_properties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceSubgroupProperties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
        properties->subgroupSize = 32;
}

static void mock_get_physical_device_subgroup_size_control_properties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceSubgroupSizeControlProperties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
        properties->minSubgroupSize   = 1;
        properties->maxSubgroupSize   = 128;
        properties->maxComputeWorkgroupSubgroups = 64;
}

static void mock_get_physical_device_timeline_semaphore_properties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceTimelineSemaphoreProperties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
        properties->maxTimelineSemaphoreValueDifference =
            0xFFFFFFFFFFFFFFFFull;
}

static VkResult mock_get_physical_device_tool_properties(
    VkPhysicalDevice physical_device, uint32_t* property_count,
    VkPhysicalDeviceToolProperties* properties)
{
        (void)physical_device;
        (void)properties;
        *property_count = 0;
        return VK_SUCCESS;
}

static void mock_get_physical_device_vulkan11_properties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceVulkan11Properties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
}

static void mock_get_physical_device_vulkan12_properties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceVulkan12Properties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
}

static void mock_get_physical_device_vulkan13_properties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceVulkan13Properties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
}

static void mock_get_physical_device_protected_memory_properties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceProtectedMemoryProperties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
}

/*
 * The core vkGetPhysicalDeviceExternalImageFormatProperties function is
 * absent from this header (only the NV flavor); declared with the modern
 * VkImageFormatProperties* out-param.
 */
static void mock_get_physical_device_external_image_format_properties(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceExternalImageFormatInfo* info,
    VkImageFormatProperties* properties)
{
        (void)physical_device;
        (void)info;
        memset(properties, 0, sizeof(*properties));
}

/* The 1.3 VkPhysicalDeviceCustomizableTimelineSemaphoreProperties struct
 * is absent from this header; declared minimally. */
struct mock_customizable_timeline_semaphore_properties {
        VkStructureType sType;
        void* pNext;
        uint64_t maxTimelineSemaphoreValueDifference;
};

static void mock_get_physical_device_customizable_timeline_semaphore_properties(
    VkPhysicalDevice physical_device,
    struct mock_customizable_timeline_semaphore_properties* properties)
{
        (void)physical_device;
        memset(properties, 0, sizeof(*properties));
        properties->maxTimelineSemaphoreValueDifference =
            0xFFFFFFFFFFFFFFFFull;
}

/*
 * vkEnumerateInstanceVersion: mandatory for any ICD whose manifest declares
 * an api_version of 1.1 or greater; the loader treats a failing ICD as 1.0.
 */
static VkResult mock_enumerate_instance_version(uint32_t* version)
{
        *version = VK_MAKE_VERSION(1, 3, 0);
        return VK_SUCCESS;
}

static VkResult mock_enumerate_device_extensions(
    VkPhysicalDevice physical_device, const char* layer_name,
    uint32_t* property_count, VkExtensionProperties* properties)
{
        (void)physical_device;
        (void)layer_name;
        fprintf(stderr, "[mock-icd] enumerate_device_extensions: count query=%d\n",
                properties == NULL);
        if (!properties) {
                *property_count = 1;
                fprintf(stderr, "[mock-icd] enumerate_device_extensions: returning SUCCESS, count=1\n");
                return VK_SUCCESS;
        }
        if (*property_count >= 1) {
                strcpy(properties[0].extensionName, "VK_KHR_swapchain");
                properties[0].specVersion   = 1;
                *property_count             = 1;
                return VK_SUCCESS;
        }
        *property_count = 0;
        return VK_SUCCESS;
}

static VkResult mock_create_device(VkPhysicalDevice physical_device,
                                   const VkDeviceCreateInfo* create_info,
                                   const VkAllocationCallbacks* allocator,
                                   VkDevice* device)
{
        (void)physical_device;
        (void)create_info;
        (void)allocator;
        *device = MOCK_DEVICE;
        return VK_SUCCESS;
}

/*
 * The mock's device object is static, so there is nothing to free here.
 * (This loader generation does not even invoke the ICD's DestroyDevice;
 * the device-scoped cleanup lives in mock_icd_cleanup below.)
 */
static void mock_destroy_device(VkDevice device,
                                const VkAllocationCallbacks* allocator)
{
        (void)device;
        (void)allocator;
}

/*
 * The Vulkan spec lets a device destroy implicitly free every device-
 * scoped object it created (except VkDeviceMemory and friends); a real
 * driver does that in its own DestroyDevice. This loader generation
 * never calls the ICD's DestroyDevice, so the equivalent cleanup runs
 * when the mock is unloaded: free every backing object that survived
 * (the layer's device-scoped stable resources and any command buffers
 * it did not free explicitly). Objects already released by their
 * destroy or FreeMemory path are off the list and skipped.
 */
__attribute__((destructor))
static void mock_icd_cleanup(void)
{
        struct mock_object* obj = g_object_list;
        while (obj) {
                struct mock_object* next = obj->next;
                if (obj->mapped)
                        free(obj->mapped);
                free(obj);
                obj = next;
        }
        g_object_list = NULL;
}

/*
 * Physical device extension proc-addr (loader-ICD interface v7+): the
 * loader resolves device-scope extension entry points (surface creation
 * and friends) through this instead of the instance table. The mock
 * advertises no physical device extensions, so nothing resolves.
 */
static PFN_vkVoidFunction mock_icd_get_physical_device_proc_addr(
    VkPhysicalDevice physical_device, const char* func_name)
{
        (void)physical_device;
        if (func_name)
                fprintf(stderr, "[mock-icd] pd gpa miss: %s\n", func_name);
        return NULL;
}

/*
 * The ICD instance proc-addr entry point. Under the current loader-ICD
 * interface the loader dlsym's this symbol directly and resolves every
 * other instance-scope command through it.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char* func_name)
{
        (void)instance;
        if (!func_name)
                return NULL;

        if (!strcmp(func_name, "vkGetInstanceProcAddr"))
                return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
        if (!strcmp(func_name, "vk_icdGetInstanceProcAddr"))
                return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
        if (!strcmp(func_name, "vk_icdNegotiateLoaderICDInterfaceVersion"))
                return (PFN_vkVoidFunction)
                    vk_icdNegotiateLoaderICDInterfaceVersion;
        if (!strcmp(func_name, "vk_icdGetPhysicalDeviceProcAddr"))
                return (PFN_vkVoidFunction)
                    mock_icd_get_physical_device_proc_addr;
        if (!strcmp(func_name, "vkGetDeviceProcAddr"))
                return (PFN_vkVoidFunction)mock_device_gpa;
        if (!strcmp(func_name, "vkCreateInstance"))
                return (PFN_vkVoidFunction)mock_create_instance;
        if (!strcmp(func_name, "vkDestroyInstance"))
                return (PFN_vkVoidFunction)mock_destroy_instance;
        if (!strcmp(func_name, "vkEnumerateInstanceExtensionProperties"))
                return (PFN_vkVoidFunction)mock_enumerate_instance_extensions;
        if (!strcmp(func_name, "vkEnumerateInstanceLayerProperties"))
                return (PFN_vkVoidFunction)mock_enumerate_instance_layers;
        if (!strcmp(func_name, "vkEnumerateInstanceVersion"))
                return (PFN_vkVoidFunction)mock_enumerate_instance_version;
        if (!strcmp(func_name, "vkEnumeratePhysicalDevices"))
                return (PFN_vkVoidFunction)mock_enumerate_physical_devices;
        if (!strcmp(func_name, "vkGetPhysicalDeviceProperties"))
                return (PFN_vkVoidFunction)mock_get_physical_device_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceProperties2"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_properties2;
        if (!strcmp(func_name, "vkGetPhysicalDeviceFeatures"))
                return (PFN_vkVoidFunction)mock_get_physical_device_features;
        if (!strcmp(func_name, "vkGetPhysicalDeviceFeatures2"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_features2;
        if (!strcmp(func_name, "vkGetPhysicalDeviceQueueFamilyProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_queue_family_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceQueueFamilyProperties2"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_queue_family_properties2;
        if (!strcmp(func_name, "vkGetPhysicalDeviceMemoryProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_memory_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceMemoryProperties2"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_memory_properties2;
        if (!strcmp(func_name, "vkGetPhysicalDeviceFormatProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_format_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceFormatProperties2"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_format_properties2;
        if (!strcmp(func_name, "vkGetPhysicalDeviceImageFormatProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_image_format_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceImageFormatProperties2"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_image_format_properties2;
        if (!strcmp(func_name, "vkGetPhysicalDeviceSparseImageFormatInfo"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_sparse_image_format_info;
        if (!strcmp(func_name,
                    "vkGetPhysicalDeviceSparseImageFormatProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_sparse_image_format_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceSubgroupProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_subgroup_properties;
        if (!strcmp(func_name,
                    "vkGetPhysicalDeviceSubgroupSizeControlProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_subgroup_size_control_properties;
        if (!strcmp(func_name,
                    "vkGetPhysicalDeviceTimelineSemaphoreProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_timeline_semaphore_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceToolProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_tool_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceVulkan11Properties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_vulkan11_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceVulkan12Properties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_vulkan12_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceVulkan13Properties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_vulkan13_properties;
        if (!strcmp(func_name, "vkGetPhysicalDeviceProtectedMemoryProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_protected_memory_properties;
        if (!strcmp(func_name,
                    "vkGetPhysicalDeviceExternalImageFormatProperties"))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_external_image_format_properties;
        if (!strcmp(func_name,
                    "vkGetPhysicalDeviceCustomizableTimelineSemaphoreProperties"
                    ))
                return (PFN_vkVoidFunction)
                    mock_get_physical_device_customizable_timeline_semaphore_properties;
        if (!strcmp(func_name, "vkEnumerateDeviceExtensionProperties"))
                return (PFN_vkVoidFunction)mock_enumerate_device_extensions;
        if (!strcmp(func_name, "vkCreateDevice"))
                return (PFN_vkVoidFunction)mock_create_device;
        if (!strcmp(func_name, "vkDestroyDevice"))
                return (PFN_vkVoidFunction)mock_destroy_device;
        fprintf(stderr, "[mock-icd] instance gpa miss: %s\n", func_name);
        return NULL;
}

/* ------------------------------------------------------------------
 * Loader ICD negotiation
 * ------------------------------------------------------------------ */

/*
 * Loader-ICD interface version negotiation (interface v4+, vk_icd.h).
 * The loader dlsym's this before loading the ICD and then dlsym's
 * vk_icdGetInstanceProcAddr to resolve everything else.
 *
 * pVersion  In: the version the loader requests. Out: the version both
 *           sides agreed on (never above what this ICD implements).
 */
VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion)
{
        uint32_t requested = *pVersion;
        if (requested > CURRENT_LOADER_ICD_INTERFACE_VERSION)
                *pVersion = CURRENT_LOADER_ICD_INTERFACE_VERSION;
        else
                *pVersion = requested;
        fprintf(stderr,
                "[mock-icd] negotiate: loader wants v%u, ICD offers v%u\n",
                requested, *pVersion);
        return VK_SUCCESS;
}
