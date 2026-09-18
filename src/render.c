/*
 * Overlay rendering.
 *
 * Records and submits the crosshair draw for a presented swapchain
 * image: reuses the per-image draw ring slot (waiting on its fence so a
 * slot is never re-recorded mid-submit), renders the crosshair quad
 * into the application's framebuffer, and hands back the draw whose
 * overlay semaphore the layer folds into vkQueuePresentKHR.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/krosshair.h"

#ifdef UNIT_TEST
#define RENDER_API
#else
#define RENDER_API static
#endif

/*
 * Wait for this draw slot's previous GPU submission to finish so its
 * command buffer can be safely re-recorded.
 *
 * device_data    The device (vtable + handles).
 * draw           The per-swapchain-image draw slot.
 * image_index    Which image (for logging only).
 *
 * Returns 1 if the slot is ready to record; 0 if the previous submit did
 * not finish within 100 ms (the caller should skip this frame — a slot
 * must never be destroyed while its submit is in flight).
 */
RENDER_API int wait_draw_slot_ready(device_data_t* device_data,
                                    krosshair_draw_t* draw,
                                    unsigned image_index)
{
        if (!draw->fence_submitted)
                return 1;

        VkResult wait_result = device_data->vtable.WaitForFences(
            device_data->device, 1, &draw->fence, VK_TRUE, 100000000);
        if (wait_result != VK_SUCCESS) {
                KROSSHAIR_LOG("[KROSSHAIR] slot %u fence timeout, skipping frame\n",
                               image_index);
                return 0;
        }
        VK_CHECK(device_data->vtable.ResetFences(
            device_data->device, 1, &draw->fence));
        draw->fence_submitted = 0;
        return 1;
}

/*
 * Create (or recreate after a resolution change) the full-screen
 * "game framebuffer" copy texture. Shader-based effects sample this
 * texture to see the game frame underneath the crosshair.
 *
 * data  The swapchain state holding the texture handles (destroyed and
 *       rebuilt here if missing or sized for a different resolution).
 */
RENDER_API void ensure_game_fb_copy(swapchain_data_t* data)
{
        device_data_t* device_data = data->device_data;

        if (data->game_fb_image &&
            data->game_fb_width == data->width &&
            data->game_fb_height == data->height)
                return;

        if (data->game_fb_image_view) {
                device_data->vtable.DestroyImageView(
                    device_data->device, data->game_fb_image_view, NULL);
                data->game_fb_image_view = VK_NULL_HANDLE;
        }
        if (data->game_fb_image) {
                device_data->vtable.DestroyImage(
                    device_data->device, data->game_fb_image, NULL);
                data->game_fb_image = VK_NULL_HANDLE;
        }
        if (data->game_fb_mem) {
                kh_perflog_dev_free(data->game_fb_mem);
                device_data->vtable.FreeMemory(
                    device_data->device, data->game_fb_mem, NULL);
                data->game_fb_mem = VK_NULL_HANDLE;
        }

        VkImageCreateInfo image_info = {};
        image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.format        = data->format;
        image_info.extent.width  = data->width;
        image_info.extent.height = data->height;
        image_info.extent.depth  = 1;
        image_info.mipLevels     = 1;
        image_info.arrayLayers   = 1;
        image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(device_data->vtable.CreateImage(
            device_data->device, &image_info, NULL, &data->game_fb_image));

        VkMemoryRequirements mem_req;
        device_data->vtable.GetImageMemoryRequirements(
            device_data->device, data->game_fb_image, &mem_req);

        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_req.size;
        alloc_info.memoryTypeIndex =
            vk_memory_type(device_data,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           mem_req.memoryTypeBits);
        VK_CHECK(device_data->vtable.AllocateMemory(
            device_data->device, &alloc_info, NULL, &data->game_fb_mem));
        kh_perflog_dev_alloc(data->game_fb_mem, mem_req.size);
        VK_CHECK(device_data->vtable.BindImageMemory(
            device_data->device, data->game_fb_image,
            data->game_fb_mem, 0));

        VkImageViewCreateInfo view_info = {};
        view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image    = data->game_fb_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format   = data->format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        VK_CHECK(device_data->vtable.CreateImageView(
            device_data->device, &view_info, NULL,
            &data->game_fb_image_view));

        data->game_fb_width  = data->width;
        data->game_fb_height = data->height;

        KROSSHAIR_LOG("[KROSSHAIR] created game FB copy texture %ux%u\n",
                      data->width, data->height);
}

