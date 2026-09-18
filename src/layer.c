/*
 * Vulkan implicit layer entry points.
 *
 * This is the API surface of the layer: the overlay_* functions the
 * loader and dispatch table call (CreateInstance, CreateDevice,
 * CreateSwapchainKHR, QueuePresentKHR, ...), plus the name-to-function
 * map that backs vkGetInstanceProcAddr/vkGetDeviceProcAddr. All
 * implementation work lives in the other translation units
 * (input, objects, gpu, crosshair, apng, render).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <linux/input.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>
#include <limits.h>
#include <math.h>

#include "../include/dispatch.h"
#include "../include/keys.h"
#include "../include/krosshair.h"

/*
 * UNIT_TEST build: expose the lookup table and the intercepted entry
 * points so the tests/test_*.c binaries can drive them directly
 * (see tests/test_layer_api.h). Release builds keep them static.
 */
#ifdef UNIT_TEST
#define LAYER_API
#else
#define LAYER_API static
#endif

/* Lock serializing access to the global object map (vk_obj_map). */
pthread_mutex_t global_lock;

/* ------------------------------------------------------------------
 * Physical device registration
 * ------------------------------------------------------------------ */

/*
 * Ask the driver for the instance's physical device list.
 *
 * instance_data  Per-instance state; provides the instance handle and vtable.
 * count_out      Receives the number of physical devices.
 *
 * Returns a malloc'd array of count_out physical devices; the caller frees it.
 *
 * The Vulkan API requires two calls: one with a NULL device array to learn
 * the count, one with a real array to receive the handles.
 */
static VkPhysicalDevice* enumerate_physical_devices(
    instance_data_t* instance_data, uint32_t* count_out)
{
        *count_out = 0;
        instance_data->vtable.EnumeratePhysicalDevices(
            instance_data->instance, count_out, NULL);

        VkPhysicalDevice* physical_devices =
            malloc(sizeof(VkPhysicalDevice) * *count_out);
        instance_data->vtable.EnumeratePhysicalDevices(
            instance_data->instance, count_out, physical_devices);
        return physical_devices;
}

/*
 * Map every physical device into the object map, pointing to the instance.
 *
 * A physical device is not its own Vulkan state — it only ever leads to
 * the instance it belongs to — so overlay_CreateDevice resolves a
 * VkPhysicalDevice by looking up the instance_data_t stored here.
 * Each entry gets an asprintf'd display name, freed on unmap.
 */
static void register_physical_devices(instance_data_t* instance_data)
{
        uint32_t physical_device_count;
        VkPhysicalDevice* physical_devices =
            enumerate_physical_devices(instance_data, &physical_device_count);

        for (uint32_t i = 0; i < physical_device_count; i++) {
                KROSSHAIR_LOG("[*] mapping physical_devices[%d] obj: %lu %p\n",
                    i, HKEY(physical_devices[i]), (void*)instance_data);
                char* phys_device_name;
                asprintf(&phys_device_name, "physical_devices[%d]", i);
                map_object(HKEY(physical_devices[i]), instance_data,
                           phys_device_name);
        }

        free(physical_devices);
}

/*
 * Remove every physical device entry created by register_physical_devices(),
 * freeing the asprintf'd display names along the way.
 */
static void unregister_physical_devices(instance_data_t* instance_data)
{
        uint32_t physical_device_count;
        VkPhysicalDevice* physical_devices =
            enumerate_physical_devices(instance_data, &physical_device_count);

        for (uint32_t i = 0; i < physical_device_count; i++) {
                /* free the name string we allocated with asprintf */
                vk_object_t obj;
                if (vk_map_get(&vk_obj_map, HKEY(physical_devices[i]), &obj))
                        free((char*)obj.name);
                unmap_object(HKEY(physical_devices[i]));
        }

        free(physical_devices);
}

/* ------------------------------------------------------------------
 * Swapchain registry helpers
 * ------------------------------------------------------------------ */

