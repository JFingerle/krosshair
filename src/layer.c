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

pthread_mutex_t global_lock;
VkPhysicalDeviceDriverProperties driver_properties = {};

/* ------------------------------------------------------------------
 * Hotkey toggle state
 * ------------------------------------------------------------------ */
static void instance_data_map_physical_devices(instance_data_t* instance_data,
                                               int map)
{
        uint32_t physical_device_count = 0;
        instance_data->vtable.EnumeratePhysicalDevices(
            instance_data->instance, &physical_device_count, NULL);

        VkPhysicalDevice* physical_devices =
            malloc(sizeof(VkPhysicalDevice) * physical_device_count);
        instance_data->vtable.EnumeratePhysicalDevices(
            instance_data->instance, &physical_device_count, physical_devices);

        for (uint32_t i = 0; i < physical_device_count; i++) {
                if (map) {
                        KROSSHAIR_LOG("[*] mapping physical_devices[%d] obj: %lu %p\n",
                               i, HKEY(physical_devices[i]), (void*)instance_data);
                        char* fmt_phys_device;
                        asprintf(&fmt_phys_device, "physical_devices[%d]", i);
                        map_object(HKEY(physical_devices[i]), instance_data,
                                   fmt_phys_device);
                } else {
                        /* free the name string we allocated with asprintf */
                        vk_object_t obj;
                        if (vk_map_get(&vk_obj_map, HKEY(physical_devices[i]), &obj))
                                free((char*)obj.name);
                        unmap_object(HKEY(physical_devices[i]));
                }
        }

        free(physical_devices);
}

static swapchain_data_t* new_swapchain_data(VkSwapchainKHR swapchain,
                                            device_data_t* device_data)
{
        swapchain_data_t* swapchain_data = malloc(sizeof(*swapchain_data));
        memset(swapchain_data, 0, sizeof(*swapchain_data));
        swapchain_data->device_data = device_data;
        swapchain_data->swapchain   = swapchain;
        KROSSHAIR_LOG("[*] mapping data->swapchain obj: %lu %p\n",
               HKEY(swapchain_data->swapchain), (void*)swapchain_data);
        map_object(HKEY(swapchain_data->swapchain), swapchain_data,
                   "swapchain_data->swapchain");
         return swapchain_data;
 }

 /* remove `data` from the device's swapchain registry. Only compares the
  * pointer value — `data` may already be freed when this is called */
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

static VkResult overlay_CreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
        VkSwapchainCreateInfoKHR create_info = *pCreateInfo;
        create_info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        device_data_t* device_data = FIND_OBJ(device_data_t, device);

        /* save reference to old swapchain data - we must NOT destroy it before
         * the create call because the driver needs oldSwapchain to be valid */
        swapchain_data_t* old_swapchain_data = NULL;
        if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE) {
                old_swapchain_data =
                    FIND_OBJ(swapchain_data_t, pCreateInfo->oldSwapchain);
        }

        VkResult result = device_data->vtable.CreateSwapchainKHR(
            device, &create_info, pAllocator, pSwapchain);
        if (result != VK_SUCCESS) return result;

        /* now that the new swapchain is created, clean up the old one's
         * internal resources (Vulkan retires oldSwapchain automatically) */
        if (old_swapchain_data) {
                destroy_swapchain_data(old_swapchain_data);
                unmap_object(HKEY(old_swapchain_data->swapchain));
                unregister_swapchain(device_data, old_swapchain_data);
                free(old_swapchain_data);
        }

        swapchain_data_t* swapchain_data =
            new_swapchain_data(*pSwapchain, device_data);

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

        setup_swapchain_data(swapchain_data, pCreateInfo);

        KROSSHAIR_LOG("[KROSSHAIR] CreateSwapchainKHR: created %lu (%ux%u, n_images=%u, old=%lu)\n",
                      (unsigned long)*pSwapchain, pCreateInfo->imageExtent.width,
                      pCreateInfo->imageExtent.height, swapchain_data->n_images,
                      (unsigned long)pCreateInfo->oldSwapchain);

        return result;
}

