/*
 * Unit tests for the pure memory-type selection in src/gpu.c.
 *
 * vk_memory_type() reads the physical device's memory properties
 * through the instance vtable, so the test installs a stub
 * GetPhysicalDeviceMemoryProperties that returns a synthesized
 * VkPhysicalDeviceMemoryProperties and asserts the selected type
 * index for various requirement/flag/type-bit combinations.
 */
#define _GNU_SOURCE

#include <stdint.h>

#include "test.h"
#include "../include/krosshair.h"

/*
 * Synthesized memory types:
 *   0: host-visible, host-coherent
 *   1: device-local only
 *   2: host-visible, host-coherent, device-local
 *   3: host-visible (non-coherent)
 */
static VkPhysicalDeviceMemoryProperties g_mem_props = {
    .memoryTypeCount = 4,
    .memoryTypes = {
        { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 },
        { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 },
        { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 },
        { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0 },
    },
};

static void stub_memory_properties(VkPhysicalDevice pd,
                                   VkPhysicalDeviceMemoryProperties* out)
{
    (void)pd;
    *out = g_mem_props;
}

static device_data_t g_device;
static instance_data_t g_instance;

/* Wire a zeroed device whose memory-property query hits the stub. */
static void reset_device(void)
{
    memset(&g_instance, 0, sizeof(g_instance));
    memset(&g_device, 0, sizeof(g_device));
    g_instance.vtable.GetPhysicalDeviceMemoryProperties =
        stub_memory_properties;
    g_device.instance = &g_instance;
    g_device.physical_device = (VkPhysicalDevice)(uintptr_t)0x2222;
}

static void test_host_visible_selection(void)
{
    reset_device();
    /* only type 0 may be used */
    CHECK_EQ(vk_memory_type(&g_device, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 1), 0);
    /* types 0 and 1: type 1 is not host-visible */
    CHECK_EQ(vk_memory_type(&g_device, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 3), 0);
    /* only type 3 may be used: non-coherent host-visible */
    CHECK_EQ(vk_memory_type(&g_device, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 8), 3);
    /* types 0 and 3 both match: the first wins */
    CHECK_EQ(vk_memory_type(&g_device, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 9), 0);
}

static void test_device_local_selection(void)
{
    reset_device();
    /* types 0 and 1: only type 1 is device-local */
    CHECK_EQ(vk_memory_type(&g_device, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 3), 1);
    CHECK_EQ(vk_memory_type(&g_device, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 15),
             1);
}

static void test_combined_flags_pick_first_superset(void)
{
    reset_device();
    /* host-visible + device-local: type 2 is the first to expose both */
    CHECK_EQ(vk_memory_type(&g_device,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           15),
             2);
    /* host-visible + coherent on types 0 and 3: type 0 */
    CHECK_EQ(vk_memory_type(&g_device,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           9),
             0);
}

static void test_no_matching_type(void)
{
    reset_device();
    /* type 1 exposes no property flags */
    CHECK_EQ(vk_memory_type(&g_device, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 2),
             K_NO_MEMORYTYPE);
    /* type 3 is not coherent */
    CHECK_EQ(vk_memory_type(&g_device,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           8),
             K_NO_MEMORYTYPE);
    /* no candidate bits at all */
    CHECK_EQ(vk_memory_type(&g_device, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0),
             K_NO_MEMORYTYPE);
}

static void test_zero_property_flags_accepts_first(void)
{
    reset_device();
    CHECK_EQ(vk_memory_type(&g_device, 0, 15), 0);
}

static void test_no_memory_types(void)
{
    reset_device();
    g_mem_props.memoryTypeCount = 0;
    CHECK_EQ(vk_memory_type(&g_device, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 15),
             K_NO_MEMORYTYPE);
    g_mem_props.memoryTypeCount = 4;
}

int main(void)
{
    fprintf(stderr, "# test_gpu\n");
    test_host_visible_selection();
    test_device_local_selection();
    test_combined_flags_pick_first_superset();
    test_no_matching_type();
    test_zero_property_flags_accepts_first();
    test_no_memory_types();
    fprintf(stderr, "  -> %d passed, %d failed\n", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