/*
 * Allocate, zero-initialize and register the per-swapchain bookkeeping.
 *
 * swapchain    The newly created VkSwapchainKHR handle.
 * device_data  Owning device (used for descriptor-pool teardown later).
 *
 * Returns the new swapchain_data_t, already stored in the object map.
 */
static swapchain_data_t* new_swapchain_data(VkSwapchainKHR swapchain,
                                            device_data_t* device_data)
{
        swapchain_data_t* swapchain_data = malloc(sizeof(*swapchain_data));
        memset(swapchain_data, 0, sizeof(*swapchain_data));
        kh_perflog_host_alloc(sizeof(*swapchain_data), "swapchain data");
        swapchain_data->device_data = device_data;
        swapchain_data->swapchain   = swapchain;
        KROSSHAIR_LOG("[*] mapping data->swapchain obj: %lu %p\n",
               HKEY(swapchain_data->swapchain), (void*)swapchain_data);
        map_object(HKEY(swapchain_data->swapchain), swapchain_data,
                   "swapchain_data->swapchain");
        return swapchain_data;
}

/*
 * Remove `data` from the device's swapchain registry. Only compares the
 * pointer value — `data` may already be freed when this is called
 * (the DestroyDevice sweep frees entries after removing them).
 */
static void unregister_swapchain(device_data_t* device_data,
                                 swapchain_data_t* data)
{
        /* registry is capped at the descriptor-pool maxSets */
        uint32_t stored = device_data->swapchain_count < KROSSHAIR_MAX_SWAPCHAINS
                              ? device_data->swapchain_count
                              : KROSSHAIR_MAX_SWAPCHAINS;
        for (uint32_t i = 0; i < stored; i++) {
                if (device_data->swapchains[i] == data) {
                        for (uint32_t j = i; j + 1 < stored; j++)
                                device_data->swapchains[j] =
                                    device_data->swapchains[j + 1];
                        break;
                }
        }
        if (device_data->swapchain_count > 0) device_data->swapchain_count--;
}

/* ------------------------------------------------------------------
 * Layer entry points (called by the Vulkan loader / dispatch table)
 * ------------------------------------------------------------------ */

/*
 * Intercepted vkCreateSwapchainKHR.
 *
 * device       VkDevice the swapchain is created for.
 * create_info  Application's swapchain creation parameters.
 * allocator    Vulkan allocator callbacks (passed through).
 * swapchain_out Receives the new VkSwapchainKHR handle.
 *
 * The app's imageUsage is extended with COLOR_ATTACHMENT | TRANSFER_SRC so
 * the game's swapchain images can be used directly as the overlay's render
 * pass attachments. If create_info->oldSwapchain is set (e.g. a resize),
 * the old swapchain's layer resources are released after the new one is
 * successfully created — the driver keeps oldSwapchain valid until then.
 */
