/*
 * GPU resource management.
 *
 * Creates and tears down all Vulkan resources: the device-scoped stable
 * resources (render pass, pipelines, descriptor pools, shader modules,
 * command pool) which survive swapchain recreation, and the
 * swapchain-scoped ones (images, views, framebuffers, vertex/index
 * buffers, upload buffers). Also handles image/mask hot-reload
 * shutdown paths used by the crosshair module.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/krosshair.h"
#include "../include/shaders.h"
#include "../shaders/dynamic_spv.h"

uint32_t vk_memory_type(device_data_t* device_data,
                               VkMemoryPropertyFlags properties,
                               uint32_t type_bits)
{
        VkPhysicalDeviceMemoryProperties _properties;
        device_data->instance->vtable.GetPhysicalDeviceMemoryProperties(
            device_data->physical_device, &_properties);
        for (uint32_t i = 0; i < _properties.memoryTypeCount; i++) {
                if ((_properties.memoryTypes[i].propertyFlags & properties) ==
                        properties &&
                    type_bits & (1 << i))
                        return i;
        }
        return K_NO_MEMORYTYPE;
}


void create_or_resize_buffer(device_data_t* device_data,
                                    VkBuffer* buffer,
                                    VkDeviceMemory* buffer_mem,
                                    VkDeviceSize* buffer_size, size_t new_size,
                                    VkBufferUsageFlagBits usage)
{
        if (*buffer != VK_NULL_HANDLE) {
                device_data->vtable.DestroyBuffer(device_data->device, *buffer,
                                                  NULL);
        }
        if (*buffer_mem) {
                device_data->vtable.FreeMemory(device_data->device, *buffer_mem,
                                               NULL);
        }

        if (device_data->properties.limits.nonCoherentAtomSize > 0) {
                VkDeviceSize atom_size =
                    device_data->properties.limits.nonCoherentAtomSize - 1;
                new_size = (new_size + atom_size) & ~atom_size;
        }

        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size               = new_size;
        buffer_info.usage              = usage;
        buffer_info.sharingMode        = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(device_data->vtable.CreateBuffer(device_data->device,
                                                  &buffer_info, NULL, buffer));

        VkMemoryRequirements req;
        device_data->vtable.GetBufferMemoryRequirements(device_data->device,
                                                        *buffer, &req);

        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex =
            vk_memory_type(device_data, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                           req.memoryTypeBits);
        VK_CHECK(device_data->vtable.AllocateMemory(
            device_data->device, &alloc_info, NULL, buffer_mem));

        VK_CHECK(device_data->vtable.BindBufferMemory(device_data->device,
                                                      *buffer, *buffer_mem, 0));
        *buffer_size = new_size;
}


void shutdown_krosshair_image(swapchain_data_t* data)
{
        device_data_t* device_data = data->device_data;

        /* drain all ring slots' in-flight submits before freeing the
         * crosshair set and images: every slot's command buffer may still
         * be executing and referencing them (a per-frame slot-fence wait
         * only covers the current image_index slot). No-op when called
         * from destroy_swapchain_data, which already waited above. */
        VK_CHECK(device_data->vtable.DeviceWaitIdle(device_data->device));

        if (data->crosshair_image_view) {
                device_data->vtable.DestroyImageView(device_data->device,
                                                     data->crosshair_image_view, NULL);
                data->crosshair_image_view = VK_NULL_HANDLE;
        }
        if (data->crosshair_image) {
                device_data->vtable.DestroyImage(device_data->device,
                                                 data->crosshair_image, NULL);
                data->crosshair_image = VK_NULL_HANDLE;
        }
        if (data->crosshair_mem) {
                device_data->vtable.FreeMemory(device_data->device, data->crosshair_mem,
                                               NULL);
                data->crosshair_mem = VK_NULL_HANDLE;
        }

        if (data->crosshair_upload_buffer) {
                device_data->vtable.DestroyBuffer(device_data->device,
                                                  data->crosshair_upload_buffer, NULL);
                data->crosshair_upload_buffer = VK_NULL_HANDLE;
        }
        if (data->crosshair_upload_buffer_mem) {
                device_data->vtable.FreeMemory(device_data->device,
                                               data->crosshair_upload_buffer_mem, NULL);
                data->crosshair_upload_buffer_mem = VK_NULL_HANDLE;
        }

        /* free THIS swapchain's crosshair set (targeted, not a bulk pool reset) —
         * gated on the set handle, so a fresh first upload (no set yet) is a
         * no-op. FreeDescriptorSets frees only this swapchain's slot; other
         * swapchains' sets (up to maxSets=4) are untouched. The mask is NOT
         * touched here: it is freed only on mask-reload
         * (ensure_swapchain_dynamic_mask) or full swapchain teardown, so a
         * crosshair hot-reload no longer wipes the mask. */
        if (data->descriptor_set != VK_NULL_HANDLE) {
                VkDescriptorSet sets[] = {data->descriptor_set};
                device_data->vtable.FreeDescriptorSets(
                    device_data->device, device_data->descriptor_pool,
                    1, sets);
        }
        data->descriptor_set = VK_NULL_HANDLE;

        /* clean up animation state */
        if (data->anim_delays) {
                free(data->anim_delays);
                data->anim_delays = NULL;
        }
        data->anim_frame_count    = 0;
        data->anim_frame_height   = 0;
        data->anim_current_frame  = 0;
        data->crosshair_tex_width = 0;
}

