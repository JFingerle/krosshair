/*
 * Integration tests for the layer entry points (src/layer.c) with the
 * "next" chain stubbed.
 *
 * Instead of a real Vulkan loader + ICD, this test hands
 * overlay_CreateInstance / overlay_CreateDevice hand-built
 * VkLayerInstanceCreateInfo / VkLayerDeviceCreateInfo chains whose
 * GetInstanceProcAddr / GetDeviceProcAddr point at stub functions. The
 * stubs hand out fake handles and record calls, so we can assert exactly
 * what the layer did: which chain calls it made, what it registered in
 * the object map, and how it tore everything down.
 */
#define _GNU_SOURCE

#include <stdint.h>

#include "test.h"
#include "test_layer_api.h"
#include "../include/krosshair.h"

/* ── fake handles ────────────────────────────────────────────── */

#define FAKE_INSTANCE ((VkInstance)(uintptr_t)0x1111)
#define FAKE_PHYS     ((VkPhysicalDevice)(uintptr_t)0x2222)
#define FAKE_DEVICE   ((VkDevice)(uintptr_t)0x3333)
#define FAKE_QUEUE    ((VkQueue)(uintptr_t)0x4444)

#define FAKE_SAMPLER     ((VkSampler)(uintptr_t)0x5001)
#define FAKE_DESC_POOL   ((VkDescriptorPool)(uintptr_t)0x5002)
#define FAKE_DESC_LAYOUT ((VkDescriptorSetLayout)(uintptr_t)0x5003)
#define FAKE_PIPE_LAYOUT ((VkPipelineLayout)(uintptr_t)0x5004)
#define FAKE_CMD_POOL    ((VkCommandPool)(uintptr_t)0x5005)

/* ── call counters ───────────────────────────────────────────── */

static int stub_create_instance_calls = 0;
static int stub_destroy_instance_calls = 0;
static int stub_create_device_calls = 0;
static int stub_destroy_device_calls = 0;
static int stub_set_loader_data_calls = 0;
static VkQueue stub_last_loader_data_queue = VK_NULL_HANDLE;

/* ── instance-scope stubs ────────────────────────────────────── */

static VkResult stub_create_instance(const VkInstanceCreateInfo* ci,
                                      const VkAllocationCallbacks* alloc,
                                      VkInstance* out)
{
        (void)ci;
        (void)alloc;
        stub_create_instance_calls++;
        *out = FAKE_INSTANCE;
        return VK_SUCCESS;
}

static void stub_destroy_instance(VkInstance instance,
                                  const VkAllocationCallbacks* alloc)
{
        (void)instance;
        (void)alloc;
        stub_destroy_instance_calls++;
}

static VkResult stub_create_device(VkPhysicalDevice pd,
                                   const VkDeviceCreateInfo* ci,
                                   const VkAllocationCallbacks* alloc,
                                   VkDevice* out)
{
        (void)pd;
        (void)ci;
        (void)alloc;
        stub_create_device_calls++;
        *out = FAKE_DEVICE;
        return VK_SUCCESS;
}

static void stub_destroy_device(VkDevice device,
                                const VkAllocationCallbacks* alloc)
{
        (void)device;
        (void)alloc;
        stub_destroy_device_calls++;
}

static VkResult stub_enumerate_physical_devices(VkInstance instance,
                                                uint32_t* count,
                                                VkPhysicalDevice* devices)
{
        (void)instance;
        if (devices == NULL) {
                *count = 1;
                return VK_SUCCESS;
        }
        devices[0] = FAKE_PHYS;
        return VK_SUCCESS;
}

static void stub_get_pd_properties(VkPhysicalDevice pd,
                                   VkPhysicalDeviceProperties* props)
{
        (void)pd;
        memset(props, 0, sizeof(*props));
        props->apiVersion = VK_MAKE_VERSION(1, 3, 0);
        props->deviceID   = 42;
        props->limits.maxImageDimension2D = 8192;
}