static void overlay_DestroySwapchainKHR(VkDevice device,
                                        VkSwapchainKHR swapchain,
                                        const VkAllocationCallbacks* pAllocator)
{
        device_data_t* device_data = FIND_OBJ(device_data_t, device);
        swapchain_data_t* data     = FIND_OBJ(swapchain_data_t, swapchain);

        if (data) {
                destroy_swapchain_data(data);
                unmap_object(HKEY(data->swapchain));
                unregister_swapchain(device_data, data);
                free(data);
        }

        device_data->vtable.DestroySwapchainKHR(device_data->device,
                                                swapchain, pAllocator);
}

static VkResult overlay_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator,
                                       VkInstance* pInstance)
{
        init_input_thread();

        VkLayerInstanceCreateInfo* chain_info =
            get_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
        assert(chain_info->u.pLayerInfo);
        PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
            chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkCreateInstance fpCreateInstance =
            (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL,
                                                        "vkCreateInstance");
        if (!fpCreateInstance) {
                return VK_ERROR_INITIALIZATION_FAILED;
        }

        chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

        VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
        if (result != VK_SUCCESS) return result;

        instance_data_t* instance_data = new_instance_data(*pInstance);
        vk_load_instance_commands(instance_data->instance,
                                  fpGetInstanceProcAddr,
                                  &instance_data->vtable);
        /* capture the next-link DestroyInstance (the vtable entry above is the
         * gpa-lookup, which the loader commonly leaves NULL) */
        instance_data->chain_DestroyInstance =
            (PFN_vkDestroyInstance)fpGetInstanceProcAddr(NULL,
                                                         "vkDestroyInstance");
        instance_data_map_physical_devices(instance_data, 1);

        return result;
}


static krosshair_draw_t* before_present(swapchain_data_t* swapchain_data,
                                        queue_data_t* present_queue,
                                        const VkSemaphore* wait_semaphores,
                                        unsigned n_wait_semaphores,
                                        unsigned image_index)
{
        krosshair_draw_t* draw = NULL;

        draw = render_swapchain_display(swapchain_data, present_queue,
                                        wait_semaphores, n_wait_semaphores,
                                        image_index);

        return draw;
}

static VkLayerDeviceCreateInfo* get_device_chain_info(
    const VkDeviceCreateInfo* pCreateInfo, VkLayerFunction func)
{
        vk_foreach_struct(item, pCreateInfo->pNext)
        {
                if (item->sType ==
                        VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                    ((VkLayerDeviceCreateInfo*)item)->function == func)
                        return (VkLayerDeviceCreateInfo*)item;
        }
        return NULL;
}

