/*
 * tests/test_mock_icd.c — end-to-end test of the krosshair layer against
 * a mock Vulkan ICD (tests/mock_icd.c) through the real Vulkan loader.
 *
 * The layer is enabled with KROSSHAIR=1 + VK_LAYER_PATH; the mock driver
 * is selected with VK_DRIVER_FILES. The test drives the full
 * instance -> device -> swapchain -> acquire -> submit -> present cycle
 * and then reads the ICD's call counters (via dlopen + dlsym on the same
 * .so the loader dlopened) to assert the layer actually rendered and
 * forwarded: the app submits one empty submit, and the layer must add
 * its own overlay submit on present.
 *
 * Run via `make test` (it sets the environment); running it standalone
 * requires the env vars set by the Makefile.
 *
 * Custom-image angle: when KROSSHAIR_E2E_CUSTOM_IMG is set, the test
 * generates a small 3-frame 11x13 APNG, writes it to a temp file and
 * points KROSSHAIR_IMG at it. The layer decodes it into an 11x(13*3)
 * vertical frame atlas and uploads it; that upload buffer becomes the
 * largest single device-memory allocation, which differs from the
 * built-in 50x50 crosshair's — so the max allocation observed by the
 * mock identifies which image the layer loaded.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include "apng_fixture.h"
#include "test.h"

typedef int (*stats_fn)(int* submits, int* cmdbufs, int* presents,
                         int* swapchains, int* acquires, int* queue_queries,
                         VkDeviceSize* max_alloc);

/*
 * Check a VkResult for success (VkResult is small, CHECK_EQ prints it).
 *
 * result  The VkResult to validate.
 *
 * Returns 1 on VK_SUCCESS, 0 otherwise.
 */
static int ok(VkResult result)
{
        return result == VK_SUCCESS;
}

/*
 * Check whether the krosshair layer .so is actually mapped into this
 * process, i.e. whether the loader loaded it as an implicit layer.
 * Some packaged loaders enumerate the layer manifest but silently skip
 * loading the layer (observed with Debian's libvulkan 1.4.309), in which
 * case the layer's overlay submits never reach the ICD and the stats
 * checks below cannot pass, so the end-to-end angle must be skipped
 * instead of failing.
 *
 * Returns 1 when the layer is mapped (or the check cannot be made),
 * 0 when the loader demonstrably did not load it.
 */
static int layer_loaded_in_process(void)
{
        FILE* f = fopen("/proc/self/maps", "r");
        if (!f)
                return 1;
        char line[512];
        int loaded = 0;
        while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "krosshair.so"))
                        loaded = 1;
        }
        fclose(f);
        return loaded;
}