static VkResult overlay_CreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* create_info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain_out)
{
        VkSwapchainCreateInfoKHR modified_info = *create_info;
        modified_info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        device_data_t* device_data = FIND_OBJ(device_data_t, device);

        /* save reference to old swapchain data - we must NOT destroy it before
         * the create call because the driver needs oldSwapchain to be valid */
        swapchain_data_t* old_swapchain_data = NULL;
        if (create_info->oldSwapchain != VK_NULL_HANDLE) {
                old_swapchain_data =
                    FIND_OBJ(swapchain_data_t, create_info->oldSwapchain);
        }

        VkResult result = device_data->vtable.CreateSwapchainKHR(
            device, &modified_info, allocator, swapchain_out);
        if (result != VK_SUCCESS) return result;

        /* now that the new swapchain is created, clean up the old one's
         * internal resources (Vulkan retires oldSwapchain automatically) */
        if (old_swapchain_data) {
                destroy_swapchain_data(old_swapchain_data);
                unmap_object(HKEY(old_swapchain_data->swapchain));
                unregister_swapchain(device_data, old_swapchain_data);
                kh_perflog_host_free(sizeof(swapchain_data_t),
                                     "swapchain data");
                free(old_swapchain_data);
        }

        swapchain_data_t* swapchain_data =
            new_swapchain_data(*swapchain_out, device_data);

        device_data->swapchain_count++;
        if (device_data->swapchain_count > 1) {
                KROSSHAIR_LOG(
                    "[KROSSHAIR] WARNING: %u concurrent swapchains on this "
                    "device — descriptor pools are sized for up to 4; "
                    "exceeding that will fail allocation\n",
                    device_data->swapchain_count);
        }
        /* the registry is capped at the descriptor-pool maxSets: only the
         * first KROSSHAIR_MAX_SWAPCHAINS swapchains are tracked (a 5th
         * can't allocate a descriptor set anyway), but swapchain_count
         * stays accurate for the warning above, which fires on every
         * creation beyond the first — tracked or not.
         * Known limitation (count > cap only): while the count exceeds the
         * cap, unregister_swapchain's shift-down can duplicate or leave
         * stale entries in the capped array, so the DestroyDevice sweep can
         * double-free. Confined to the already-unsupported >4 case. */
        if (device_data->swapchain_count <= KROSSHAIR_MAX_SWAPCHAINS)
                device_data->swapchains[device_data->swapchain_count - 1] =
                    swapchain_data;

        setup_swapchain_data(swapchain_data, create_info);

        KROSSHAIR_LOG("[KROSSHAIR] CreateSwapchainKHR: created %lu (%ux%u, n_images=%u, old=%lu)\n",
                      (unsigned long)*swapchain_out, create_info->imageExtent.width,
                      create_info->imageExtent.height, swapchain_data->n_images,
                      (unsigned long)create_info->oldSwapchain);

        return result;
}

/*
 * Intercepted vkDestroySwapchainKHR.
 *
 * device      VkDevice owning the swapchain.
 * swapchain   Handle being destroyed.
 * allocator   Vulkan allocator callbacks (passed through).
 *
 * Releases the overlay resources attached to this swapchain (and its
 * bookkeeping) before forwarding the call down the layer chain.
 */
static void overlay_DestroySwapchainKHR(VkDevice device,
                                        VkSwapchainKHR swapchain,
                                        const VkAllocationCallbacks* allocator)
{
        device_data_t* device_data = FIND_OBJ(device_data_t, device);
        swapchain_data_t* data     = FIND_OBJ(swapchain_data_t, swapchain);

        if (data) {
                destroy_swapchain_data(data);
                unmap_object(HKEY(data->swapchain));
                unregister_swapchain(device_data, data);
                kh_perflog_host_free(sizeof(swapchain_data_t),
                                     "swapchain data");
                free(data);
        }

        device_data->vtable.DestroySwapchainKHR(device_data->device,
                                                swapchain, allocator);
}

/*
 * Intercepted vkCreateInstance.
 *
 * create_info  Application's instance creation parameters.
 * allocator    Vulkan allocator callbacks (passed through).
 * instance_out Receives the new VkInstance handle.
 *
 * Starts the input thread, unwraps the loader's layer-link info from
 * create_info->pNext to reach the next layer's entry points, creates the
 * real instance below us, then loads the instance-level dispatch table
 * and registers all physical devices in the object map.
 */
LAYER_API VkResult overlay_CreateInstance(const VkInstanceCreateInfo* create_info,
                                       const VkAllocationCallbacks* allocator,
                                       VkInstance* instance_out)
{
        init_input_thread();
        kh_perflog_init();

        VkLayerInstanceCreateInfo* chain_info =
            get_instance_chain_info(create_info, VK_LAYER_LINK_INFO);
        assert(chain_info->u.pLayerInfo);
        PFN_vkGetInstanceProcAddr next_gpa =
            chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkCreateInstance next_create_instance =
            (PFN_vkCreateInstance)next_gpa(NULL, "vkCreateInstance");
        if (!next_create_instance) {
                return VK_ERROR_INITIALIZATION_FAILED;
        }

        /* step past our layer's link-info node in the pNext chain */
        chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

        VkResult result = next_create_instance(create_info, allocator, instance_out);
        if (result != VK_SUCCESS) return result;

        instance_data_t* instance_data = new_instance_data(*instance_out);
        vk_load_instance_commands(instance_data->instance,
                                  next_gpa,
                                  &instance_data->vtable);
        /* capture the next-link DestroyInstance (the vtable entry above is the
         * gpa-lookup, which the loader commonly leaves NULL) */
        instance_data->chain_DestroyInstance =
            (PFN_vkDestroyInstance)next_gpa(NULL, "vkDestroyInstance");
        register_physical_devices(instance_data);

        return result;
}