static VkResult overlay_CreateDevice(VkPhysicalDevice physical_device,
                                     const VkDeviceCreateInfo* pCreateInfo,
                                     const VkAllocationCallbacks* pAllocator,
                                     VkDevice* pDevice)
{
        instance_data_t* instance_data =
            FIND_OBJ(instance_data_t, physical_device);
        VkLayerDeviceCreateInfo* chain_info =
            get_device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

        assert(chain_info->u.pLayerInfo);
        PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
            chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr =
            chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        PFN_vkCreateDevice fpCreateDevice =
            (PFN_vkCreateDevice)fpGetInstanceProcAddr(NULL, "vkCreateDevice");
        if (fpCreateDevice == NULL) {
                return VK_ERROR_INITIALIZATION_FAILED;
        }

        chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

        /*
         * this whole driver stuff doesn't seem necessary?
         * */
        // const char** enabled_extensions = malloc(
        //     sizeof(*enabled_extensions) *
        //     pCreateInfo->enabledExtensionCount);
        // for (size_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        //         enabled_extensions[i] =
        //         pCreateInfo->ppEnabledExtensionNames[i];
        // };
        //
        // uint32_t extension_count;
        // instance_data->vtable.EnumerateDeviceExtensionProperties(
        //     physical_device, NULL, &extension_count, NULL);
        //
        // VkExtensionProperties* available_extensions =
        //     malloc(sizeof(*available_extensions) * extension_count);
        // instance_data->vtable.EnumerateDeviceExtensionProperties(
        //     physical_device, NULL, &extension_count, available_extensions);
        //
        // uint32_t found_extensions = 0;
        // // TODO: this works?
        // for (size_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        //         for (size_t j = 0; j < pCreateInfo->enabledExtensionCount;
        //              j++) {
        //                 printf("available_extensions[%lu]: %s\n", i,
        //                        available_extensions[i].extensionName);
        //                 printf("enabled_extensions[%lu]: %s\n", i,
        //                        enabled_extensions[j]);
        //                 if (!strcmp(available_extensions[i].extensionName,
        //                             enabled_extensions[j])) {
        //                         found_extensions = found_extensions + 1;
        //                 }
        //         }
        // }
        // free(available_extensions);
        // free(enabled_extensions);
        // if (found_extensions != pCreateInfo->enabledExtensionCount) {
        //         printf(
        //             "[KROSSHAIR_ERROR] extensions don't match. "
        //             "found_extensions: %d, enabled_extensions: %d\n",
        //             found_extensions, pCreateInfo->enabledExtensionCount);
        // }

        VkResult result =
            fpCreateDevice(physical_device, pCreateInfo, pAllocator, pDevice);
        if (result != VK_SUCCESS) {
                return result;
        }

        device_data_t* device_data   = new_device_data(*pDevice, instance_data);
        device_data->physical_device = physical_device;
        vk_load_device_commands(*pDevice, fpGetDeviceProcAddr,
                                &device_data->vtable);
        /* capture the next-link DestroyDevice (the vtable entry above is the
         * gpa-lookup, which the loader commonly leaves NULL) */
        device_data->chain_DestroyDevice =
            (PFN_vkDestroyDevice)fpGetInstanceProcAddr(NULL,
                                                       "vkDestroyDevice");

        instance_data->vtable.GetPhysicalDeviceProperties(
            device_data->physical_device, &device_data->properties);

        VkLayerDeviceCreateInfo* load_data_info =
            get_device_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
        device_data->set_device_loader_data =
            load_data_info->u.pfnSetDeviceLoaderData;

        // driver_properties.sType =
        //     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        // driver_properties.pNext = NULL;

        device_map_queues(device_data, pCreateInfo);

        /* stable GPU resources (sampler, descriptor pools, cmd pool,
         * pipeline layouts) — created once per device */
        create_device_stable_resources(device_data);

        return result;
}

static VkResult overlay_QueuePresentKHR(VkQueue queue,
                                        const VkPresentInfoKHR* pPresentInfo)
{
        queue_data_t* queue_data = FIND_OBJ(queue_data_t, queue);

        if (!queue_data) {
                KROSSHAIR_LOG("[KROSSHAIR] QueuePresent: queue_data is NULL!\n");
                return VK_SUCCESS;
        }

        VkResult result          = VK_SUCCESS;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
                VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[i];
                swapchain_data_t* swapchain_data =
                    FIND_OBJ(swapchain_data_t, swapchain);

                uint32_t image_index          = pPresentInfo->pImageIndices[i];

                VkPresentInfoKHR present_info = *pPresentInfo;
                present_info.swapchainCount   = 1;
                present_info.pSwapchains      = &swapchain;
                present_info.pImageIndices    = &image_index;

                krosshair_draw_t* draw = NULL;
                if (swapchain_data) {
                        /* Known limitation: only i == 0 receives the app's wait
                         * semaphores; subsequent swapchains draw with
                         * n_wait_semaphores == 0. If two swapchains share a
                         * graphics queue, the second overlay submit does not wait
                         * on the app's semaphore and can race the game's writes to
                         * its image. Pre-existing; not introduced by the leak fix. */
                        draw = before_present(
                            swapchain_data, queue_data,
                            pPresentInfo->pWaitSemaphores,
                            i == 0 ? pPresentInfo->waitSemaphoreCount : 0,
                            image_index);
                }

                if (draw) {
                        present_info.pWaitSemaphores    = &draw->semaphore;
                        present_info.waitSemaphoreCount = 1;
                }

                VkResult chain_result =
                    queue_data->device->vtable.QueuePresentKHR(queue,
                                                               &present_info);

                if (present_info.pResults) {
                        pPresentInfo->pResults[i] = chain_result;
                }
                if (chain_result != VK_SUCCESS && result == VK_SUCCESS) {
                        result = chain_result;
                }
        }

        return result;
}

