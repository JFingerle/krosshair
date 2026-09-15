/*
 * Unit tests for the Vulkan object bookkeeping (src/objects.c).
 *
 * Exercises vk_map_set / vk_map_get / vk_map_delete and their thread-safe
 * wrappers map_object / find_object_data / unmap_object, including the
 * update-in-place and map-full edge cases.
 */
#define _GNU_SOURCE

#include <stdint.h>

#include "test.h"
#include "../include/krosshair.h"

typedef struct dummy {
        int v;
} dummy_t;

static void test_map_set_get_delete(void)
{
        dummy_t d = { 42 };

        CHECK(vk_map_set(&vk_obj_map, 0x1001, &d, "test") >= 0);

        vk_object_t obj;
        CHECK_EQ(vk_map_get(&vk_obj_map, 0x1001, &obj), 1);
        CHECK_PTR(obj.data, &d);
        CHECK_STR(obj.name, "test");
        CHECK_EQ((int)obj.obj, 0x1001);

        CHECK_PTR(find_object_data(0x1001), &d);

        /* unknown handle: no entry */
        CHECK_EQ(vk_map_get(&vk_obj_map, 0x9999, &obj), 0);
        CHECK(find_object_data(0x9999) == NULL);

        vk_map_delete(&vk_obj_map, 0x1001);
        CHECK(find_object_data(0x1001) == NULL);
}

/* vk_map_set on an existing handle updates data/name in place. */
static void test_map_set_updates_in_place(void)
{
        dummy_t a = { 1 }, b = { 2 };

        CHECK(vk_map_set(&vk_obj_map, 0x2001, &a, "first") >= 0);
        CHECK(vk_map_set(&vk_obj_map, 0x2001, &b, "second") >= 0);

        vk_object_t obj;
        CHECK_EQ(vk_map_get(&vk_obj_map, 0x2001, &obj), 1);
        CHECK_PTR(obj.data, &b);
        CHECK_STR(obj.name, "second");

        vk_map_delete(&vk_obj_map, 0x2001);
}

/* The map is capped at MAX_VK_OBJECTS; overflow is rejected, not clobbered. */
static void test_map_full_rejects_overflow(void)
{
        size_t free_before = 0;
        for (size_t i = 0; i < MAX_VK_OBJECTS; i++)
                if (vk_obj_map.data[i].obj == 0)
                        free_before++;

        uint64_t start = 0x500000;
        int inserted = 0;
        for (uint64_t i = 0; i < free_before; i++) {
                if (vk_map_set(&vk_obj_map, start + i,
                               (void*)(uintptr_t)(i + 1), "fill") >= 0)
                        inserted++;
        }
        CHECK_EQ((size_t)inserted, free_before);

        /* map is now full: the next distinct handle must be rejected */
        CHECK(vk_map_set(&vk_obj_map, 0x600000, (void*)1, "overflow") == -1);

        /* cleanup: remove everything we inserted */
        for (uint64_t i = 0; i < free_before; i++)
                vk_map_delete(&vk_obj_map, start + i);
        CHECK(vk_map_set(&vk_obj_map, 0x600000, (void*)1, "refit") >= 0);
        vk_map_delete(&vk_obj_map, 0x600000);
}

static void test_map_object_helpers(void)
{
        dummy_t d = { 7 };

        map_object(0x3001, &d, "helper");
        CHECK_PTR(find_object_data(0x3001), &d);

        unmap_object(0x3001);
        CHECK(find_object_data(0x3001) == NULL);

        /* unmap of an unknown handle is a no-op, not an error */
        unmap_object(0x7777);
        CHECK(find_object_data(0x7777) == NULL);
}

/* new_*_data allocators register themselves in the global map. */
static void test_new_instance_data_registers(void)
{
        VkInstance fake = (VkInstance)(uintptr_t)0x4001;
        instance_data_t* id = new_instance_data(fake);

        CHECK(id != NULL);
        if (id)
                CHECK_EQ((uintptr_t)id->instance, (uintptr_t)fake);
        CHECK_PTR(find_object_data(HKEY(fake)), id);

        unmap_object(HKEY(fake));
        free(id);
}

static void test_new_device_data_registers(void)
{
        VkDevice fake = (VkDevice)(uintptr_t)0x4002;
        device_data_t* dd = new_device_data(fake, NULL);

        CHECK(dd != NULL);
        if (dd) {
                CHECK_EQ((uintptr_t)dd->device, (uintptr_t)fake);
                CHECK(dd->instance == NULL);
        }
        CHECK_PTR(find_object_data(HKEY(fake)), dd);

        unmap_object(HKEY(fake));
        free(dd);
}

int main(void)
{
        fprintf(stderr, "# test_objects\n");
        test_map_set_get_delete();
        test_map_set_updates_in_place();
        test_map_full_rejects_overflow();
        test_map_object_helpers();
        test_new_instance_data_registers();
        test_new_device_data_registers();
        fprintf(stderr, "  -> %d passed, %d failed\n", test_passed, test_failed);
        return test_failed ? 1 : 0;
}