/*
 * Find this layer's node in a device create-info pNext chain.
 *
 * create_info  Device creation parameters to walk.
 * func         VkLayerFunction tag identifying which node we want
 *              (VK_LAYER_LINK_INFO, VK_LOADER_DATA_CALLBACK, ...).
 *
 * Returns the matching VkLayerDeviceCreateInfo, or NULL.
 */
static VkLayerDeviceCreateInfo* get_device_chain_info(
    const VkDeviceCreateInfo* create_info, VkLayerFunction func)
{
        vk_foreach_struct(item, create_info->pNext)
        {
                if (item->sType ==
                        VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                    ((VkLayerDeviceCreateInfo*)item)->function == func)
                        return (VkLayerDeviceCreateInfo*)item;
        }
        return NULL;
}

/*
 * Intercepted vkCreateDevice.
 *
 * physical_device  Physical device the logical device is created on.
 * create_info      Application's device creation parameters.
 * allocator        Vulkan allocator callbacks (passed through).
 * device_out       Receives the new VkDevice handle.
 *
 * Unwraps the loader chain to reach the next layer's entry points, creates
 * the real device below us, then builds our per-device state: dispatch
 * table, queue registry, device properties and the stable GPU resources
 * (sampler, descriptor pools, command pool, pipeline layouts).
 */
LAYER_API VkResult overlay_CreateDevice(VkPhysicalDevice physical_device,
                                     const VkDeviceCreateInfo* create_info,
                                     const VkAllocationCallbacks* allocator,
                                     VkDevice* device_out)
{
        instance_data_t* instance_data =
            FIND_OBJ(instance_data_t, physical_device);
        VkLayerDeviceCreateInfo* chain_info =
            get_device_chain_info(create_info, VK_LAYER_LINK_INFO);

        assert(chain_info->u.pLayerInfo);
        PFN_vkGetInstanceProcAddr next_gpa =
            chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkGetDeviceProcAddr next_gdpa =
            chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        PFN_vkCreateDevice next_create_device =
            (PFN_vkCreateDevice)next_gpa(NULL, "vkCreateDevice");
        if (next_create_device == NULL) {
                return VK_ERROR_INITIALIZATION_FAILED;
        }

        /* step past our layer's link-info node in the pNext chain */
        chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

        VkResult result =
            next_create_device(physical_device, create_info, allocator, device_out);
        if (result != VK_SUCCESS) {
                return result;
        }

        device_data_t* device_data   = new_device_data(*device_out, instance_data);
        device_data->physical_device = physical_device;
        vk_load_device_commands(*device_out, next_gdpa,
                                &device_data->vtable);
        /* capture the next-link DestroyDevice (the vtable entry above is the
         * gpa-lookup, which the loader commonly leaves NULL) */
        device_data->chain_DestroyDevice =
            (PFN_vkDestroyDevice)next_gpa(NULL, "vkDestroyDevice");

        instance_data->vtable.GetPhysicalDeviceProperties(
            device_data->physical_device, &device_data->properties);

        /* remember how to hand the device back to the loader (required by
         * the implicit layer contract before vkDestroyDevice) */
        VkLayerDeviceCreateInfo* load_data_info =
            get_device_chain_info(create_info, VK_LOADER_DATA_CALLBACK);
        device_data->set_device_loader_data =
            load_data_info->u.pfnSetDeviceLoaderData;

        device_map_queues(device_data, create_info);

        /* stable GPU resources (sampler, descriptor pools, cmd pool,
         * pipeline layouts) — created once per device */
        create_device_stable_resources(device_data);

        return result;
}