void shutdown_dynamic_mask(swapchain_data_t* data)
{
        device_data_t* device_data = data->device_data;

        /* same in-flight drain as shutdown_krosshair_image: ring slot
         * submits may still be sampling the mask image, and the mask set
         * (which binds it) is freed by the reload caller right after we
         * return */
        VK_CHECK(device_data->vtable.DeviceWaitIdle(device_data->device));

        if (data->dynamic_mask.image_view) {
                device_data->vtable.DestroyImageView(device_data->device,
                                                     data->dynamic_mask.image_view, NULL);
                data->dynamic_mask.image_view = VK_NULL_HANDLE;
        }
        if (data->dynamic_mask.image) {
                device_data->vtable.DestroyImage(device_data->device,
                                                 data->dynamic_mask.image, NULL);
                data->dynamic_mask.image = VK_NULL_HANDLE;
        }
        if (data->dynamic_mask.mem) {
                device_data->vtable.FreeMemory(device_data->device,
                                               data->dynamic_mask.mem, NULL);
                data->dynamic_mask.mem = VK_NULL_HANDLE;
        }
        if (data->dynamic_mask.upload_buffer) {
                device_data->vtable.DestroyBuffer(device_data->device,
                                                  data->dynamic_mask.upload_buffer, NULL);
                data->dynamic_mask.upload_buffer = VK_NULL_HANDLE;
        }
        if (data->dynamic_mask.upload_buffer_mem) {
                device_data->vtable.FreeMemory(device_data->device,
                                               data->dynamic_mask.upload_buffer_mem, NULL);
                data->dynamic_mask.upload_buffer_mem = VK_NULL_HANDLE;
        }

        data->dynamic_mask.tex_width = 0;
}