static VkResult overlay_AllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers)
{
        device_data_t* device_data = FIND_OBJ(device_data_t, device);
        VkResult result            = device_data->vtable.AllocateCommandBuffers(
            device, pAllocateInfo, pCommandBuffers);
        if (result != VK_SUCCESS) return result;

        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
                new_cmd_buffer_data(pCommandBuffers[i], pAllocateInfo->level,
                                    device_data);
        }

        /* KNOWN LIMITATION: the vk_obj_map entries above are never unmapped —
         * there is no vkFreeCommandBuffers interception. The entries accumulate
         * for the process lifetime (the app frees its command buffers, but the
         * map keeps the host bookkeeping). Pre-existing; out of scope for this
         * leak fix. */

         return result;
  }

static void overlay_DestroyDevice(VkDevice device,
                                  const VkAllocationCallbacks* pAllocator)
{
        device_data_t* device_data = FIND_OBJ(device_data_t, device);
        if (!device_data) {
                KROSSHAIR_LOG(
                    "[KROSSHAIR] DestroyDevice: no device_data for device %p\n",
                    (void*)device);
                return;
        }

        PFN_vkDestroyDevice chain_destroy = device_data->chain_DestroyDevice;

        /* device-scoped GPU objects (sampler, descriptor pools, cmd pool,
         * layouts, render pass, pipelines) are reclaimed by the driver when
         * the underlying device is destroyed, so we do not destroy them
         * explicitly here. Only the host bookkeeping must be freed. */

        /* tear down any swapchains that survived for this device (the app can
         * destroy the device while swapchains are still alive during shutdown).
         * destroy_swapchain_data does the GPU teardown + host-string frees;
         * unmap + free release the map entries.
         * Assumes the registry is consistent, which only holds while the live
         * count stays <= KROSSHAIR_MAX_SWAPCHAINS (see the registration site
         * in overlay_CreateSwapchainKHR for the >cap desync limitation). */
        uint32_t n = device_data->swapchain_count;
        if (n > KROSSHAIR_MAX_SWAPCHAINS)
                n = KROSSHAIR_MAX_SWAPCHAINS; /* registry cap */
        for (uint32_t i = 0; i < n; i++) {
                swapchain_data_t* sc = device_data->swapchains[i];
                if (!sc) continue;
                KROSSHAIR_LOG(
                    "[KROSSHAIR] DestroyDevice: tearing down surviving "
                    "swapchain %lu\n", (unsigned long)sc->swapchain);
                destroy_swapchain_data(sc);
                unmap_object(HKEY(sc->swapchain));
                free(sc);
        }

        /* free queue data, iterating queues[] once; graphic_queue aliases
         * one entry so null it (never freed separately). */
        device_data->graphic_queue = NULL;
        for (uint32_t i = 0; i < device_data->queue_count; i++) {
                if (device_data->queues[i]) {
                        unmap_object(HKEY(device_data->queues[i]->queue));
                        free(device_data->queues[i]);
                        device_data->queues[i] = NULL;
                }
        }

        unmap_object(HKEY(device_data->device));
        free(device_data);

        if (chain_destroy) {
                chain_destroy(device, pAllocator);
        }
}

static void overlay_DestroyInstance(VkInstance instance,
                                    const VkAllocationCallbacks* pAllocator)
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

        /* free the physical-device map entries + asprintf'd names (the (…,0)
         * path is the only place they're freed); must run before freeing
         * instance_data since it reads the vtable + instance handle. */
        instance_data_map_physical_devices(instance_data, 0);

        unmap_object(HKEY(instance_data->instance));
        free(instance_data);

        if (chain_destroy) {
                chain_destroy(instance, pAllocator);
        }
}

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

size_t name_to_funcptr_map_count =
    (sizeof(name_to_funcptr_map) / sizeof(name_to_funcptr_map[0]));

static void* find_ptr(const char* name)
{
        for (uint32_t i = 0; i < name_to_funcptr_map_count; i++) {
                if (!strcmp(name, name_to_funcptr_map[i].name)) {
                        return name_to_funcptr_map[i].func;
                }
        }
        return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
overlay_GetInstanceProcAddr(VkInstance instance, const char* func_name)
{
        void* ptr = find_ptr(func_name);
        // printf("found function %s at %p\n", func_name, ptr);
        if (ptr) return (PFN_vkVoidFunction)ptr;
        if (instance == NULL) return NULL;

        instance_data_t* instance_data = FIND_OBJ(instance_data_t, instance);
        if (instance_data->vtable.GetInstanceProcAddr == NULL) return NULL;

        return instance_data->vtable.GetInstanceProcAddr(instance, func_name);
}

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