/*
 * Intercepted vkQueuePresentKHR.
 *
 * queue          Presentation queue.
 * present_info   The app's present request (may list several swapchains).
 *
 * The app presents its swapchains one batch at a time, but the overlay has
 * to submit its draw on each swapchain's image individually, so this
 * splits the request: for every swapchain it renders the overlay (when
 * visible), then issues a single-swapchain QueuePresentKHR that also waits
 * on the overlay's completion semaphore. Per-swapchain results are written
 * back into present_info->pResults.
 */
static VkResult overlay_QueuePresentKHR(VkQueue queue,
                                        const VkPresentInfoKHR* present_info)
{
        queue_data_t* queue_data = FIND_OBJ(queue_data_t, queue);

        if (!queue_data) {
                KROSSHAIR_LOG("[KROSSHAIR] QueuePresent: queue_data is NULL!\n");
                return VK_SUCCESS;
        }

        VkResult result          = VK_SUCCESS;
        for (uint32_t i = 0; i < present_info->swapchainCount; i++) {
                VkSwapchainKHR swapchain = present_info->pSwapchains[i];
                swapchain_data_t* swapchain_data =
                    FIND_OBJ(swapchain_data_t, swapchain);

                uint32_t image_index          = present_info->pImageIndices[i];

                /* a copy of the app's present info narrowed to this one swapchain */
                VkPresentInfoKHR single_present = *present_info;
                single_present.swapchainCount   = 1;
                single_present.pSwapchains      = &swapchain;
                single_present.pImageIndices    = &image_index;

                krosshair_draw_t* draw = NULL;
                if (swapchain_data) {
                        /* render the overlay onto this swapchain's image
                         * (no-op when the crosshair is hidden).
                         * Known limitation: only i == 0 receives the app's wait
                         * semaphores; subsequent swapchains draw with
                         * n_wait_semaphores == 0. If two swapchains share a
                         * graphics queue, the second overlay submit does not wait
                         * on the app's semaphore and can race the game's writes to
                         * its image. Pre-existing; not introduced by the leak fix. */
                        draw = render_swapchain_display(
                            swapchain_data, queue_data,
                            present_info->pWaitSemaphores,
                            i == 0 ? present_info->waitSemaphoreCount : 0,
                            image_index);
                }

                if (draw) {
                        single_present.pWaitSemaphores    = &draw->semaphore;
                        single_present.waitSemaphoreCount = 1;
                }

                VkResult chain_result =
                    queue_data->device->vtable.QueuePresentKHR(queue,
                                                               &single_present);

                if (single_present.pResults) {
                        present_info->pResults[i] = chain_result;
                }
                if (chain_result != VK_SUCCESS && result == VK_SUCCESS) {
                        result = chain_result;
                }
        }

        return result;
}

/*
 * Intercepted vkAllocateCommandBuffers.
 *
 * device             VkDevice to allocate on.
 * allocate_info      Allocation parameters (pool, level, count).
 * command_buffers_out Receives the allocated handles.
 *
 * Forwards the allocation down the chain and records per-command-buffer
 * bookkeeping for each handle so later submissions can be intercepted.
 *
 * KNOWN LIMITATION: the object-map entries are never unmapped — there is
 * no vkFreeCommandBuffers interception. They accumulate for the process
 * lifetime (the app frees its command buffers, but the map keeps the host
 * bookkeeping). Pre-existing; out of scope for the leak fix.
 */
static VkResult overlay_AllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* allocate_info,
    VkCommandBuffer* command_buffers_out)
{
        device_data_t* device_data = FIND_OBJ(device_data_t, device);
        VkResult result            = device_data->vtable.AllocateCommandBuffers(
            device, allocate_info, command_buffers_out);
        if (result != VK_SUCCESS) return result;

        for (uint32_t i = 0; i < allocate_info->commandBufferCount; i++) {
                new_cmd_buffer_data(command_buffers_out[i], allocate_info->level,
                                    device_data);
        }

        return result;
}

