#ifndef KROSSHAIR_TEST_LAYER_API_H
#define KROSSHAIR_TEST_LAYER_API_H

/*
 * Declarations of the test-only exports that src/layer.c exposes when
 * compiled with -DUNIT_TEST. Types must stay in sync with layer.c.
 */

#include <stddef.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

typedef struct name_to_funcptr {
    const char* name;
    void* func;
} name_to_funcptr_t;

extern name_to_funcptr_t name_to_funcptr_map[];
extern size_t name_to_funcptr_map_count;
extern void* find_ptr(const char* name);

extern VkResult
overlay_CreateInstance(const VkInstanceCreateInfo* create_info,
                       const VkAllocationCallbacks* allocator,
                       VkInstance* instance);
extern VkResult overlay_CreateDevice(VkPhysicalDevice physicalDevice,
                                     const VkDeviceCreateInfo* create_info,
                                     const VkAllocationCallbacks* allocator,
                                     VkDevice* device);
extern void
overlay_DestroyDevice(VkDevice device,
                      const VkAllocationCallbacks* allocator);
extern void
overlay_DestroyInstance(VkInstance instance,
                        const VkAllocationCallbacks* allocator);

/* Non-static in the layer regardless of build type (see layer.c). */
extern PFN_vkVoidFunction
overlay_GetInstanceProcAddr(VkInstance instance, const char* func_name);
extern PFN_vkVoidFunction
overlay_GetDeviceProcAddr(VkDevice device, const char* func_name);

#endif /* KROSSHAIR_TEST_LAYER_API_H */
