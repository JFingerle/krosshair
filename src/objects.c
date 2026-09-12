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

int vk_map_set(vk_object_map_t* map, uint64_t obj, void* data, const char* name)
{
        // check if entry already exists
        for (size_t i = 0; i < MAX_VK_OBJECTS; i++) {
                if (map->data[i].obj == obj) {
                        map->data[i].data = data;
                        map->data[i].name = name;
                        map->count++;
                        return i;
                }
        }

        // if it doesn't already exist, find the first free one
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
 * returns 0 on failure, 1 on success
 * */
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

void* find_object_data(uint64_t obj)
{
        pthread_mutex_lock(&global_lock);

        vk_object_t temp_obj;
        if (!vk_map_get(&vk_obj_map, obj, &temp_obj)) {
                pthread_mutex_unlock(&global_lock);
                return NULL;
        }

        pthread_mutex_unlock(&global_lock);
        return temp_obj.data;
}

void map_object(uint64_t obj, void* data, const char* name)
{
        pthread_mutex_lock(&global_lock);

        if (vk_map_set(&vk_obj_map, obj, data, name) == -1) {
                pthread_mutex_unlock(&global_lock);
                return;
        }
        // vk_map_print(&vk_obj_map);
        pthread_mutex_unlock(&global_lock);
}

void unmap_object(uint64_t obj)
{
        pthread_mutex_lock(&global_lock);
        vk_map_delete(&vk_obj_map, obj);
        pthread_mutex_unlock(&global_lock);
}

VkLayerInstanceCreateInfo* get_instance_chain_info(
    const VkInstanceCreateInfo* p_create_info, VkLayerFunction func)
{
        vk_foreach_struct(item, p_create_info->pNext)
        {
                if (item->sType ==
                        VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                    ((VkLayerInstanceCreateInfo*)item)->function == func)
                        return (VkLayerInstanceCreateInfo*)item;
        }
        return NULL;
}

instance_data_t* new_instance_data(VkInstance instance)
{
        instance_data_t* instance_data = malloc(sizeof(instance_data_t));
        memset(instance_data, 0, sizeof(*instance_data));
        instance_data->instance        = instance;
        KROSSHAIR_LOG("[*] mapping data->instance obj: %lu data: %p\n",
               HKEY(instance_data->instance), (void*)instance_data);
        map_object(HKEY(instance_data->instance), instance_data,
                   "instance_data->instance");
        return instance_data;
}

device_data_t* new_device_data(VkDevice device,
                                      instance_data_t* instance)
{
        device_data_t* device_data = malloc(sizeof(device_data_t));
        memset(device_data, 0, sizeof(*device_data));
        device_data->instance      = instance;
        device_data->device        = device;
        KROSSHAIR_LOG("[*] mapping data->device obj: %lu %p\n",
               HKEY(device_data->device), (void*)device_data);
        map_object(HKEY(device_data->device), device_data,
                   "device_data->device");
        return device_data;
}

cmd_buffer_data_t* new_cmd_buffer_data(VkCommandBuffer cmd_buffer,
                                              VkCommandBufferLevel level,
                                              device_data_t* device_data)
{
        cmd_buffer_data_t* cmdbuffer_data = malloc(sizeof(cmd_buffer_data_t));
        memset(cmdbuffer_data, 0, sizeof(*cmdbuffer_data));
        cmdbuffer_data->device_data       = device_data;
        cmdbuffer_data->cmd_buffer        = cmd_buffer;
        cmdbuffer_data->level             = level;
        KROSSHAIR_LOG("[*] mapping data->cmd_buffer obj: %lu %p\n",
               HKEY(cmdbuffer_data->cmd_buffer), (void*)cmdbuffer_data);
        map_object(HKEY(cmdbuffer_data->cmd_buffer), cmdbuffer_data,
                   "cmdbuffer_data->cmd_buffer");
        return cmdbuffer_data;
}


static queue_data_t* new_queue_data(VkQueue queue,
                                    const VkQueueFamilyProperties* family_props,
                                    uint32_t family_index,
                                    device_data_t* device_data)
{
        queue_data_t* queue_data = malloc(sizeof(*queue_data));
        memset(queue_data, 0, sizeof(*queue_data));
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

void device_map_queues(device_data_t* data,
                              const VkDeviceCreateInfo* pCreateInfo)
{
        uint32_t n_queues = 0;
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
                n_queues += pCreateInfo->pQueueCreateInfos[i].queueCount;
        }
        data->queue_count              = n_queues;

        instance_data_t* instance_data = data->instance;
        uint32_t n_family_props;
        instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(
            data->physical_device, &n_family_props, NULL);
        VkQueueFamilyProperties* family_props =
            malloc(sizeof(*family_props) * n_family_props);
        instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(
            data->physical_device, &n_family_props, family_props);

        uint32_t queue_index = 0;
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
                for (uint32_t j = 0;
                     j < pCreateInfo->pQueueCreateInfos[i].queueCount; j++) {
                        VkQueue queue;
                        data->vtable.GetDeviceQueue(
                            data->device,
                            pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex,
                            j, &queue);

                        VK_CHECK(
                            data->set_device_loader_data(data->device, queue));

                        data->queues[queue_index++] = new_queue_data(
                            queue,
                            &family_props[pCreateInfo->pQueueCreateInfos[i]
                                              .queueFamilyIndex],
                            pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex,
                            data);
                }
        }

        free(family_props);
}

