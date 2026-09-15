/*
 * Unit tests for the layer's command lookup table (src/layer.c).
 *
 * These are pure lookup tests — no Vulkan driver, no chains: the
 * name_to_funcptr_map is walked entry-by-entry and the two Get*ProcAddr
 * entry points are exercised with NULL handles (which must short-circuit
 * on the intercepted names before touching any chain).
 *
 * Built with -DUNIT_TEST so layer.c exports find_ptr / name_to_funcptr_map.
 */
#define _GNU_SOURCE

#include <stdint.h>

#include "test.h"
#include "test_layer_api.h"

/* Every registered command resolves to a non-NULL, non-NULL-name entry. */
static void test_map_integrity(void)
{
        CHECK(name_to_funcptr_map_count > 0);
        for (size_t i = 0; i < name_to_funcptr_map_count; i++) {
                CHECK(name_to_funcptr_map[i].name != NULL);
                CHECK(name_to_funcptr_map[i].func != NULL);
                /* find_ptr must return the very pointer stored in the map */
                CHECK_PTR(find_ptr(name_to_funcptr_map[i].name),
                          name_to_funcptr_map[i].func);
        }
}

/* No two map entries may share a name (lookup is linear-first-match). */
static void test_no_duplicate_names(void)
{
        int duplicates = 0;
        for (size_t i = 0; i < name_to_funcptr_map_count && !duplicates; i++)
                for (size_t j = i + 1; j < name_to_funcptr_map_count; j++)
                        if (strcmp(name_to_funcptr_map[i].name,
                                   name_to_funcptr_map[j].name) == 0)
                                duplicates++;
        CHECK_EQ(duplicates, 0);
}

/* Known intercepted names resolve to the actual override functions. */
static void test_find_ptr_known_names(void)
{
        CHECK_PTR(find_ptr("vkCreateInstance"), (void*)overlay_CreateInstance);
        CHECK_PTR(find_ptr("vkCreateDevice"), (void*)overlay_CreateDevice);
        CHECK_PTR(find_ptr("vkDestroyDevice"), (void*)overlay_DestroyDevice);
        CHECK_PTR(find_ptr("vkDestroyInstance"), (void*)overlay_DestroyInstance);
        CHECK_PTR(find_ptr("vkGetInstanceProcAddr"),
                  (void*)overlay_GetInstanceProcAddr);
        CHECK_PTR(find_ptr("vkGetDeviceProcAddr"),
                  (void*)overlay_GetDeviceProcAddr);
}

/* Unknown / malformed names must return NULL, not garbage. */
static void test_find_ptr_unknown_names(void)
{
        CHECK(find_ptr("vkTotallyBogusCommand") == NULL);
        CHECK(find_ptr("") == NULL);
        CHECK(find_ptr("CreateDevice") == NULL); /* missing vk prefix */
        CHECK(find_ptr("vkcreatedevice") == NULL); /* case-sensitive */
}

/*
 * With a NULL handle there is no chain to fall through to, so:
 * intercepted names must return the override, everything else NULL.
 */
static void test_gpa_null_instance(void)
{
        CHECK_PTR(overlay_GetInstanceProcAddr(NULL, "vkCreateDevice"),
                  (void*)overlay_CreateDevice);
        CHECK(overlay_GetInstanceProcAddr(NULL, "vkTotallyBogusCommand") == NULL);
}

static void test_gdpa_null_device(void)
{
        CHECK_PTR(overlay_GetDeviceProcAddr(NULL, "vkQueuePresentKHR"),
                  find_ptr("vkQueuePresentKHR"));
        CHECK(overlay_GetDeviceProcAddr(NULL, "vkTotallyBogusCommand") == NULL);
}

int main(void)
{
        fprintf(stderr, "# test_dispatch\n");
        test_map_integrity();
        test_no_duplicate_names();
        test_find_ptr_known_names();
        test_find_ptr_unknown_names();
        test_gpa_null_instance();
        test_gdpa_null_device();
        fprintf(stderr, "  -> %d passed, %d failed\n", test_passed, test_failed);
        return test_failed ? 1 : 0;
}