/*
 * Record barriers and a full-image copy from the swapchain image into
 * the game_fb copy texture, so shader-based effects can sample the game
 * frame. Also transfers ownership of the swapchain image from the
 * present queue family to the graphics queue family.
 *
 * data           Swapchain state (image handles, dimensions).
 * cmd_buffer     Command buffer to record into.
 * image_index    Which swapchain image to copy.
 * present_queue  Queue the image was last presented on (source family).
 */
RENDER_API void record_framebuffer_copy(swapchain_data_t* data,
                                        VkCommandBuffer cmd_buffer,
                                        unsigned image_index,
                                        queue_data_t* present_queue)
{
        device_data_t* device_data = data->device_data;

        /* swapchain image: PRESENT_SRC -> TRANSFER_SRC (so we can read it) */
        VkImageMemoryBarrier swapchain_barrier = {};
        swapchain_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        swapchain_barrier.image         = data->images[image_index];
        swapchain_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        swapchain_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        swapchain_barrier.oldLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        swapchain_barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        swapchain_barrier.srcQueueFamilyIndex = present_queue->family_index;
        swapchain_barrier.dstQueueFamilyIndex = device_data->graphic_queue->family_index;
        swapchain_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        swapchain_barrier.subresourceRange.baseMipLevel   = 0;
        swapchain_barrier.subresourceRange.levelCount     = 1;
        swapchain_barrier.subresourceRange.baseArrayLayer = 0;
        swapchain_barrier.subresourceRange.layerCount     = 1;
        device_data->vtable.CmdPipelineBarrier(
            cmd_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
            &swapchain_barrier);

        /* game_fb copy: UNDEFINED -> TRANSFER_DST (so we can write it) */
        VkImageMemoryBarrier fb_barrier = {};
        fb_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        fb_barrier.image         = data->game_fb_image;
        fb_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fb_barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        fb_barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        fb_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        fb_barrier.subresourceRange.baseMipLevel   = 0;
        fb_barrier.subresourceRange.levelCount     = 1;
        fb_barrier.subresourceRange.baseArrayLayer = 0;
        fb_barrier.subresourceRange.layerCount     = 1;
        device_data->vtable.CmdPipelineBarrier(
            cmd_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
            &fb_barrier);

        /* copy swapchain -> game_fb */
        VkImageCopy copy_region = {};
        copy_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.srcSubresource.layerCount = 1;
        copy_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.dstSubresource.layerCount = 1;
        copy_region.extent.width  = data->width;
        copy_region.extent.height = data->height;
        copy_region.extent.depth  = 1;
        device_data->vtable.CmdCopyImage(
            cmd_buffer,
            data->images[image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            data->game_fb_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copy_region);

        /* game_fb copy: TRANSFER_DST -> SHADER_READ_ONLY (so the shader can sample it) */
        fb_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fb_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        fb_barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        fb_barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        device_data->vtable.CmdPipelineBarrier(
            cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &fb_barrier);

        /* swapchain image: TRANSFER_SRC -> COLOR_ATTACHMENT (so the render pass works) */
        swapchain_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        swapchain_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        swapchain_barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        swapchain_barrier.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        device_data->vtable.CmdPipelineBarrier(
            cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, NULL, 0, NULL, 1, &swapchain_barrier);
}

/*
 * Simple layout transition for the no-copy path: move the swapchain
 * image from PRESENT_SRC_KHR to COLOR_ATTACHMENT_OPTIMAL so the render
 * pass can write the crosshair into it (and hand ownership to the
 * graphics queue family).
 *
 * data           Swapchain state.
 * cmd_buffer     Command buffer to record into.
 * image_index    Which swapchain image to transition.
 * present_queue  Queue the image was last presented on (source family).
 */
RENDER_API void transition_swapchain_for_render(swapchain_data_t* data,
                                                VkCommandBuffer cmd_buffer,
                                                unsigned image_index,
                                                queue_data_t* present_queue)
{
        device_data_t* device_data = data->device_data;

        VkImageMemoryBarrier swapchain_barrier = {};
        swapchain_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        swapchain_barrier.image         = data->images[image_index];
        swapchain_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        swapchain_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        swapchain_barrier.oldLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        swapchain_barrier.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        swapchain_barrier.srcQueueFamilyIndex = present_queue->family_index;
        swapchain_barrier.dstQueueFamilyIndex = device_data->graphic_queue->family_index;
        swapchain_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        swapchain_barrier.subresourceRange.baseMipLevel   = 0;
        swapchain_barrier.subresourceRange.levelCount     = 1;
        swapchain_barrier.subresourceRange.baseArrayLayer = 0;
        swapchain_barrier.subresourceRange.layerCount     = 1;
        device_data->vtable.CmdPipelineBarrier(
            cmd_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0, NULL, 1,
            &swapchain_barrier);
}

/*
 * Advance the animated crosshair (GIF/APNG) to the frame that should be
 * shown right now, and recompute the quad's UV coordinates to point at
 * that frame's strip in the atlas texture.
 *
 * The frame clock accumulates elapsed time (rather than resetting to
 * "now") so leftover time carries over and the animation stays in sync
 * when the present rate is lower than the animation rate.
 *
 * data  Swapchain state holding the animation frame index, per-frame
 *      delays and the quad vertex buffer (rewritten here).
 * draw  The draw slot being recorded (flagged dirty so the updated
 *      vertex buffer is re-uploaded).
 */
RENDER_API void advance_anim_frame(swapchain_data_t* data,
                                   krosshair_draw_t* draw)
{
        if (data->anim_frame_count <= 1 || !data->anim_delays)
                return;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        long elapsed_ms =
            (now.tv_sec - data->anim_last_frame_time.tv_sec) * 1000 +
            (now.tv_nsec - data->anim_last_frame_time.tv_nsec) / 1000000;

        int frame_advanced = 0;
        int delay_ms = data->anim_delays[data->anim_current_frame];

        /* consume all elapsed time, advancing multiple frames if
         * the present rate is lower than the animation rate */
        while (elapsed_ms >= delay_ms) {
                /* accumulate: add delay to last_frame_time instead
                 * of resetting to now, so leftover time carries
                 * over and the animation stays in sync */
                data->anim_last_frame_time.tv_nsec += (long)delay_ms * 1000000L;
                while (data->anim_last_frame_time.tv_nsec >= 1000000000L) {
                        data->anim_last_frame_time.tv_sec++;
                        data->anim_last_frame_time.tv_nsec -= 1000000000L;
                }

                data->anim_current_frame =
                    (data->anim_current_frame + 1) % data->anim_frame_count;
                delay_ms = data->anim_delays[data->anim_current_frame];
                frame_advanced = 1;

                /* recalculate elapsed from updated base */
                elapsed_ms =
                    (now.tv_sec - data->anim_last_frame_time.tv_sec) * 1000 +
                    (now.tv_nsec - data->anim_last_frame_time.tv_nsec) / 1000000;
        }

        if (frame_advanced) {
                float uv_step = 1.0f / (float)data->anim_frame_count;
                float uv_top  = uv_step * (float)data->anim_current_frame;
                float uv_bot  = uv_top + uv_step;
                setup_vertices_uv(
                    data->vertices,
                    (float)data->width, (float)data->height,
                    (float)data->crosshair_tex_width,
                    (float)data->anim_frame_height, 1.0f,
                    uv_top, uv_bot);

                /* force vertex buffer re-upload with new UVs */
                draw->vertex_buffer_initialized = 0;
        }
}

/*
 * Copy `bytes` bytes of host data into a device memory block through a
 * temporary CPU mapping (map, memcpy, flush, unmap).
 *
 * device_data   The device.
 * memory        Device memory to write into.
 * data          Host source.
 * bytes         Number of bytes to copy.
 */
RENDER_API void upload_device_memory(device_data_t* device_data,
                                     VkDeviceMemory memory,
                                     const void* data, size_t bytes)
{
        void* mapped = NULL;
        VK_CHECK(device_data->vtable.MapMemory(
            device_data->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
        memcpy(mapped, data, bytes);

        VkMappedMemoryRange range = {};
        range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = memory;
        range.size   = VK_WHOLE_SIZE;
        VK_CHECK(device_data->vtable.FlushMappedMemoryRanges(
            device_data->device, 1, &range));
        device_data->vtable.UnmapMemory(device_data->device, memory);
}

/*
 * Make sure the draw slot's vertex and index buffers are large enough
 * for this frame's quad, growing them (and marking them dirty for a
 * re-upload) when needed.
 *
 * device_data   The device.
 * data          Swapchain state (vertex array to upload).
 * draw          The draw slot owning the GPU buffers.
 */
RENDER_API void ensure_quad_buffers(device_data_t* device_data,
                                    swapchain_data_t* data,
                                    krosshair_draw_t* draw)
{
        size_t vertex_size = sizeof(data->vertices);
        size_t index_size  = sizeof(indices);
        if (draw->vertex_buffer_size < vertex_size) {
                create_or_resize_buffer(device_data, &draw->vertex_buffer,
                                        &draw->vertex_buffer_mem,
                                        &draw->vertex_buffer_size, vertex_size,
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                draw->vertex_buffer_initialized = 0;
        }
        if (draw->index_buffer_size < index_size) {
                create_or_resize_buffer(device_data, &draw->index_buffer,
                                        &draw->index_buffer_mem,
                                        &draw->index_buffer_size, index_size,
                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
                draw->index_buffer_initialized = 0;
        }

        if (!draw->vertex_buffer_initialized) {
                upload_device_memory(device_data, draw->vertex_buffer_mem,
                                     data->vertices, sizeof(data->vertices));
                draw->vertex_buffer_initialized = 1;
        }
        if (!draw->index_buffer_initialized) {
                upload_device_memory(device_data, draw->index_buffer_mem,
                                     indices, sizeof(indices));
                draw->index_buffer_initialized = 1;
        }
}

/*
 * Record the optional second draw call: the shader-based dynamic effect
 * mask that samples the game framebuffer copy (see record_framebuffer_copy).
 * Only runs when the mask is uploaded, the shader pipeline exists and the
 * framebuffer copy texture is available.
 *
 * data  Swapchain state (mask texture/vertices, descriptor set, push
 *      constants).
 * draw  The draw slot (second vertex buffer, command buffer).
 */
RENDER_API void record_dynamic_mask_draw(swapchain_data_t* data,
                                         krosshair_draw_t* draw)
{
        device_data_t* device_data = data->device_data;
        if (!data->dynamic_mask.uploaded || !device_data->shader_pipeline ||
            !data->game_fb_image_view)
                return;

        /* ensure the mask's own vertex buffer exists and is up to date */
        size_t mask_vertex_size = sizeof(data->dynamic_mask.vertices);
        if (draw->vertex_buffer2_size < mask_vertex_size) {
                create_or_resize_buffer(device_data, &draw->vertex_buffer2,
                                        &draw->vertex_buffer2_mem,
                                        &draw->vertex_buffer2_size, mask_vertex_size,
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        }
        upload_device_memory(device_data, draw->vertex_buffer2_mem,
                             data->dynamic_mask.vertices,
                             sizeof(data->dynamic_mask.vertices));

        device_data->vtable.CmdBindPipeline(
            draw->cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            device_data->shader_pipeline);

        VkDeviceSize mask_offsets[1] = {0};
        device_data->vtable.CmdBindVertexBuffers(
            draw->cmd_buffer, 0, 1, &draw->vertex_buffer2, mask_offsets);

        /* allocate the descriptor set (mask texture + game FB copy) once */
        if (!data->shader_mask_desc_set) {
                VkDescriptorSetAllocateInfo alloc_info = {};
                alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                alloc_info.descriptorPool     = device_data->shader_desc_pool;
                alloc_info.descriptorSetCount = 1;
                alloc_info.pSetLayouts        = &device_data->shader_desc_layout;
                VK_CHECK(device_data->vtable.AllocateDescriptorSets(
                    device_data->device, &alloc_info,
                    &data->shader_mask_desc_set));

                VkDescriptorImageInfo mask_image = {};
                mask_image.sampler     = device_data->crosshair_sampler;
                mask_image.imageView   = data->dynamic_mask.image_view;
                mask_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkDescriptorImageInfo fb_image = {};
                fb_image.sampler     = device_data->crosshair_sampler;
                fb_image.imageView   = data->game_fb_image_view;
                fb_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet writes[2] = {};
                writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet          = data->shader_mask_desc_set;
                writes[0].dstBinding      = 0;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[0].pImageInfo      = &mask_image;
                writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet          = data->shader_mask_desc_set;
                writes[1].dstBinding      = 1;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].pImageInfo      = &fb_image;
                device_data->vtable.UpdateDescriptorSets(
                    device_data->device, 2, writes, 0, NULL);
        }

        device_data->vtable.CmdBindDescriptorSets(
            draw->cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            device_data->shader_pipeline_layout, 0, 1,
            &data->shader_mask_desc_set, 0, NULL);

        /* fill the quad's NDC bounds into push constants (the shader fades
         * the effect across the quad) */
        data->dynamic_pc.quad_ndc_min[0]  = data->dynamic_mask.vertices[0].pos.x;
        data->dynamic_pc.quad_ndc_min[1]  = data->dynamic_mask.vertices[0].pos.y;
        data->dynamic_pc.quad_ndc_size[0] = data->dynamic_mask.vertices[2].pos.x -
                                            data->dynamic_mask.vertices[0].pos.x;
        data->dynamic_pc.quad_ndc_size[1] = data->dynamic_mask.vertices[2].pos.y -
                                            data->dynamic_mask.vertices[0].pos.y;

        device_data->vtable.CmdPushConstants(
            draw->cmd_buffer, device_data->shader_pipeline_layout,
            VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(data->dynamic_pc), &data->dynamic_pc);

        device_data->vtable.CmdDrawIndexed(
            draw->cmd_buffer,
            sizeof(indices) / sizeof(indices[0]),
            1, 0, 0, 0);
}

/*
 * Submit the recorded overlay command buffer to the GPU.
 *
 * If the present queue differs from the graphics queue and the app did not
 * give us a wait semaphore, two submits are used with an intermediate
 * semaphore (signal on the present queue, wait on the graphics queue) to
 * synchronize the engines ourselves. Otherwise a single submit waits on
 * the app's semaphores in the color-attachment stage.
 *
 * device_data         The device.
 * draw                The draw slot (command buffer, semaphores, fence).
 * present_queue       The queue the image will be presented on.
 * wait_semaphores     App's wait semaphores for this swapchain image
 *                     (may be NULL when n_wait_semaphores is 0).
 * n_wait_semaphores   How many of them.
 *
 * Returns 0 on success, -1 on failure (the caller skips this frame).
 */
RENDER_API int submit_overlay_draw(device_data_t* device_data,
                                   krosshair_draw_t* draw,
                                   queue_data_t* present_queue,
                                   const VkSemaphore* wait_semaphores,
                                   unsigned n_wait_semaphores)
{
        VkResult submit_result = VK_SUCCESS;

        if (n_wait_semaphores == 0 &&
            device_data->graphic_queue->queue != present_queue->queue) {
                VkPipelineStageFlags stages_wait =
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo submit_info       = {};
                submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit_info.commandBufferCount = 0;
                submit_info.pWaitDstStageMask  = &stages_wait;
                submit_info.waitSemaphoreCount = 0;
                submit_info.signalSemaphoreCount = 1;
                submit_info.pSignalSemaphores    = &draw->crossengine_semaphore;

                submit_result = device_data->vtable.QueueSubmit(
                    present_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
                if (submit_result != VK_SUCCESS) {
                        KROSSHAIR_LOG("[KROSSHAIR] crossengine QueueSubmit failed: %d\n",
                                      submit_result);
                        return -1;
                }

                submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit_info.commandBufferCount = 1;
                submit_info.pWaitDstStageMask  = &stages_wait;
                submit_info.pCommandBuffers    = &draw->cmd_buffer;
                submit_info.waitSemaphoreCount = 1;
                submit_info.pWaitSemaphores    = &draw->crossengine_semaphore;
                submit_info.signalSemaphoreCount = 1;
                submit_info.pSignalSemaphores    = &draw->semaphore;

                submit_result = device_data->vtable.QueueSubmit(
                    device_data->graphic_queue->queue, 1, &submit_info,
                    draw->fence);
                if (submit_result != VK_SUCCESS) {
                        KROSSHAIR_LOG("[KROSSHAIR] graphic QueueSubmit failed: %d\n",
                                      submit_result);
                        return -1;
                }
                draw->fence_submitted = 1;
        } else {
                /* wait in the fragment stage until the swapchain image is ready
                 */
                VkPipelineStageFlags* stages_wait =
                    malloc(sizeof(*stages_wait) * (n_wait_semaphores ? n_wait_semaphores : 1));
                for (unsigned s = 0; s < n_wait_semaphores; s++)
                        stages_wait[s] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

                VkSubmitInfo submit_info       = {};
                submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit_info.commandBufferCount = 1;
                submit_info.pCommandBuffers    = &draw->cmd_buffer;
                submit_info.pWaitDstStageMask  = stages_wait;
                submit_info.waitSemaphoreCount = n_wait_semaphores;
                submit_info.pWaitSemaphores    = wait_semaphores;
                submit_info.signalSemaphoreCount = 1;
                submit_info.pSignalSemaphores    = &draw->semaphore;

                submit_result = device_data->vtable.QueueSubmit(
                    device_data->graphic_queue->queue, 1, &submit_info,
                    draw->fence);
                free(stages_wait);
                if (submit_result != VK_SUCCESS) {
                        KROSSHAIR_LOG("[KROSSHAIR] QueueSubmit failed: %d (wait_sems=%u)\n",
                                      submit_result, n_wait_semaphores);
                        return -1;
                }
                draw->fence_submitted = 1;
        }

        return 0;
}

/*
 * Render and submit the crosshair overlay for one swapchain image.
 *
 * swapchain_data    Per-swapchain state (images, framebuffers, texture,
 *                  animation, quad vertices, draw slots).
 * present_queue     The queue this image is being presented on.
 * wait_semaphores   The app's wait semaphores for this image (NULL when
 *                  n_wait_semaphores is 0).
 * n_wait_semaphores How many of the above.
 * image_index       Index of the swapchain image to render onto.
 *
 * Returns the draw slot whose semaphore must be waited on by
 * vkQueuePresentKHR, or NULL (overlay skipped for this frame).
 */
krosshair_draw_t* render_swapchain_display(
    swapchain_data_t* swapchain_data, queue_data_t* present_queue,
    const VkSemaphore* wait_semaphores, unsigned n_wait_semaphores,
    unsigned image_index)
{
        if (!crosshair_visible)
                return NULL;

        device_data_t* device_data = swapchain_data->device_data;

        krosshair_draw_t* draw = swapchain_data->draws[image_index];
        if (!draw) {
                KROSSHAIR_LOG("[KROSSHAIR] no draw slot for image %u\n", image_index);
                return NULL;
        }

        /* Slot reuse: wait for this slot's previous submit to complete
         * before re-recording its command buffer.  If it does not complete
         * in time, skip this frame (presented without the overlay
         * semaphore) — never destroy a slot while its submit is in flight. */
        if (!wait_draw_slot_ready(device_data, draw, image_index))
                return NULL;

        device_data->vtable.ResetCommandBuffer(draw->cmd_buffer, 0);

        VkRenderPassBeginInfo render_pass_info = {};
        render_pass_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_info.renderPass  = device_data->render_pass;
        render_pass_info.framebuffer = swapchain_data->framebuffers[image_index];
        render_pass_info.renderArea.extent.width   = swapchain_data->width;
        render_pass_info.renderArea.extent.height  = swapchain_data->height;

        VkCommandBufferBeginInfo buffer_begin_info = {};
        buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        device_data->vtable.BeginCommandBuffer(draw->cmd_buffer,
                                               &buffer_begin_info);
        ensure_swapchain_crosshair(swapchain_data, draw->cmd_buffer);
        ensure_swapchain_dynamic_mask(swapchain_data, draw->cmd_buffer);

        if (swapchain_data->dynamic_mask.uploaded) {
                /* shader-based mode: copy the game framebuffer so the
                 * mask shader can sample it */
                ensure_game_fb_copy(swapchain_data);
                record_framebuffer_copy(swapchain_data, draw->cmd_buffer,
                                        image_index, present_queue);
        } else {
                transition_swapchain_for_render(swapchain_data, draw->cmd_buffer,
                                                image_index, present_queue);
        }

        device_data->vtable.CmdBeginRenderPass(
            draw->cmd_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

        /* advance animation frame if needed (GIF or APNG) */
        advance_anim_frame(swapchain_data, draw);

        ensure_quad_buffers(device_data, swapchain_data, draw);

        device_data->vtable.CmdBindPipeline(
            draw->cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            device_data->pipeline);

        VkDeviceSize offsets[1] = {0};
        device_data->vtable.CmdBindVertexBuffers(draw->cmd_buffer, 0, 1,
                                                 &draw->vertex_buffer, offsets);
        device_data->vtable.CmdBindIndexBuffer(
            draw->cmd_buffer, draw->index_buffer, 0, VK_INDEX_TYPE_UINT16);

        VkViewport viewport = {};
        viewport.x          = 0;
        viewport.y          = 0;
        viewport.width      = swapchain_data->width;
        viewport.height     = swapchain_data->height;
        viewport.minDepth   = 0.0f;
        viewport.maxDepth   = 1.0f;
        device_data->vtable.CmdSetViewport(draw->cmd_buffer, 0, 1, &viewport);

        VkRect2D scissor      = {};
        scissor.offset.x      = 0;
        scissor.offset.y      = 0;
        scissor.extent.width  = swapchain_data->width;
        scissor.extent.height = swapchain_data->height;
        device_data->vtable.CmdSetScissor(draw->cmd_buffer, 0, 1, &scissor);

        device_data->vtable.CmdBindDescriptorSets(
            draw->cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            device_data->pipeline_layout, 0, 1, &swapchain_data->descriptor_set, 0,
            NULL);
        device_data->vtable.CmdDrawIndexed(draw->cmd_buffer,
                                           sizeof(indices) / sizeof(indices[0]),
                                           1, 0, 0, 0);

        /* single dynamic effect mask draw (optional) */
        record_dynamic_mask_draw(swapchain_data, draw);

        device_data->vtable.CmdEndRenderPass(draw->cmd_buffer);

        /*
         * transfer the image back to the present queue family
         * image layout was already changed to present by the render pass
         */
        if (device_data->graphic_queue->family_index !=
            present_queue->family_index) {
                VkImageMemoryBarrier return_barrier = {};
                return_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                return_barrier.image         = swapchain_data->images[image_index];
                return_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                return_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                return_barrier.oldLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                return_barrier.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                return_barrier.srcQueueFamilyIndex =
                    device_data->graphic_queue->family_index;
                return_barrier.dstQueueFamilyIndex = present_queue->family_index;
                return_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                return_barrier.subresourceRange.baseMipLevel   = 0;
                return_barrier.subresourceRange.levelCount     = 1;
                return_barrier.subresourceRange.baseArrayLayer = 0;
                return_barrier.subresourceRange.layerCount     = 1;
                device_data->vtable.CmdPipelineBarrier(
                    draw->cmd_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0, NULL, 1,
                    &return_barrier);
        }

        VkResult end_result = device_data->vtable.EndCommandBuffer(draw->cmd_buffer);
        if (end_result != VK_SUCCESS) {
                KROSSHAIR_LOG("[KROSSHAIR] EndCommandBuffer failed: %d\n", end_result);
                return NULL;
        }

        if (submit_overlay_draw(device_data, draw, present_queue,
                                wait_semaphores, n_wait_semaphores) != 0)
                return NULL;

        return draw;
}