static void stub_get_queue_families(VkPhysicalDevice pd, uint32_t* count,
                                    VkQueueFamilyProperties* props)
{
        (void)pd;
        if (props == NULL) {
                *count = 1;
                return;
        }
        props[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
        props[0].queueCount = 8;
        props[0].minImageTransferGranularity.width  = 1;
        props[0].minImageTransferGranularity.height = 1;
        props[0].minImageTransferGranularity.depth  = 1;
}

static PFN_vkVoidFunction stub_instance_gpa(VkInstance instance, const char* name)
{
        (void)instance;
        if (name == NULL) return NULL;
        if (strcmp(name, "vkCreateInstance") == 0)
                return (PFN_vkVoidFunction)stub_create_instance;
        if (strcmp(name, "vkDestroyInstance") == 0)
                return (PFN_vkVoidFunction)stub_destroy_instance;
        if (strcmp(name, "vkCreateDevice") == 0)
                return (PFN_vkVoidFunction)stub_create_device;
        if (strcmp(name, "vkDestroyDevice") == 0)
                return (PFN_vkVoidFunction)stub_destroy_device;
        if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
                return (PFN_vkVoidFunction)stub_enumerate_physical_devices;
        if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
                return (PFN_vkVoidFunction)stub_get_pd_properties;
        if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
                return (PFN_vkVoidFunction)stub_get_queue_families;
        return NULL;
}

static PFN_vkVoidFunction stub_instance_gpa_empty(VkInstance instance, const char* name)
{
        (void)instance;
        (void)name;
        return NULL;
}

/* ── device-scope stubs ──────────────────────────────────────── */

static void stub_get_device_queue(VkDevice device, uint32_t family,
                                  uint32_t index, VkQueue* out)
{
        (void)device;
        (void)family;
        (void)index;
        *out = FAKE_QUEUE;
}

static void stub_create_sampler(VkDevice device, const VkSamplerCreateInfo* ci,
                                const VkAllocationCallbacks* alloc,
                                VkSampler* out)
{
        (void)device;
        (void)ci;
        (void)alloc;
        *out = FAKE_SAMPLER;
}

static void stub_create_descriptor_pool(VkDevice device,
                                        const VkDescriptorPoolCreateInfo* ci,
                                        const VkAllocationCallbacks* alloc,
                                        VkDescriptorPool* out)
{
        (void)device;
        (void)ci;
        (void)alloc;
        *out = FAKE_DESC_POOL;
}

static void stub_create_descriptor_set_layout(
        VkDevice device, const VkDescriptorSetLayoutCreateInfo* ci,
        const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out)
{
        (void)device;
        (void)ci;
        (void)alloc;
        *out = FAKE_DESC_LAYOUT;
}

static void stub_create_pipeline_layout(VkDevice device,
                                        const VkPipelineLayoutCreateInfo* ci,
                                        const VkAllocationCallbacks* alloc,
                                        VkPipelineLayout* out)
{
        (void)device;
        (void)ci;
        (void)alloc;
        *out = FAKE_PIPE_LAYOUT;
}

static void stub_create_command_pool(VkDevice device,
                                     const VkCommandPoolCreateInfo* ci,
                                     const VkAllocationCallbacks* alloc,
                                     VkCommandPool* out)
{
        (void)device;
        (void)ci;
        (void)alloc;
        *out = FAKE_CMD_POOL;
}

static PFN_vkVoidFunction stub_device_gdpa(VkDevice device, const char* name)
{
        (void)device;
        if (name == NULL) return NULL;
        if (strcmp(name, "vkGetDeviceQueue") == 0)
                return (PFN_vkVoidFunction)stub_get_device_queue;
        if (strcmp(name, "vkCreateSampler") == 0)
                return (PFN_vkVoidFunction)stub_create_sampler;
        if (strcmp(name, "vkCreateDescriptorPool") == 0)
                return (PFN_vkVoidFunction)stub_create_descriptor_pool;
        if (strcmp(name, "vkCreateDescriptorSetLayout") == 0)
                return (PFN_vkVoidFunction)stub_create_descriptor_set_layout;
        if (strcmp(name, "vkCreatePipelineLayout") == 0)
                return (PFN_vkVoidFunction)stub_create_pipeline_layout;
        if (strcmp(name, "vkCreateCommandPool") == 0)
                return (PFN_vkVoidFunction)stub_create_command_pool;
        return NULL;
}

static VkResult stub_set_loader_data(VkDevice device, void* loader_data)
{
        (void)device;
        stub_set_loader_data_calls++;
        stub_last_loader_data_queue = (VkQueue)loader_data;
        return VK_SUCCESS;
}

/* ── chain builders ──────────────────────────────────────────── */

static VkLayerInstanceLink instance_link;
static VkLayerInstanceCreateInfo instance_chain;

static void setup_instance_chain(PFN_vkGetInstanceProcAddr gpa)
{
        instance_link.pNext = NULL;
        instance_link.pfnNextGetInstanceProcAddr = gpa;
        instance_chain.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
        instance_chain.function = VK_LAYER_LINK_INFO;
        instance_chain.u.pLayerInfo = &instance_link;
}

static VkResult make_instance(VkInstance* out)
{
        setup_instance_chain(stub_instance_gpa);
        VkInstanceCreateInfo ci = { 0 };
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pNext = &instance_chain;
        return overlay_CreateInstance(&ci, NULL, out);
}

static VkLayerDeviceLink device_link;
static VkLayerDeviceCreateInfo device_chain_link;
static VkLayerDeviceCreateInfo device_chain_loader;

static VkResult make_device(VkPhysicalDevice phys, VkDevice* out)
{
        device_link.pNext = NULL;
        device_link.pfnNextGetInstanceProcAddr = stub_instance_gpa;
        device_link.pfnNextGetDeviceProcAddr   = stub_device_gdpa;
        device_chain_link.sType    = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
        device_chain_link.function = VK_LAYER_LINK_INFO;
        device_chain_link.u.pLayerInfo = &device_link;
        device_chain_loader.sType    = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
        device_chain_loader.function = VK_LOADER_DATA_CALLBACK;
        device_chain_loader.u.pfnSetDeviceLoaderData = stub_set_loader_data;

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = { 0 };
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = 0;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &prio;

        device_chain_link.pNext = &device_chain_loader;
        device_chain_loader.pNext = NULL;

        VkDeviceCreateInfo ci = { 0 };
        ci.sType             = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.pNext             = &device_chain_link;
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos     = &qci;
        return overlay_CreateDevice(phys, &ci, NULL, out);
}

/* ── tests ───────────────────────────────────────────────────── */

static void test_create_instance(void)
{
        VkInstance inst = VK_NULL_HANDLE;
        CHECK_EQ((int)make_instance(&inst), (int)VK_SUCCESS);
        CHECK_EQ(stub_create_instance_calls, 1);
        CHECK_EQ((uintptr_t)inst, (uintptr_t)FAKE_INSTANCE);

        /* both the instance and its physical device are mapped, and both
         * resolve to the same instance_data */
        instance_data_t* id =
                (instance_data_t*)find_object_data(HKEY(FAKE_INSTANCE));
        CHECK(id != NULL);
        if (id) {
                CHECK_EQ((uintptr_t)id->instance, (uintptr_t)FAKE_INSTANCE);
                /* the layer does not populate api_version; the struct is
                 * zero-initialized in new_instance_data() */
                CHECK_EQ((int)id->api_version, 0);
        }
        CHECK_PTR(find_object_data(HKEY(FAKE_PHYS)),
                  find_object_data(HKEY(FAKE_INSTANCE)));
}

static void test_gpa_fallthrough_after_instance(void)
{
        VkInstance inst = FAKE_INSTANCE;

        /* intercepted names win even with a live instance */
        CHECK_PTR(overlay_GetInstanceProcAddr(inst, "vkCreateDevice"),
                  (void*)overlay_CreateDevice);
        /* non-intercepted names fall through to the chain GPA */
        CHECK_PTR(
                overlay_GetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties"),
                (void*)stub_get_pd_properties);
        CHECK(overlay_GetInstanceProcAddr(inst, "vkTotallyBogus") == NULL);
}

static void test_create_device(void)
{
        VkDevice dev = VK_NULL_HANDLE;
        CHECK_EQ((int)make_device(FAKE_PHYS, &dev), (int)VK_SUCCESS);
        CHECK_EQ(stub_create_device_calls, 1);
        CHECK_EQ((uintptr_t)dev, (uintptr_t)FAKE_DEVICE);

        device_data_t* dd = (device_data_t*)find_object_data(HKEY(FAKE_DEVICE));
        CHECK(dd != NULL);
        if (!dd)
                return;

        CHECK_PTR(dd->instance, find_object_data(HKEY(FAKE_INSTANCE)));
        CHECK_EQ((uintptr_t)dd->physical_device, (uintptr_t)FAKE_PHYS);
        CHECK_EQ((int)dd->properties.apiVersion, VK_MAKE_VERSION(1, 3, 0));
        CHECK_EQ((int)dd->queue_count, 1);

        /* graphics queue discovered via the stub chain */
        CHECK(dd->graphic_queue != NULL);
        if (dd->graphic_queue) {
                CHECK_EQ((uintptr_t)dd->graphic_queue->queue,
                         (uintptr_t)FAKE_QUEUE);
                CHECK_EQ((int)dd->graphic_queue->family_index, 0);
                CHECK((dd->graphic_queue->flags & VK_QUEUE_GRAPHICS_BIT) != 0);
        }
        CHECK(find_object_data(HKEY(FAKE_QUEUE)) != NULL);

        /* loader bookkeeping was handed the queue */
        CHECK_EQ(stub_set_loader_data_calls, 1);
        CHECK_EQ((uintptr_t)stub_last_loader_data_queue,
                 (uintptr_t)FAKE_QUEUE);

        /* destroy is captured from the chain (loader resolves it directly) */
        CHECK_PTR(dd->chain_DestroyDevice, (void*)stub_destroy_device);

        /* device-stable resources came back with the stub handles */
        CHECK_EQ((uintptr_t)dd->crosshair_sampler, (uintptr_t)FAKE_SAMPLER);
        CHECK_EQ((uintptr_t)dd->descriptor_pool, (uintptr_t)FAKE_DESC_POOL);
        CHECK_EQ((uintptr_t)dd->descriptor_layout, (uintptr_t)FAKE_DESC_LAYOUT);
        CHECK_EQ((uintptr_t)dd->pipeline_layout, (uintptr_t)FAKE_PIPE_LAYOUT);
        CHECK_EQ((uintptr_t)dd->shader_desc_pool, (uintptr_t)FAKE_DESC_POOL);
        CHECK_EQ((uintptr_t)dd->shader_desc_layout, (uintptr_t)FAKE_DESC_LAYOUT);
        CHECK_EQ((uintptr_t)dd->shader_pipeline_layout, (uintptr_t)FAKE_PIPE_LAYOUT);
        CHECK_EQ((uintptr_t)dd->cmd_pool, (uintptr_t)FAKE_CMD_POOL);
}

static void test_gdpa_fallthrough_after_device(void)
{
        VkDevice dev = FAKE_DEVICE;

        /* intercepted names win even with a live device */
        CHECK_PTR(overlay_GetDeviceProcAddr(dev, "vkCreateSwapchainKHR"),
                  find_ptr("vkCreateSwapchainKHR"));
        /* non-intercepted names fall through to the chain GDPA */
        CHECK_PTR(overlay_GetDeviceProcAddr(dev, "vkGetDeviceQueue"),
                  (void*)stub_get_device_queue);
        CHECK(overlay_GetDeviceProcAddr(dev, "vkTotallyBogus") == NULL);
}

static void test_destroy_device(void)
{
        overlay_DestroyDevice(FAKE_DEVICE, NULL);
        CHECK_EQ(stub_destroy_device_calls, 1);
        CHECK(find_object_data(HKEY(FAKE_DEVICE)) == NULL);
        /* per-queue state is unmapped with the device */
        CHECK(find_object_data(HKEY(FAKE_QUEUE)) == NULL);
}

static void test_destroy_instance(void)
{
        overlay_DestroyInstance(FAKE_INSTANCE, NULL);
        CHECK_EQ(stub_destroy_instance_calls, 1);
        CHECK(find_object_data(HKEY(FAKE_INSTANCE)) == NULL);
        CHECK(find_object_data(HKEY(FAKE_PHYS)) == NULL);
}

/* A chain that can't even resolve vkCreateInstance must fail cleanly. */
static void test_create_instance_no_next(void)
{
        instance_link.pNext = NULL;
        instance_link.pfnNextGetInstanceProcAddr = stub_instance_gpa_empty;
        instance_chain.sType    = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
        instance_chain.function = VK_LAYER_LINK_INFO;
        instance_chain.u.pLayerInfo = &instance_link;

        VkInstanceCreateInfo ci = { 0 };
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pNext = &instance_chain;

        VkInstance inst = VK_NULL_HANDLE;
        CHECK_EQ((int)overlay_CreateInstance(&ci, NULL, &inst),
                 (int)VK_ERROR_INITIALIZATION_FAILED);
        CHECK(inst == VK_NULL_HANDLE);
}

int main(void)
{
        fprintf(stderr, "# test_layer_chain\n");
        test_create_instance();
        test_gpa_fallthrough_after_instance();
        test_create_device();
        test_gdpa_fallthrough_after_device();
        test_destroy_device();
        test_destroy_instance();
        test_create_instance_no_next();
        fprintf(stderr, "  -> %d passed, %d failed\n", test_passed, test_failed);
        return test_failed ? 1 : 0;
}