static void test_mock_icd(void)
{
        /* open the ICD the loader will also dlopen; dlopen dedupes by
         * inode, so we share its process-global call counters. */
        const char* so = getenv("MOCK_ICD_SO");
        CHECK(so != NULL);
        void* handle = so ? dlopen(so, RTLD_NOW | RTLD_LOCAL) : NULL;
        CHECK(handle != NULL);
        stats_fn stats =
            (stats_fn)dlsym(handle, "mock_icd_get_stats");
        CHECK(stats != NULL);

        /* ---- optional custom crosshair image ----

         * When KROSSHAIR_E2E_CUSTOM_IMG is set, generate a 3-frame 11x13
         * APNG, write it to a temp file and point KROSSHAIR_IMG at it.
         * The layer must decode it into an 11x(13*3) vertical frame
         * atlas; the resulting upload-buffer allocation differs from the
         * built-in 50x50 crosshair's (asserted below).
         */
        int use_custom_image = getenv("KROSSHAIR_E2E_CUSTOM_IMG") != NULL;
        char custom_path[64] = "";
        if (use_custom_image) {
                /* 3 frames of 11x13 RGBA, each a flat field of a different
                 * value; 1/10 s delay per frame. */
                unsigned char frame0[11 * 13 * 4];
                unsigned char frame1[11 * 13 * 4];
                unsigned char frame2[11 * 13 * 4];
                memset(frame0, 0x33, sizeof(frame0));
                memset(frame1, 0x66, sizeof(frame1));
                memset(frame2, 0x99, sizeof(frame2));
                const unsigned char* frames[3] = { frame0, frame1, frame2 };
                uint16_t delay_nums[3] = { 1, 1, 1 };
                uint16_t delay_dens[3] = { 10, 10, 10 };
                /* 8 (SIG) + 25 (IHDR) + 20 (acTL)
                 * + 3 * (38 (fcTL) + 12 (fdAT hdr) + 4 (seq) + 7 (zlib)
                 *        + 11 * 13 * (4 * 11 + 1) (raw scanlines))
                 * + 12 (IEND) */
                unsigned char apng[19600];
                size_t apng_len = build_apng(apng, 11, 13, 3, frames,
                                             delay_nums, delay_dens);
                CHECK(apng_len > 0);
                /* decode_crosshair_file dispatches on the file
                 * extension, so the fixture needs a .apng suffix
                 * (mkstemp's trailing-XXXXXX rule rules out a suffix) */
                const char* custom_name = "/tmp/kh_e2e_custom.apng";
                FILE* f = fopen(custom_name, "wb");
                CHECK(f != NULL);
                CHECK(fwrite(apng, 1, apng_len, f) == apng_len);
                fclose(f);
                snprintf(custom_path, sizeof(custom_path), "%s", custom_name);
                setenv("KROSSHAIR_IMG", custom_path, 1);
        }

        /* ---- instance ---- */

        VkInstance instance = VK_NULL_HANDLE;
        VkApplicationInfo app_info = {
            .sType          = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "mock-icd-test",
            .apiVersion     = VK_API_VERSION_1_3,
        };
        VkInstanceCreateInfo instance_info = {
            .sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app_info,
        };
        CHECK(ok(vkCreateInstance(&instance_info, NULL, &instance)));
        CHECK(instance != VK_NULL_HANDLE);

        /* If the loader did not load the layer, no correct application
         * behavior can make the stats checks below pass. */
        if (!layer_loaded_in_process()) {
                fprintf(stderr,
                        "[test] loader did not load krosshair.so; "
                        "skipping end-to-end angle\n");
                fflush(stderr);
                if (use_custom_image)
                        remove(custom_path);
                return;
        }

        VkPhysicalDevice physical = VK_NULL_HANDLE;
        uint32_t device_count = 0;
        CHECK(ok(vkEnumeratePhysicalDevices(instance, &device_count, NULL)));
        CHECK_EQ(device_count, 1);
        CHECK(ok(vkEnumeratePhysicalDevices(
            instance, &device_count, &physical)));
        CHECK(physical != VK_NULL_HANDLE);

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        CHECK(props.apiVersion >= VK_MAKE_VERSION(1, 1, 0));

        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        CHECK(mem.memoryTypeCount >= 1);
        CHECK(mem.memoryHeapCount >= 1);

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            physical, &family_count, NULL);
        CHECK(family_count >= 1);
        VkQueueFamilyProperties family;
        vkGetPhysicalDeviceQueueFamilyProperties(
            physical, &family_count, &family);
        CHECK(family.queueFlags & VK_QUEUE_GRAPHICS_BIT);

        /* ---- device ---- */

        VkDevice device = VK_NULL_HANDLE;
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0,
            .queueCount       = 1,
            .pQueuePriorities = &priority,
        };
        const char* extensions[] = { "VK_KHR_swapchain" };
        VkDeviceCreateInfo device_info = {
            .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount    = 1,
            .pQueueCreateInfos       = &queue_info,
            .enabledExtensionCount   = 1,
            .ppEnabledExtensionNames = extensions,
        };
        CHECK(ok(vkCreateDevice(physical, &device_info, NULL, &device)));
        CHECK(device != VK_NULL_HANDLE);

        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, 0, 0, &queue); /* void-returning */
        CHECK(queue != VK_NULL_HANDLE);

        /* ---- swapchain (fake surface handle; the mock ignores it) ---- */

        VkSurfaceKHR surface = (VkSurfaceKHR)(uintptr_t)0xC0FFEE01;
        VkSwapchainCreateInfoKHR swapchain_info = {
            .sType           = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface         = surface,
            .minImageCount   = 3,
            .imageFormat     = VK_FORMAT_B8G8R8A8_UNORM,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            .imageExtent     = { 320, 240 },
            .imageArrayLayers = 1,
            .imageUsage      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform    = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha  = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode     = VK_PRESENT_MODE_IMMEDIATE_KHR,
            .clipped         = VK_TRUE,
            .oldSwapchain    = VK_NULL_HANDLE,
        };
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        CHECK(ok(vkCreateSwapchainKHR(device, &swapchain_info, NULL,
                                      &swapchain)));
        CHECK(swapchain != VK_NULL_HANDLE);

        /* ---- two full frames: acquire, app submit, present ---- */

        for (int frame = 0; frame < 2; frame++) {
                uint32_t index = 0;
                CHECK(ok(vkAcquireNextImageKHR(device, swapchain,
                                               UINT32_MAX, VK_NULL_HANDLE,
                                               VK_NULL_HANDLE, &index)));
                CHECK_EQ(index, 0);

                /* the app's own (empty) submit flows through the loader
                 * to the ICD untouched — the layer does not intercept
                 * QueueSubmit, only adds its own overlay submit on
                 * present. */
                VkSubmitInfo submit = {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                };
                CHECK(ok(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)));

                VkPresentInfoKHR present = {
                    .sType          = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                    .swapchainCount = 1,
                    .pSwapchains    = &swapchain,
                    .pImageIndices  = &index,
                };
                CHECK(ok(vkQueuePresentKHR(queue, &present)));
        }

        /* ---- assert the ICD actually saw the layer's work ---- */

        int submits = 0, cmdbufs = 0, presents = 0;
        int swapchains = 0, acquires = 0, queue_queries = 0;
        VkDeviceSize max_alloc = 0;
        CHECK(stats(&submits, &cmdbufs, &presents, &swapchains, &acquires,
                    &queue_queries, &max_alloc) == 0);
        /* 2 app submits (empty) + >= 1 overlay submit per present */
        CHECK(submits >= 4);
        /* the layer recorded and submitted its own draw command buffer
         * for each present — without it cmdbufs would be 0 */
        CHECK(cmdbufs >= 2);
        CHECK_EQ(presents, 2);
        CHECK_EQ(swapchains, 1);
        CHECK_EQ(acquires, 2);
        CHECK(queue_queries >= 1);

        /* The layer's crosshair upload buffer is the largest single
         * device-memory allocation (image memory stays 1024 in the
         * mock): its size is exactly the decoded atlas — 11x(13*3)x4
         * = 1716 bytes for the custom APNG, 50x50x4 = 10000 bytes for
         * the built-in crosshair. Proves the layer decoded the file
         * KROSSHAIR_IMG points at (or fell back to the built-in
         * image). */
        VkDeviceSize expected = use_custom_image
            ? (VkDeviceSize)(11 * 13 * 3 * 4)
            : (VkDeviceSize)(50 * 50 * 4);
        CHECK_EQ((int)max_alloc, (int)expected);

        if (use_custom_image)
                remove(custom_path);

        /* ---- teardown ---- */

        fprintf(stderr, "[trace] before DestroySwapchain\n");
        fflush(stderr);
        vkDestroySwapchainKHR(device, swapchain, NULL); /* void-returning */
        fprintf(stderr, "[trace] before DeviceWaitIdle\n");
        fflush(stderr);
        CHECK(ok(vkDeviceWaitIdle(device)));
        fprintf(stderr, "[trace] before DestroyDevice\n");
        fflush(stderr);
        vkDestroyDevice(device, NULL);
        fprintf(stderr, "[trace] before DestroyInstance\n");
        fflush(stderr);
        vkDestroyInstance(instance, NULL);
        fprintf(stderr, "[trace] before dlclose\n");
        fflush(stderr);
        dlclose(handle);
        fprintf(stderr, "[trace] after dlclose\n");
        fflush(stderr);
}

TESTS_MAIN(test_mock_icd)