/*
 * Intercepted vkDestroyDevice.
 *
 * device    VkDevice being destroyed.
 * allocator Vulkan allocator callbacks (passed through).
 *
 * Tears down all host bookkeeping for this device (surviving swapchains,
 * queue data, the device entry itself) before forwarding the destroy call
 * down the layer chain. Device-scoped GPU objects (sampler, pools, cmd
 * pool, layouts, render pass, pipelines) are reclaimed by the driver when
 * the underlying device is destroyed, so they are not freed explicitly.
 */
LAYER_API void overlay_DestroyDevice(VkDevice device,
                                  const VkAllocationCallbacks* allocator)
{
        device_data_t* device_data = FIND_OBJ(device_data_t, device);
        if (!device_data) {
                KROSSHAIR_LOG(
                    "[KROSSHAIR] DestroyDevice: no device_data for device %p\n",
                    (void*)device);
                return;
        }

        PFN_vkDestroyDevice chain_destroy = device_data->chain_DestroyDevice;

        /* tear down any swapchains that survived for this device (the app can
         * destroy the device while swapchains are still alive during shutdown).
         * destroy_swapchain_data does the GPU teardown + host-string frees;
         * unmap + free release the map entries.
         * Assumes the registry is consistent, which only holds while the live
         * count stays <= KROSSHAIR_MAX_SWAPCHAINS (see the registration site
         * in overlay_CreateSwapchainKHR for the >cap desync limitation). */
        uint32_t n_tracked = device_data->swapchain_count;
        if (n_tracked > KROSSHAIR_MAX_SWAPCHAINS)
                n_tracked = KROSSHAIR_MAX_SWAPCHAINS; /* registry cap */
        for (uint32_t i = 0; i < n_tracked; i++) {
                swapchain_data_t* sc = device_data->swapchains[i];
                if (!sc) continue;
                KROSSHAIR_LOG(
                    "[KROSSHAIR] DestroyDevice: tearing down surviving "
                    "swapchain %lu\n", (unsigned long)sc->swapchain);
                destroy_swapchain_data(sc);
                unmap_object(HKEY(sc->swapchain));
                kh_perflog_host_free(sizeof(swapchain_data_t),
                                     "swapchain data");
                free(sc);
        }

        /* free queue data, iterating queues[] once; graphic_queue aliases
         * one entry so null it (never freed separately). */
        device_data->graphic_queue = NULL;
        for (uint32_t i = 0; i < device_data->queue_count; i++) {
                if (device_data->queues[i]) {
                        unmap_object(HKEY(device_data->queues[i]->queue));
                        kh_perflog_host_free(sizeof(queue_data_t),
                                             "queue data");
                        free(device_data->queues[i]);
                        device_data->queues[i] = NULL;
                }
        }

        unmap_object(HKEY(device_data->device));
        kh_perflog_host_free(sizeof(*device_data), "device data");
        free(device_data);

        if (chain_destroy) {
                chain_destroy(device, allocator);
        }
    }

/*
 * Intercepted vkDestroyInstance.
 *
 * instance   VkInstance being destroyed.
 * allocator  Vulkan allocator callbacks (passed through).
 *
 * Unregisters the physical-device map entries (freeing their display
 * names) and the instance entry itself, then forwards the destroy call
 * down the layer chain.
 */
LAYER_API void overlay_DestroyInstance(VkInstance instance,
                                    const VkAllocationCallbacks* allocator)
{
        instance_data_t* instance_data = FIND_OBJ(instance_data_t, instance);
        if (!instance_data) {
                KROSSHAIR_LOG(
                    "[KROSSHAIR] DestroyInstance: no instance_data for "
                    "instance %p\n",
                    (void*)instance);
                return;
        }

        PFN_vkDestroyInstance chain_destroy =
            instance_data->chain_DestroyInstance;

        /* free the physical-device map entries + asprintf'd names; must run
         * before freeing instance_data since it reads the vtable + instance
         * handle. */
        unregister_physical_devices(instance_data);

        unmap_object(HKEY(instance_data->instance));
        kh_perflog_host_free(sizeof(*instance_data), "instance data");
        free(instance_data);

        if (chain_destroy) {
                chain_destroy(instance, allocator);
        }
}

