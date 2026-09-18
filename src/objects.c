/*
 * Vulkan object bookkeeping.
 *
 * Owns the vk_obj_map that associates Vulkan handles with per-instance
 * and per-device state, and provides the allocation of that state
 * (instance, device, queue, command-buffer data) plus queue discovery
 * and chain-info lookup used by the layer entry points.
 */

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/krosshair.h"

/*
 * Store per-object data in the map for a Vulkan handle.
 *
 * If the handle is already present its data and name are updated in
 * place; otherwise the first free slot is taken.
 *
 * map   Object map to update.
 * obj   Vulkan handle (as an integer key) to store data for.
 * data  Pointer to the layer's per-object state.
 * name  Human-readable tag for this entry (debugging only).
 *
 * Returns the index of the stored entry, or -1 if the map is full.
 */
int vk_map_set(vk_object_map_t* map, uint64_t obj, void* data, const char* name)
{
        /* update an existing entry */
        for (size_t i = 0; i < MAX_VK_OBJECTS; i++) {
                if (map->data[i].obj == obj) {
                        map->data[i].data = data;
                        map->data[i].name = name;
                        return i;
                }
        }

        /* find the first free slot for a new entry */
        for (size_t i = 0; i < MAX_VK_OBJECTS; i++) {
                if (map->data[i].obj == 0) {
                        map->data[i].obj  = obj;
                        map->data[i].data = data;
                        map->data[i].name = name;
                        map->count++;
                        return i;
                }
        }

        return -1;
}

/*
 * Look up the per-object entry for a Vulkan handle.
 *
 * map    Object map to search.
 * obj    Vulkan handle (as an integer key) to look up.
 * obj_out Receives a copy of the matching entry.
 *
 * Returns 1 on success, 0 if the handle is not mapped.
 */
int vk_map_get(vk_object_map_t* map, uint64_t obj, vk_object_t* obj_out)
{
        for (size_t i = 0; i < MAX_VK_OBJECTS; i++) {
                if (map->data[i].obj == obj) {
                        *obj_out = map->data[i];
                        return 1;
                }
        }

        return 0;
}

/* Log every live entry of the object map (debugging aid). */
void vk_map_print(vk_object_map_t* map)
{
        KROSSHAIR_LOG(
                "#################################### [ITERATING MAP] "
                "######################################\n");
        for (size_t i = 0; i < MAX_VK_OBJECTS; i++) {
                if (map->data[i].obj == 0) continue;

                KROSSHAIR_LOG("item %lu (%s) has obj: \t\t %lu, data \t\t %p\n", i,
                        map->data[i].name, map->data[i].obj, map->data[i].data);
        }
        KROSSHAIR_LOG(
                "##################################################################"
                "####"
                "#####################\n");
}

/* Remove the mapping for a Vulkan handle (the data pointer is NOT freed). */
void vk_map_delete(vk_object_map_t* map, uint64_t obj)
{
        for (size_t i = 0; i < MAX_VK_OBJECTS; i++) {
                if (map->data[i].obj == obj) {
                        map->data[i].obj  = 0;
                        map->data[i].data = NULL;
                        map->data[i].name = NULL;
                        map->count--;
                }
        }
}

vk_object_map_t vk_obj_map;

/*
 * Thread-safe lookup of the per-object state for a Vulkan handle.
 * Returns the stored data pointer, or NULL if the handle is not mapped.
 */
void* find_object_data(uint64_t obj)
{
        pthread_mutex_lock(&global_lock);

        vk_object_t entry;
        if (!vk_map_get(&vk_obj_map, obj, &entry)) {
                pthread_mutex_unlock(&global_lock);
                return NULL;
        }

        pthread_mutex_unlock(&global_lock);
        return entry.data;
}

/*
 * Thread-safe mapping of a Vulkan handle to per-object state.
 * Silently drops the mapping if the object map is full.
 */