void destroy_swapchain_data(swapchain_data_t* data)
{
        if (!data) return;

        device_data_t* device_data = data->device_data;

        /* wait for all in-flight submits (ring slots) to finish before
         * destroying resources still referenced by a pending present.
         * Single choke point: both overlay_CreateSwapchainKHR's oldSwapchain
         * branch and overlay_DestroySwapchainKHR reach here before GPU
         * teardown. */
        VK_CHECK(device_data->vtable.DeviceWaitIdle(device_data->device));

        for (uint32_t i = 0; i < data->n_images; i++) {
                if (data->draws[i]) {
                        destroy_draw(data, data->draws[i]);
                        data->draws[i] = NULL;
                }
        }

        /* descriptor sets are allocated from device-scoped pools that outlive
         * this swapchain. Free this swapchain's sets (targeted) so other
         * swapchains' sets are not invalidated. The pools are never destroyed
         * (device-scoped); the crosshair/mask views the sets reference are
         * destroyed below. */
        if (data->descriptor_set != VK_NULL_HANDLE) {
                VkDescriptorSet sets[] = {data->descriptor_set};
                device_data->vtable.FreeDescriptorSets(
                    device_data->device, device_data->descriptor_pool, 1, sets);
        }
        data->descriptor_set = VK_NULL_HANDLE;
        if (data->shader_mask_desc_set != VK_NULL_HANDLE) {
                VkDescriptorSet msets[] = {data->shader_mask_desc_set};
                device_data->vtable.FreeDescriptorSets(
                    device_data->device, device_data->shader_desc_pool, 1, msets);
        }
        data->shader_mask_desc_set = VK_NULL_HANDLE;

        shutdown_krosshair_image(data);
        shutdown_dynamic_mask(data);

        /* shader-based dynamic resources */
        if (data->game_fb_image_view) {
                device_data->vtable.DestroyImageView(device_data->device,
                                                     data->game_fb_image_view, NULL);
                data->game_fb_image_view = VK_NULL_HANDLE;
        }
        if (data->game_fb_image) {
                device_data->vtable.DestroyImage(device_data->device,
                                                 data->game_fb_image, NULL);
                data->game_fb_image = VK_NULL_HANDLE;
        }
        if (data->game_fb_mem) {
                device_data->vtable.FreeMemory(device_data->device,
                                               data->game_fb_mem, NULL);
                data->game_fb_mem = VK_NULL_HANDLE;
        }

        for (uint32_t i = 0; i < data->n_images; i++) {
                if (data->framebuffers[i] != VK_NULL_HANDLE) {
                        device_data->vtable.DestroyFramebuffer(device_data->device,
                                                                data->framebuffers[i], NULL);
                        data->framebuffers[i] = VK_NULL_HANDLE;
                }
                 if (data->image_views[i] != VK_NULL_HANDLE) {
                        device_data->vtable.DestroyImageView(device_data->device,
                                                             data->image_views[i], NULL);
                        data->image_views[i] = VK_NULL_HANDLE;
                 }
        }
        /* n_images = 0: the framebuffer/view loop above is the last consumer;
         * null it so a hypothetical second call to destroy_swapchain_data on
         * this struct can't double-free the same views/framebuffers. */
        data->n_images = 0;

        if (data->crosshair_path) {
                free(data->crosshair_path);
                data->crosshair_path = NULL;
        }
        if (data->dynamic_mask.path) {
                free(data->dynamic_mask.path);
                data->dynamic_mask.path = NULL;
        }
        if (data->dynamic_cfg_path) {
                free(data->dynamic_cfg_path);
                data->dynamic_cfg_path = NULL;
        }
}

static void update_image_descriptor(swapchain_data_t* data,
                                    VkImageView image_view, VkDescriptorSet set)
{
        device_data_t* device_data       = data->device_data;

        VkDescriptorImageInfo desc_image = {};
        desc_image.sampler               = device_data->crosshair_sampler;
        desc_image.imageView             = image_view;
        desc_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write_desc = {};
        write_desc.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_desc.dstSet          = set;
        write_desc.descriptorCount = 1;
        write_desc.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write_desc.pImageInfo      = &desc_image;
        device_data->vtable.UpdateDescriptorSets(device_data->device, 1,
                                                 &write_desc, 0, NULL);
}

void create_image(swapchain_data_t* data, VkDescriptorSet descriptor_set,
                         uint32_t width, uint32_t height, VkFormat format,
                         VkImage* image, VkDeviceMemory* image_mem,
                         VkImageView* image_view)
{
        device_data_t* device_data   = data->device_data;

        VkImageCreateInfo image_info = {};
        image_info.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType         = VK_IMAGE_TYPE_2D;
        image_info.format            = format;
        image_info.extent.width      = width;
        image_info.extent.height     = height;
        image_info.extent.depth      = 1;
        image_info.mipLevels         = 1;
        image_info.arrayLayers       = 1;
        image_info.samples           = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling            = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(device_data->vtable.CreateImage(device_data->device,
                                                 &image_info, NULL, image));

        VkMemoryRequirements kh_image_req;
        device_data->vtable.GetImageMemoryRequirements(device_data->device,
                                                       *image, &kh_image_req);

        VkMemoryAllocateInfo image_alloc_info = {};
        image_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        image_alloc_info.allocationSize = kh_image_req.size;
        image_alloc_info.memoryTypeIndex =
            vk_memory_type(device_data, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           kh_image_req.memoryTypeBits);
        VK_CHECK(device_data->vtable.AllocateMemory(
            device_data->device, &image_alloc_info, NULL, image_mem));
        VK_CHECK(device_data->vtable.BindImageMemory(device_data->device,
                                                     *image, *image_mem, 0));

        VkImageViewCreateInfo view_info = {};
        view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image    = *image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format   = format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        VK_CHECK(device_data->vtable.CreateImageView(
            device_data->device, &view_info, NULL, image_view));

        if (descriptor_set != VK_NULL_HANDLE) {
                update_image_descriptor(data, *image_view, descriptor_set);
        }
}

