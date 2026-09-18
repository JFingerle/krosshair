/*
 * Unit tests for the KROSSHAIR_PERFLOGGING memory-usage tracking
 * (src/perflog.c): env-var parsing, the device-memory handle->size
 * registry, and the host-memory running totals.
 */

#define _GNU_SOURCE

#include <stdlib.h>

#include "../include/krosshair.h"

#include "test.h"

static void test_perflog(void)
{
        int dummy_a, dummy_b, dummy_c;
        VkDeviceMemory mem_a = (VkDeviceMemory)&dummy_a;
        VkDeviceMemory mem_b = (VkDeviceMemory)&dummy_b;
        VkDeviceMemory mem_c = (VkDeviceMemory)&dummy_c;

        /* env parsing: only the exact value "1" enables tracking */
        unsetenv("KROSSHAIR_PERFLOGGING");
        kh_perflog_init();
        CHECK(!kh_perflog_enabled());

        setenv("KROSSHAIR_PERFLOGGING", "1", 1);
        kh_perflog_init();
        CHECK(kh_perflog_enabled());

        setenv("KROSSHAIR_PERFLOGGING", "0", 1);
        kh_perflog_init();
        CHECK(!kh_perflog_enabled());

        setenv("KROSSHAIR_PERFLOGGING", "true", 1);
        kh_perflog_init();
        CHECK(!kh_perflog_enabled());

        /* disabled: allocations are not counted */
        size_t base = kh_perflog_dev_total();
        kh_perflog_dev_alloc(mem_a, 4096);
        CHECK_EQ(kh_perflog_dev_total(), base);

        /* enabled: device allocs accumulate, frees subtract the size
         * recorded at alloc time */
        setenv("KROSSHAIR_PERFLOGGING", "1", 1);
        kh_perflog_init();
        base = kh_perflog_dev_total();

        kh_perflog_dev_alloc(mem_a, 4096);
        CHECK_EQ(kh_perflog_dev_total(), base + 4096);
        kh_perflog_dev_alloc(mem_b, 8192);
        CHECK_EQ(kh_perflog_dev_total(), base + 4096 + 8192);

        kh_perflog_dev_free(mem_a);
        CHECK_EQ(kh_perflog_dev_total(), base + 8192);

        /* freeing an untracked handle is a no-op */
        kh_perflog_dev_free(mem_c);
        CHECK_EQ(kh_perflog_dev_total(), base + 8192);

        /* freeing the same handle twice is a no-op after the first */
        kh_perflog_dev_free(mem_b);
        CHECK_EQ(kh_perflog_dev_total(), base);
        kh_perflog_dev_free(mem_b);
        CHECK_EQ(kh_perflog_dev_total(), base);

        /* NULL handles are ignored on both sides */
        kh_perflog_dev_alloc(VK_NULL_HANDLE, 16);
        kh_perflog_dev_free(VK_NULL_HANDLE);
        CHECK_EQ(kh_perflog_dev_total(), base);

        /* host totals: allocs accumulate, frees subtract */
        size_t host_base = kh_perflog_host_total();
        kh_perflog_host_alloc(100, "test object");
        CHECK_EQ(kh_perflog_host_total(), host_base + 100);
        kh_perflog_host_free(100, "test object");
        CHECK_EQ(kh_perflog_host_total(), host_base);

        unsetenv("KROSSHAIR_PERFLOGGING");
}

TESTS_MAIN(test_perflog)