void map_object(uint64_t obj, void* data, const char* name)
{
        pthread_mutex_lock(&global_lock);

        if (vk_map_set(&vk_obj_map, obj, data, name) == -1) {
                pthread_mutex_unlock(&global_lock);
                return;
        }
        pthread_mutex_unlock(&global_lock);
}

/* Thread-safe removal of a Vulkan handle's mapping. */
void unmap_object(uint64_t obj)
{
        pthread_mutex_lock(&global_lock);
        vk_map_delete(&vk_obj_map, obj);
        pthread_mutex_unlock(&global_lock);
}

/*
 * Find this layer's link in the instance-creation struct chain.
 *
 * The loader prepends a VkLayerInstanceCreateInfo to pNext before calling
 * vkCreateInstance; it identifies which link of the chain leads to
 * `func`, so the layer can call the rest of the chain without skipping
 * itself.
 *
 * create_info  Instance creation info passed to vkCreateInstance.
 * func         Chain function we are intercepting (vkCreateInstance).
 *
 * Returns the matching chain struct, or NULL if not present.
 */
VkLayerInstanceCreateInfo* get_instance_chain_info(
    const VkInstanceCreateInfo* create_info, VkLayerFunction func)
{
        vk_foreach_struct(item, create_info->pNext)
        {
                if (item->sType ==
                        VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                    ((VkLayerInstanceCreateInfo*)item)->function == func)
                        return (VkLayerInstanceCreateInfo*)item;
        }
        return NULL;
}

/*
 * Allocate and register the per-instance state.
 *
 * instance  The VkInstance this state belongs to.
 *
 * Returns a zeroed instance_data_t registered in the object map under
 * the VkInstance handle.
 */
instance_data_t* new_instance_data(VkInstance instance)
{
        instance_data_t* instance_data = malloc(sizeof(instance_data_t));
        memset(instance_data, 0, sizeof(*instance_data));
        kh_perflog_host_alloc(sizeof(instance_data_t), "instance data");
        instance_data->instance = instance;
        KROSSHAIR_LOG("[*] mapping data->instance obj: %lu data: %p\n",
                HKEY(instance_data->instance), (void*)instance_data);
        map_object(HKEY(instance_data->instance), instance_data,
                "instance_data->instance");
        return instance_data;
}

/*
 * Allocate and register the per-device state.
 *
 * device    The VkDevice this state belongs to.
 * instance  Per-instance state the device was created from.
 *
 * Returns a zeroed device_data_t registered in the object map under
 * the VkDevice handle.
 */
device_data_t* new_device_data(VkDevice device, instance_data_t* instance)
{
        device_data_t* device_data = malloc(sizeof(device_data_t));
        memset(device_data, 0, sizeof(*device_data));
        kh_perflog_host_alloc(sizeof(device_data_t), "device data");
        device_data->instance = instance;
        device_data->device   = device;
        KROSSHAIR_LOG("[*] mapping data->device obj: %lu %p\n",
                HKEY(device_data->device), (void*)device_data);
        map_object(HKEY(device_data->device), device_data,
                "device_data->device");
        return device_data;
}

/*
 * Allocate and register the per-command-buffer state.
 *
 * cmd_buffer  The VkCommandBuffer this state belongs to.
 * level       Command buffer level (primary/secondary) it was created with.
 * device_data Per-device state the command buffer was allocated from.
 *
 * Returns a zeroed cmd_buffer_data_t registered in the object map under
 * the VkCommandBuffer handle.
 */
cmd_buffer_data_t* new_cmd_buffer_data(VkCommandBuffer cmd_buffer,
        VkCommandBufferLevel level, device_data_t* device_data)
{
        cmd_buffer_data_t* cmd_buffer_data = malloc(sizeof(cmd_buffer_data_t));
        memset(cmd_buffer_data, 0, sizeof(*cmd_buffer_data));
        cmd_buffer_data->device_data = device_data;
        cmd_buffer_data->cmd_buffer  = cmd_buffer;
        cmd_buffer_data->level       = level;
        KROSSHAIR_LOG("[*] mapping data->cmd_buffer obj: %lu %p\n",
                HKEY(cmd_buffer_data->cmd_buffer), (void*)cmd_buffer_data);
        map_object(HKEY(cmd_buffer_data->cmd_buffer), cmd_buffer_data,
                "cmd_buffer_data->cmd_buffer");
        return cmd_buffer_data;
}