VkDescriptorSet create_image_with_desc(swapchain_data_t* data,
                                              uint32_t width, uint32_t height,
                                              VkFormat format, VkImage* image,
                                              VkDeviceMemory* image_mem,
                                              VkImageView* image_view)
{
        device_data_t* device_data             = data->device_data;

        VkDescriptorSet descriptor_set         = {};

        /* main pool is device-scoped, sized maxSets=4 — one set per concurrent
         * swapchain. A 5th concurrent swapchain would exhaust the pool (the
         * allocation below would fail with VK_ERROR_OUT_OF_POOL_MEMORY). */
        VkDescriptorSetAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool     = data->device_data->descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts        = &data->device_data->descriptor_layout;
        VK_CHECK(device_data->vtable.AllocateDescriptorSets(
            device_data->device, &alloc_info, &descriptor_set));

        create_image(data, descriptor_set, width, height, format, image,
                     image_mem, image_view);
        return descriptor_set;
}

static void create_or_resize_upload_buffer(device_data_t* device_data,
                                          VkBuffer* upload_buffer,
                                          VkDeviceMemory* upload_buffer_mem,
                                          VkDeviceSize new_size)
{
        if (*upload_buffer != VK_NULL_HANDLE) {
                device_data->vtable.DestroyBuffer(device_data->device, *upload_buffer,
                                                  NULL);
                *upload_buffer = VK_NULL_HANDLE;
        }
        if (*upload_buffer_mem) {
                device_data->vtable.FreeMemory(device_data->device, *upload_buffer_mem,
                                               NULL);
                *upload_buffer_mem = VK_NULL_HANDLE;
        }

        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size               = new_size;
        buffer_info.usage              = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode        = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(device_data->vtable.CreateBuffer(
            device_data->device, &buffer_info, NULL, upload_buffer));

        VkMemoryRequirements upload_buffer_req;
        device_data->vtable.GetBufferMemoryRequirements(
            device_data->device, *upload_buffer, &upload_buffer_req);

        VkMemoryAllocateInfo upload_alloc_info = {};
        upload_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        upload_alloc_info.allocationSize = upload_buffer_req.size;
        upload_alloc_info.memoryTypeIndex =
            vk_memory_type(device_data, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                           upload_buffer_req.memoryTypeBits);
        VK_CHECK(device_data->vtable.AllocateMemory(
            device_data->device, &upload_alloc_info, NULL, upload_buffer_mem));
        VK_CHECK(device_data->vtable.BindBufferMemory(
            device_data->device, *upload_buffer, *upload_buffer_mem, 0));
}