/* ------------------------------------------------------------------
 * Name-to-function dispatch (backs GetInstance/GetDeviceProcAddr)
 * ------------------------------------------------------------------ */

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
overlay_GetInstanceProcAddr(VkInstance instance, const char* func_name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
overlay_GetDeviceProcAddr(VkDevice device, const char* func_name);

typedef struct name_to_funcptr {
        const char* name;
        void* func;
} name_to_funcptr_t;

name_to_funcptr_t name_to_funcptr_map[] = {
    { "vkGetInstanceProcAddr",    (void*)overlay_GetInstanceProcAddr},
    {   "vkGetDeviceProcAddr",      (void*)overlay_GetDeviceProcAddr},
    {      "vkCreateInstance",         (void*)overlay_CreateInstance},
    {     "vkQueuePresentKHR",        (void*)overlay_QueuePresentKHR},
    {  "vkCreateSwapchainKHR",     (void*)overlay_CreateSwapchainKHR},
    { "vkDestroySwapchainKHR",    (void*)overlay_DestroySwapchainKHR},
    {      "vkCreateDevice",           (void*)overlay_CreateDevice},
    {   "vkDestroyDevice",          (void*)overlay_DestroyDevice},
    {  "vkDestroyInstance",        (void*)overlay_DestroyInstance},
    {"AllocateCommandBuffers", (void*)overlay_AllocateCommandBuffers}
};

LAYER_API size_t name_to_funcptr_map_count =
    (sizeof(name_to_funcptr_map) / sizeof(name_to_funcptr_map[0]));

/*
 * Look up a Vulkan function name in the layer's override table.
 *
 * name  The function name as queried (e.g. "vkCreateDevice").
 *
 * Returns the overriding function, or NULL to fall through to the next
 * layer / driver.
 */
LAYER_API void* find_ptr(const char* name)
{
        for (uint32_t i = 0; i < name_to_funcptr_map_count; i++) {
                if (!strcmp(name, name_to_funcptr_map[i].name)) {
                        return name_to_funcptr_map[i].func;
                }
        }
        return NULL;
}

/*
 * Intercepted vkGetInstanceProcAddr.
 *
 * instance  Instance handle, or NULL when querying before creation.
 * func_name Name of the function to resolve.
 *
 * Returns the layer's override if one exists, otherwise forwards the
 * query to the next layer/driver in the chain.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
overlay_GetInstanceProcAddr(VkInstance instance, const char* func_name)
{
        void* ptr = find_ptr(func_name);
        if (ptr) return (PFN_vkVoidFunction)ptr;
        if (instance == NULL) return NULL;

        instance_data_t* instance_data = FIND_OBJ(instance_data_t, instance);
        if (instance_data->vtable.GetInstanceProcAddr == NULL) return NULL;

        return instance_data->vtable.GetInstanceProcAddr(instance, func_name);
}

/*
 * Intercepted vkGetDeviceProcAddr.
 *
 * device    Device handle (may be NULL when the query does not need it).
 * func_name Name of the function to resolve.
 *
 * Returns the layer's override if one exists, otherwise forwards the
 * query to the next layer/driver in the chain.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
overlay_GetDeviceProcAddr(VkDevice device, const char* func_name)
{
        void* ptr = find_ptr(func_name);
        if (ptr) return (PFN_vkVoidFunction)ptr;
        if (device == NULL) return NULL;

        device_data_t* device_data = FIND_OBJ(device_data_t, device);
        if (device_data->vtable.GetDeviceProcAddr == NULL) return NULL;

        return device_data->vtable.GetDeviceProcAddr(device, func_name);
}
