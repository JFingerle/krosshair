/*
 * Memory-usage tracking for the KROSSHAIR_PERFLOGGING env var.
 *
 * When KROSSHAIR_PERFLOGGING=1, the layer prints one [KH]-prefixed line
 * per allocation/free of its own memory to stderr, so leaks can be
 * observed at runtime. Only the layer's internal call sites are
 * instrumented — the game's own allocations go through the same Vulkan
 * entry points but are not logged.
 *
 * Device memory is tracked through a handle->size registry because the
 * Vulkan free call carries no size; host objects are logged at their
 * call sites with a short category name. All tracked sites run on the
 * render thread, so no locking is needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/krosshair.h"

typedef struct {
        VkDeviceMemory handle;
        size_t size;
} perflog_dev_entry_t;

static int perflog_enabled = 0;
static size_t perflog_dev_total = 0;
static size_t perflog_host_total = 0;
static perflog_dev_entry_t* dev_entries = NULL;
static size_t dev_count = 0;
static size_t dev_cap = 0;

/*
 * Read KROSSHAIR_PERFLOGGING and enable tracking when it is "1".
 * Called once from overlay_CreateInstance, before any tracked
 * allocation happens.
 */
void kh_perflog_init(void)
{
        const char* value = getenv("KROSSHAIR_PERFLOGGING");
        perflog_enabled = (value != NULL && strcmp(value, "1") == 0);
        if (perflog_enabled)
                fprintf(stderr,
                        "[KH] perf: memory tracking enabled "
                        "(KROSSHAIR_PERFLOGGING=1)\n");
}

/*
 * Whether tracking is enabled.
 *
 * Returns 1 when KROSSHAIR_PERFLOGGING was "1" at init time.
 */
int kh_perflog_enabled(void)
{
        return perflog_enabled;
}

/*
 * Record a device memory allocation: remember its size for the free
 * side and log the new running total.
 *
 * mem:   the allocated VkDeviceMemory handle;
 * size:  allocation size in bytes.
 */
void kh_perflog_dev_alloc(VkDeviceMemory mem, size_t size)
{
        if (!perflog_enabled || mem == VK_NULL_HANDLE)
                return;

        if (dev_count == dev_cap) {
                size_t new_cap = dev_cap ? dev_cap * 2 : 8;
                perflog_dev_entry_t* grown =
                    realloc(dev_entries, new_cap * sizeof(*grown));
                if (!grown)
                        return;
                dev_entries = grown;
                dev_cap = new_cap;
        }
        dev_entries[dev_count].handle = mem;
        dev_entries[dev_count].size = size;
        dev_count++;

        perflog_dev_total += size;
        fprintf(stderr,
                "[KH] perf: device memory allocated %zu bytes "
                "(total %zu bytes)\n",
                size, perflog_dev_total);
}

/*
 * Record a device memory free: look up the size recorded at allocation
 * time (Vulkan frees carry no size) and log the new running total.
 * Untracked handles are ignored.
 *
 * mem: the VkDeviceMemory handle being freed.
 */
void kh_perflog_dev_free(VkDeviceMemory mem)
{
        if (!perflog_enabled || mem == VK_NULL_HANDLE)
                return;

        for (size_t i = 0; i < dev_count; i++) {
                if (dev_entries[i].handle == mem) {
                        size_t size = dev_entries[i].size;
                        perflog_dev_total -= size;
                        dev_entries[i] = dev_entries[dev_count - 1];
                        dev_count--;
                        fprintf(stderr,
                                "[KH] perf: device memory freed %zu bytes "
                                "(total %zu bytes)\n",
                                size, perflog_dev_total);
                        return;
                }
        }
}

/*
 * Log a host-side allocation of a named object category.
 *
 * size: bytes allocated;
 * what: short category name for the log line.
 */
void kh_perflog_host_alloc(size_t size, const char* what)
{
        if (!perflog_enabled)
                return;
        perflog_host_total += size;
        fprintf(stderr,
                "[KH] perf: host memory allocated %zu bytes (%s) "
                "(total %zu bytes)\n",
                size, what, perflog_host_total);
}

/*
 * Log a host-side free of a named object category.
 *
 * size: bytes freed;
 * what: short category name (must match the allocation's).
 */
void kh_perflog_host_free(size_t size, const char* what)
{
        if (!perflog_enabled)
                return;
        perflog_host_total -= size;
        fprintf(stderr,
                "[KH] perf: host memory freed %zu bytes (%s) "
                "(total %zu bytes)\n",
                size, what, perflog_host_total);
}

/*
 * Current running total of live layer device memory, in bytes.
 */
size_t kh_perflog_dev_total(void)
{
        return perflog_dev_total;
}

/*
 * Current running total of live layer host memory, in bytes.
 */
size_t kh_perflog_host_total(void)
{
        return perflog_host_total;
}