void upload_image_data(device_data_t* device_data,
                              VkCommandBuffer cmd_buffer, void* pixels,
                              VkDeviceSize upload_size, uint32_t width,
                              uint32_t height, VkBuffer* upload_buffer,
                              VkDeviceMemory* upload_buffer_mem, VkImage image)
{
        /* always create the upload buffer - each swapchain has its own */
        create_or_resize_upload_buffer(device_data, upload_buffer,
                                       upload_buffer_mem, upload_size);

        char* map = NULL;
        VK_CHECK(device_data->vtable.MapMemory(device_data->device,
                                               *upload_buffer_mem, 0,
                                               upload_size, 0, (void**)(&map)));
        memcpy(map, pixels, upload_size);
        VkMappedMemoryRange range = {};
        range.sType               = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory              = *upload_buffer_mem;
        range.size                = upload_size;
        VK_CHECK(device_data->vtable.FlushMappedMemoryRanges(
            device_data->device, 1, &range));
        device_data->vtable.UnmapMemory(device_data->device,
                                        *upload_buffer_mem);

        VkImageMemoryBarrier copy_barrier = {};
        copy_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copy_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copy_barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        copy_barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy_barrier.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier.image                       = image;
        copy_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_barrier.subresourceRange.levelCount = 1;
        copy_barrier.subresourceRange.layerCount = 1;
        device_data->vtable.CmdPipelineBarrier(
            cmd_buffer, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
            &copy_barrier);

        VkBufferImageCopy region           = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width           = width;
        region.imageExtent.height          = height;
        region.imageExtent.depth           = 1;
        device_data->vtable.CmdCopyBufferToImage(
            cmd_buffer, *upload_buffer, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier use_barrier = {};
        use_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        use_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        use_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        use_barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        use_barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        use_barrier.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        use_barrier.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        use_barrier.image                       = image;
        use_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        use_barrier.subresourceRange.levelCount = 1;
        use_barrier.subresourceRange.layerCount = 1;
        device_data->vtable.CmdPipelineBarrier(
            cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
            &use_barrier);
}

// malloc's returned string, free later

void create_device_stable_resources(device_data_t* device_data)
{
        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter     = VK_FILTER_LINEAR;
        sampler_info.minFilter     = VK_FILTER_LINEAR;
        sampler_info.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.minLod        = -1000;
        sampler_info.maxLod        = 1000;
        sampler_info.maxAnisotropy = 1;
        VK_CHECK(device_data->vtable.CreateSampler(device_data->device,
                                                   &sampler_info, NULL,
                                                   &device_data->crosshair_sampler));

        VkDescriptorPoolSize sampler_pool_size = {};
        sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        /* one crosshair sampler descriptor per concurrent swapchain set */
        sampler_pool_size.descriptorCount = KROSSHAIR_MAX_SWAPCHAINS;

        VkDescriptorPoolCreateInfo desc_pool_info = {};
        desc_pool_info.sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        desc_pool_info.maxSets = KROSSHAIR_MAX_SWAPCHAINS; /* one set per
                                                            concurrent swapchain */
        desc_pool_info.poolSizeCount = 1;
        desc_pool_info.pPoolSizes    = &sampler_pool_size;
        VK_CHECK(device_data->vtable.CreateDescriptorPool(
            device_data->device, &desc_pool_info, NULL,
            &device_data->descriptor_pool));

        VkSampler sampler                    = device_data->crosshair_sampler;
        VkDescriptorSetLayoutBinding binding = {};
        binding.descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount    = 1;
        binding.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
        binding.pImmutableSamplers = &sampler;

        VkDescriptorSetLayoutCreateInfo set_layout_info = {};
        set_layout_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_layout_info.bindingCount = 1;
        set_layout_info.pBindings    = &binding;
        VK_CHECK(device_data->vtable.CreateDescriptorSetLayout(
            device_data->device, &set_layout_info, NULL,
            &device_data->descriptor_layout));

        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount         = 1;
        layout_info.pSetLayouts            = &device_data->descriptor_layout;
        layout_info.pushConstantRangeCount = 0;
        VK_CHECK(device_data->vtable.CreatePipelineLayout(
            device_data->device, &layout_info, NULL, &device_data->pipeline_layout));

        /* ═══════════════════════════════════════════════════════════
         * Shader-based dynamic pipeline (Complement, LumaInvert, etc.)
         * Uses a custom fragment shader that reads the game framebuffer.
         * ═══════════════════════════════════════════════════════════ */
        {
                /* descriptor set layout: binding 0 = mask, binding 1 = game FB */
                VkDescriptorSetLayoutBinding shader_bindings[2] = {};
                shader_bindings[0].binding         = 0;
                shader_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                shader_bindings[0].descriptorCount = 1;
                shader_bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
                shader_bindings[0].pImmutableSamplers = &sampler;
                shader_bindings[1].binding         = 1;
                shader_bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                shader_bindings[1].descriptorCount = 1;
                shader_bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
                shader_bindings[1].pImmutableSamplers = &sampler;

                VkDescriptorSetLayoutCreateInfo sdl_info = {};
                sdl_info.sType =
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                sdl_info.bindingCount = 2;
                sdl_info.pBindings    = shader_bindings;
                VK_CHECK(device_data->vtable.CreateDescriptorSetLayout(
                    device_data->device, &sdl_info, NULL,
                    &device_data->shader_desc_layout));

                /* push constant range: full dynamic_push_constants (80 bytes) */
                VkPushConstantRange pc_range = {};
                pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
                pc_range.offset     = 0;
                pc_range.size       = sizeof(struct dynamic_push_constants);

                VkPipelineLayoutCreateInfo spl_info = {};
                spl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                spl_info.setLayoutCount         = 1;
                spl_info.pSetLayouts            = &device_data->shader_desc_layout;
                spl_info.pushConstantRangeCount = 1;
                spl_info.pPushConstantRanges    = &pc_range;
                VK_CHECK(device_data->vtable.CreatePipelineLayout(
                    device_data->device, &spl_info, NULL,
                    &device_data->shader_pipeline_layout));

                /* descriptor pool for shader dynamic desc set */
                VkDescriptorPoolSize sp_size = {};
                sp_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                sp_size.descriptorCount =
                    2 * KROSSHAIR_MAX_SWAPCHAINS; /* 2 bindings: mask + game_fb */

                VkDescriptorPoolCreateInfo sp_info = {};
                sp_info.sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                 sp_info.maxSets = KROSSHAIR_MAX_SWAPCHAINS; /* one set per
                                                              concurrent swapchain */
                sp_info.poolSizeCount = 1;
                sp_info.pPoolSizes    = &sp_size;
                VK_CHECK(device_data->vtable.CreateDescriptorPool(
                    device_data->device, &sp_info, NULL,
                    &device_data->shader_desc_pool));
        }

        VkCommandPoolCreateInfo cmd_buffer_pool_info = {};
        cmd_buffer_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cmd_buffer_pool_info.flags =
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cmd_buffer_pool_info.queueFamilyIndex =
            device_data->graphic_queue->family_index;
        VK_CHECK(device_data->vtable.CreateCommandPool(
            device_data->device, &cmd_buffer_pool_info, NULL,
            &device_data->cmd_pool));
}

/*
 * Format-scoped resources: render pass + crosshair pipeline + shader
 * pipeline.  The pipelines are bound to the render pass and both are created
 * from the app's image format, so all three are destroyed and recreated
 * together when the format changes.
 */
static void create_format_resources(device_data_t* device_data)
{
        VkFormat format = device_data->render_pass_format;

        VkAttachmentDescription attachment_desc = {};
        attachment_desc.format                  = format;
        attachment_desc.samples                 = VK_SAMPLE_COUNT_1_BIT;
        attachment_desc.loadOp                  = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment_desc.storeOp                 = VK_ATTACHMENT_STORE_OP_STORE;
        attachment_desc.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment_desc.initialLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment_desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_attachment = {};
        color_attachment.attachment            = 0;
        color_attachment.layout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &color_attachment;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass          = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass          = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo render_pass_info = {};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments    = &attachment_desc;
        render_pass_info.subpassCount    = 1;
        render_pass_info.pSubpasses      = &subpass;
        render_pass_info.dependencyCount = 1;
        render_pass_info.pDependencies   = &dependency;
        VK_CHECK(device_data->vtable.CreateRenderPass(
            device_data->device, &render_pass_info, NULL,
            &device_data->render_pass));

        VkShaderModule vert_module;
        VkShaderModule frag_module;

        VkShaderModuleCreateInfo vert_info = {};
        vert_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vert_info.codeSize = sizeof(vert_spv);
        vert_info.pCode    = (const uint32_t*)vert_spv;
        VK_CHECK(device_data->vtable.CreateShaderModule(
            device_data->device, &vert_info, NULL, &vert_module));

        VkShaderModuleCreateInfo frag_info = {};
        frag_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        frag_info.codeSize = sizeof(frag_spv);
        frag_info.pCode    = (const uint32_t*)frag_spv;
        VK_CHECK(device_data->vtable.CreateShaderModule(
            device_data->device, &frag_info, NULL, &frag_module));

        VkPipelineShaderStageCreateInfo stage[2] = {};
        stage[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stage[0].module = vert_module;
        stage[0].pName  = "main";
        stage[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stage[1].module = frag_module;
        stage[1].pName  = "main";

        VkVertexInputBindingDescription binding_desc = {};
        binding_desc.binding                         = 0;
        binding_desc.stride                          = sizeof(vertex_t);
        binding_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attribute_desc[2] = {};
        attribute_desc[0].location                          = 0;
        attribute_desc[0].binding                           = 0;
        attribute_desc[0].format   = VK_FORMAT_R32G32_SFLOAT;
        attribute_desc[0].offset   = offsetof(vertex_t, pos);
        attribute_desc[1].location = 1;
        attribute_desc[1].binding  = 0;
        attribute_desc[1].format   = VK_FORMAT_R32G32_SFLOAT;
        attribute_desc[1].offset   = offsetof(vertex_t, tex_pos);

        VkPipelineVertexInputStateCreateInfo vertex_info = {};
        vertex_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_info.vertexBindingDescriptionCount             = 1;
        vertex_info.pVertexBindingDescriptions                = &binding_desc;
        vertex_info.vertexAttributeDescriptionCount           = 2;
        vertex_info.pVertexAttributeDescriptions              = attribute_desc;

        VkPipelineInputAssemblyStateCreateInfo input_asm_info = {};
        input_asm_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_asm_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_asm_info.primitiveRestartEnable           = VK_FALSE;

        VkPipelineViewportStateCreateInfo viewport_info = {};
        viewport_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_info.viewportCount                        = 1;
        viewport_info.scissorCount                         = 1;

        VkPipelineRasterizationStateCreateInfo raster_info = {};
        raster_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster_info.polygonMode                      = VK_POLYGON_MODE_FILL;
        raster_info.cullMode                         = VK_CULL_MODE_BACK_BIT;
        raster_info.frontFace                        = VK_FRONT_FACE_CLOCKWISE;
        raster_info.lineWidth                        = 1.0f;
        raster_info.rasterizerDiscardEnable          = VK_FALSE;
        raster_info.depthClampEnable                 = VK_FALSE;
        raster_info.depthBiasEnable                  = VK_FALSE;
        raster_info.depthBiasConstantFactor          = 0.0f;
        raster_info.depthBiasClamp                   = 0.0f;
        raster_info.depthBiasSlopeFactor             = 0.0f;

        VkPipelineMultisampleStateCreateInfo ms_info = {};
        ms_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blend_attachment = {};
        blend_attachment.blendEnable                       = VK_TRUE;
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp                    = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor             = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor             = VK_BLEND_FACTOR_ZERO;
        blend_attachment.alphaBlendOp                    = VK_BLEND_OP_ADD;

        VkPipelineDepthStencilStateCreateInfo depth_info = {};
        depth_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        VkPipelineColorBlendStateCreateInfo blend_info = {};
        blend_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend_info.attachmentCount       = 1;
        blend_info.pAttachments          = &blend_attachment;

        VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic_state = {};
        dynamic_state.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount            = 2;
        dynamic_state.pDynamicStates               = dynamic_states;

        VkGraphicsPipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.flags = 0;
        pipeline_info.stageCount          = 2;
        pipeline_info.pStages             = stage;
        pipeline_info.pVertexInputState   = &vertex_info;
        pipeline_info.pInputAssemblyState = &input_asm_info;
        pipeline_info.pViewportState      = &viewport_info;
        pipeline_info.pRasterizationState = &raster_info;
        pipeline_info.pMultisampleState   = &ms_info;
        pipeline_info.pDepthStencilState  = &depth_info;
        pipeline_info.pColorBlendState    = &blend_info;
        pipeline_info.pDynamicState       = &dynamic_state;
        pipeline_info.layout              = device_data->pipeline_layout;
        pipeline_info.renderPass          = device_data->render_pass;
        VK_CHECK(device_data->vtable.CreateGraphicsPipelines(
            device_data->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
            &device_data->pipeline));

        device_data->vtable.DestroyShaderModule(device_data->device,
                                                vert_module, NULL);
        device_data->vtable.DestroyShaderModule(device_data->device,
                                                frag_module, NULL);

        /* ═══════════════════════════════════════════════════════════
         * Shader-based dynamic pipeline (Complement, LumaInvert, etc.)
         * Uses a custom fragment shader that reads the game framebuffer.
         * ═══════════════════════════════════════════════════════════ */
        {
                VkShaderModule dyn_vert_mod, dyn_frag_mod;

                VkShaderModuleCreateInfo dvi = {};
                dvi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                dvi.codeSize = sizeof(dynamic_vert_spv);
                dvi.pCode    = (const uint32_t*)dynamic_vert_spv;
                VK_CHECK(device_data->vtable.CreateShaderModule(
                    device_data->device, &dvi, NULL, &dyn_vert_mod));

                VkShaderModuleCreateInfo dfi = {};
                dfi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                dfi.codeSize = sizeof(dynamic_frag_spv);
                dfi.pCode    = (const uint32_t*)dynamic_frag_spv;
                VK_CHECK(device_data->vtable.CreateShaderModule(
                    device_data->device, &dfi, NULL, &dyn_frag_mod));

                /* desc layout + pool are device-scoped (created in
                 * create_device_stable_resources) */

                /* shader stages */
                VkPipelineShaderStageCreateInfo sstage[2] = {};
                sstage[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                sstage[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
                sstage[0].module = dyn_vert_mod;
                sstage[0].pName  = "main";
                sstage[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                sstage[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
                sstage[1].module = dyn_frag_mod;
                sstage[1].pName  = "main";

                /* the shader outputs final composited color — standard alpha blend */
                VkPipelineColorBlendAttachmentState shader_ca = {};
                shader_ca.blendEnable         = VK_TRUE;
                shader_ca.colorWriteMask =
                    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                shader_ca.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                shader_ca.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                shader_ca.colorBlendOp        = VK_BLEND_OP_ADD;
                shader_ca.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                shader_ca.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                shader_ca.alphaBlendOp        = VK_BLEND_OP_ADD;

                VkPipelineColorBlendStateCreateInfo shader_blend = {};
                shader_blend.sType =
                    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                shader_blend.attachmentCount = 1;
                shader_blend.pAttachments    = &shader_ca;

                /* reuse vertex_info, input_asm_info, viewport_info, raster_info,
                 * ms_info, depth_info, dynamic_state from earlier */
                VkGraphicsPipelineCreateInfo spi = {};
                spi.sType             = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                spi.stageCount        = 2;
                spi.pStages           = sstage;
                spi.pVertexInputState   = &vertex_info;
                spi.pInputAssemblyState = &input_asm_info;
                spi.pViewportState      = &viewport_info;
                spi.pRasterizationState = &raster_info;
                spi.pMultisampleState   = &ms_info;
                spi.pDepthStencilState  = &depth_info;
                spi.pColorBlendState    = &shader_blend;
                spi.pDynamicState       = &dynamic_state;
                spi.layout              = device_data->shader_pipeline_layout;
                spi.renderPass          = device_data->render_pass;
                VK_CHECK(device_data->vtable.CreateGraphicsPipelines(
                    device_data->device, VK_NULL_HANDLE, 1, &spi, NULL,
                    &device_data->shader_pipeline));

                device_data->vtable.DestroyShaderModule(device_data->device,
                                                        dyn_vert_mod, NULL);
                device_data->vtable.DestroyShaderModule(device_data->device,
                                                        dyn_frag_mod, NULL);
        }
}

/*
 * Format guard: create (or recreate on format change) the render pass +
 * pipelines.  On first call render_pass_format is VK_FORMAT_UNDEFINED, so the
 * guard falls through to creation.  The pipelines reference the render pass,
 * so they are destroyed before it.
 */
static void ensure_format_resources(device_data_t* device_data, VkFormat format)
{
        if (device_data->render_pass_format == format &&
            device_data->render_pass != VK_NULL_HANDLE)
                return;

        if (device_data->pipeline != VK_NULL_HANDLE) {
                device_data->vtable.DestroyPipeline(
                    device_data->device, device_data->pipeline, NULL);
                device_data->pipeline = VK_NULL_HANDLE;
        }
        if (device_data->shader_pipeline != VK_NULL_HANDLE) {
                device_data->vtable.DestroyPipeline(
                    device_data->device, device_data->shader_pipeline, NULL);
                device_data->shader_pipeline = VK_NULL_HANDLE;
        }
        if (device_data->render_pass != VK_NULL_HANDLE) {
                device_data->vtable.DestroyRenderPass(
                    device_data->device, device_data->render_pass, NULL);
                device_data->render_pass = VK_NULL_HANDLE;
        }

        device_data->render_pass_format = format;
        create_format_resources(device_data);
}

void setup_swapchain_data(swapchain_data_t* data,
                                 const VkSwapchainCreateInfoKHR* pCreateInfo)
{
        device_data_t* device_data = data->device_data;
        data->width                = pCreateInfo->imageExtent.width;
        data->height               = pCreateInfo->imageExtent.height;
        data->format               = pCreateInfo->imageFormat;

        /* The format-scoped render pass must exist before the framebuffers
         * that reference it are created */
        ensure_format_resources(device_data, pCreateInfo->imageFormat);

        uint32_t n_images = 0;
        VK_CHECK(device_data->vtable.GetSwapchainImagesKHR(
            device_data->device, data->swapchain, &n_images, NULL));

        VK_CHECK(device_data->vtable.GetSwapchainImagesKHR(
            device_data->device, data->swapchain, &n_images, data->images));
        data->n_images = n_images;

        VkImageViewCreateInfo view_info = {};
        view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format   = pCreateInfo->imageFormat;
        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount     = 1;
        for (size_t i = 0; i < n_images; i++) {
                view_info.image = data->images[i];
                VK_CHECK(device_data->vtable.CreateImageView(
                    device_data->device, &view_info, NULL,
                    &data->image_views[i]));
        }

        VkImageView attachment;
        VkFramebufferCreateInfo fb_info = {};
        fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass      = device_data->render_pass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments    = &attachment;
        fb_info.width           = data->width;
        fb_info.height          = data->height;
        fb_info.layers          = 1;
        for (size_t i = 0; i < n_images; i++) {
                attachment = data->image_views[i];
                VK_CHECK(device_data->vtable.CreateFramebuffer(
                    device_data->device, &fb_info, NULL,
                    &data->framebuffers[i]));
        }

        /* fence ring: one draw slot per swapchain image */
        for (uint32_t i = 0; i < n_images; i++)
                create_draw_slot(data, i);
}