/*
 * Allocate and register the per-queue state.
 *
 * queue          The VkQueue this state belongs to.
 * family_props   Properties of the queue family the queue was created from.
 * family_index   Index of that queue family.
 * device_data    Per-device state the queue belongs to.
 *
 * A queue with the graphics bit also becomes the device's graphic_queue,
 * the queue the layer renders its overlays on.
 *
 * Returns a zeroed queue_data_t registered in the object map under the
 * VkQueue handle.
 */
static queue_data_t* new_queue_data(VkQueue queue,
        const VkQueueFamilyProperties* family_props, uint32_t family_index,
        device_data_t* device_data)
{
        queue_data_t* queue_data = malloc(sizeof(*queue_data));
        memset(queue_data, 0, sizeof(*queue_data));
        kh_perflog_host_alloc(sizeof(*queue_data), "queue data");
        queue_data->device       = device_data;
        queue_data->queue        = queue;
        queue_data->flags        = family_props->queueFlags;
        queue_data->family_index = family_index;
        KROSSHAIR_LOG("[*] mapping data->queue obj: %lu %p\n", HKEY(queue_data->queue),
                (void*)queue_data);
        map_object(HKEY(queue_data->queue), queue_data, "queue_data->queue");

        if (queue_data->flags & VK_QUEUE_GRAPHICS_BIT) {
                device_data->graphic_queue = queue_data;
        }

        return queue_data;
}

/*
 * Ask the driver for its queue family list.
 *
 * Vulkan requires two calls: one with a NULL array to learn how many
 * families exist, and a second to fill that many entries.
 *
 * device     Per-device state (provides the physical device + vtable).
 * out_count  Receives the number of queue families.
 *
 * Returns a malloc'd array of *out_count properties; caller frees it.
 */
static VkQueueFamilyProperties* query_queue_families(
    device_data_t* device, uint32_t* out_count)
{
        *out_count = 0;
        device->instance->vtable.GetPhysicalDeviceQueueFamilyProperties(
                device->physical_device, out_count, NULL);
        VkQueueFamilyProperties* family_props =
                malloc(sizeof(*family_props) * *out_count);
        device->instance->vtable.GetPhysicalDeviceQueueFamilyProperties(
                device->physical_device, out_count, family_props);
        return family_props;
}

/*
 * Create the device's VkQueues and register per-queue state for each.
 *
 * For every queue requested in create_info this:
 *  1. creates the VkQueue via GetDeviceQueue,
 *  2. hands the queue to the loader's internal bookkeeping,
 *  3. records it in device->queues[] with its family properties.
 *
 * device       Per-device state to fill in.
 * create_info  Device creation info listing the queues to create.
 */
void device_map_queues(device_data_t* device,
        const VkDeviceCreateInfo* create_info)
{
        /* total number of queues the application is asking for */
        uint32_t total_queues = 0;
        for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++) {
                total_queues += create_info->pQueueCreateInfos[i].queueCount;
        }
        device->queue_count = total_queues;

        uint32_t family_count;
        VkQueueFamilyProperties* family_props =
                query_queue_families(device, &family_count);

        uint32_t queue_index = 0;
        for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++) {
                const VkDeviceQueueCreateInfo* queue_info =
                        &create_info->pQueueCreateInfos[i];
                for (uint32_t j = 0; j < queue_info->queueCount; j++) {
                        VkQueue queue;
                        device->vtable.GetDeviceQueue(
                                device->device, queue_info->queueFamilyIndex, j, &queue);

                        VK_CHECK(
                                device->set_device_loader_data(device->device, queue));

                        device->queues[queue_index++] = new_queue_data(
                                queue, &family_props[queue_info->queueFamilyIndex],
                                queue_info->queueFamilyIndex, device);
                }
        }

        free(family_props);
}
