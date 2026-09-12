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

krosshair_draw_t* render_swapchain_display(
    swapchain_data_t* data, queue_data_t* present_queue,
    const VkSemaphore* wait_semaphores, unsigned n_wait_semaphores,
    unsigned image_index)
{
        if (!crosshair_visible)
                return NULL;

        device_data_t* device_data = data->device_data;

        krosshair_draw_t* draw = data->draws[image_index];
        if (!draw) {
                KROSSHAIR_LOG("[KROSSHAIR] no draw slot for image %u\n", image_index);
                return NULL;
        }

        /* Slot reuse: wait for this slot's previous submit to complete
         * before re-recording its command buffer.  If it does not complete
         * in time, skip this frame (presented without the overlay
         * semaphore) — never destroy a slot while its submit is in flight. */
        if (draw->fence_submitted) {
                VkResult wait_result = device_data->vtable.WaitForFences(
                    device_data->device, 1, &draw->fence, VK_TRUE, 100000000);
                if (wait_result != VK_SUCCESS) {
                        KROSSHAIR_LOG("[KROSSHAIR] slot %u fence timeout, skipping frame\n",
                                       image_index);
                        return NULL;
                }
                VK_CHECK(device_data->vtable.ResetFences(
                    device_data->device, 1, &draw->fence));
                draw->fence_submitted = 0;
        }
        device_data->vtable.ResetCommandBuffer(draw->cmd_buffer, 0);

        VkRenderPassBeginInfo render_pass_info = {};
        render_pass_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_info.renderPass  = data->device_data->render_pass;
        render_pass_info.framebuffer = data->framebuffers[image_index];
        render_pass_info.renderArea.extent.width   = data->width;
        render_pass_info.renderArea.extent.height  = data->height;

        VkCommandBufferBeginInfo buffer_begin_info = {};
        buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        device_data->vtable.BeginCommandBuffer(draw->cmd_buffer,
                                               &buffer_begin_info);
        ensure_swapchain_crosshair(data, draw->cmd_buffer);
        ensure_swapchain_dynamic_mask(data, draw->cmd_buffer);

        /* FB copy needed whenever the dynamic mask is active */
        int needs_fb_copy = data->dynamic_mask.uploaded;

        VkImageMemoryBarrier imb = {};
        imb.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imb.pNext                = NULL;
        imb.image                = data->images[image_index];
        imb.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        imb.subresourceRange.baseMipLevel   = 0;
        imb.subresourceRange.levelCount     = 1;
        imb.subresourceRange.baseArrayLayer = 0;
        imb.subresourceRange.layerCount     = 1;
        imb.srcQueueFamilyIndex             = present_queue->family_index;
        imb.dstQueueFamilyIndex = device_data->graphic_queue->family_index;

        if (needs_fb_copy) {
                /* ── copy game framebuffer for shader-based modes ──
                 * 1) PRESENT_SRC → TRANSFER_SRC  (so we can read)
                 * 2) blit full swapchain to game_fb_image
                 * 3) TRANSFER_SRC → COLOR_ATTACHMENT (so render pass works)
                 */

                /* ensure game_fb texture exists at swapchain resolution */
                if (!data->game_fb_image ||
                    data->game_fb_width != data->width ||
                    data->game_fb_height != data->height) {
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
                                device_data->vtable.FreeMemory(
                                    device_data->device, data->game_fb_mem, NULL);
                                data->game_fb_mem = VK_NULL_HANDLE;
                        }

                        VkImageCreateInfo fbi = {};
                        fbi.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                        fbi.imageType     = VK_IMAGE_TYPE_2D;
                        fbi.format        = data->format;
                        fbi.extent.width  = data->width;
                        fbi.extent.height = data->height;
                        fbi.extent.depth  = 1;
                        fbi.mipLevels     = 1;
                        fbi.arrayLayers   = 1;
                        fbi.samples       = VK_SAMPLE_COUNT_1_BIT;
                        fbi.tiling        = VK_IMAGE_TILING_OPTIMAL;
                        fbi.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                        fbi.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
                        fbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        VK_CHECK(device_data->vtable.CreateImage(
                            device_data->device, &fbi, NULL, &data->game_fb_image));

                        VkMemoryRequirements mreq;
                        device_data->vtable.GetImageMemoryRequirements(
                            device_data->device, data->game_fb_image, &mreq);

                        VkMemoryAllocateInfo mai = {};
                        mai.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                        mai.allocationSize = mreq.size;
                        mai.memoryTypeIndex =
                            vk_memory_type(device_data,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                           mreq.memoryTypeBits);
                        VK_CHECK(device_data->vtable.AllocateMemory(
                            device_data->device, &mai, NULL, &data->game_fb_mem));
                        VK_CHECK(device_data->vtable.BindImageMemory(
                            device_data->device, data->game_fb_image,
                            data->game_fb_mem, 0));

                        VkImageViewCreateInfo fvi = {};
                        fvi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                        fvi.image    = data->game_fb_image;
                        fvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                        fvi.format   = data->format;
                        fvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        fvi.subresourceRange.levelCount = 1;
                        fvi.subresourceRange.layerCount = 1;
                        VK_CHECK(device_data->vtable.CreateImageView(
                            device_data->device, &fvi, NULL,
                            &data->game_fb_image_view));

                        data->game_fb_width  = data->width;
                        data->game_fb_height = data->height;

                        KROSSHAIR_LOG("[KROSSHAIR] created game FB copy texture %ux%u\n",
                                      data->width, data->height);
                }

                /* transition swapchain: PRESENT_SRC → TRANSFER_SRC */
                imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                imb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                imb.oldLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                imb.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                device_data->vtable.CmdPipelineBarrier(
                    draw->cmd_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);

                /* transition game_fb: UNDEFINED → TRANSFER_DST */
                VkImageMemoryBarrier fb_bar = {};
                fb_bar.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                fb_bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                fb_bar.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
                fb_bar.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                fb_bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                fb_bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                fb_bar.image         = data->game_fb_image;
                fb_bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                fb_bar.subresourceRange.levelCount = 1;
                fb_bar.subresourceRange.layerCount = 1;
                device_data->vtable.CmdPipelineBarrier(
                    draw->cmd_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &fb_bar);

                /* copy swapchain → game_fb */
                VkImageCopy region = {};
                region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.srcSubresource.layerCount = 1;
                region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.dstSubresource.layerCount = 1;
                region.extent.width  = data->width;
                region.extent.height = data->height;
                region.extent.depth  = 1;
                device_data->vtable.CmdCopyImage(
                    draw->cmd_buffer,
                    data->images[image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    data->game_fb_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &region);

                /* transition game_fb: TRANSFER_DST → SHADER_READ_ONLY */
                fb_bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                fb_bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                fb_bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                fb_bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                device_data->vtable.CmdPipelineBarrier(
                    draw->cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, NULL, 0, NULL, 1, &fb_bar);

                /* transition swapchain: TRANSFER_SRC → COLOR_ATTACHMENT */
                imb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                imb.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                imb.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                device_data->vtable.CmdPipelineBarrier(
                    draw->cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    0, 0, NULL, 0, NULL, 1, &imb);
        } else {
                /* no FB copy needed — simple transition */
                imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                imb.oldLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                imb.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                device_data->vtable.CmdPipelineBarrier(
                    draw->cmd_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                    0, 0, NULL, 0, NULL, 1, &imb);
        }

        device_data->vtable.CmdBeginRenderPass(
            draw->cmd_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

        /* advance animation frame if needed (GIF or APNG) */
        if (data->anim_frame_count > 1 && data->anim_delays) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);

                long elapsed_ms =
                    (now.tv_sec - data->anim_last_frame_time.tv_sec) * 1000 +
                    (now.tv_nsec - data->anim_last_frame_time.tv_nsec) / 1000000;

                int advanced = 0;
                int delay = data->anim_delays[data->anim_current_frame];

                /* consume all elapsed time, advancing multiple frames if
                 * the present rate is lower than the animation rate */
                while (elapsed_ms >= delay) {
                        /* accumulate: add delay to last_frame_time instead
                         * of resetting to now, so leftover time carries
                         * over and the animation stays in sync */
                        data->anim_last_frame_time.tv_nsec += (long)delay * 1000000L;
                        while (data->anim_last_frame_time.tv_nsec >= 1000000000L) {
                                data->anim_last_frame_time.tv_sec++;
                                data->anim_last_frame_time.tv_nsec -= 1000000000L;
                        }

                        data->anim_current_frame =
                            (data->anim_current_frame + 1) % data->anim_frame_count;
                        delay = data->anim_delays[data->anim_current_frame];
                        advanced = 1;

                        /* recalculate elapsed from updated base */
                        elapsed_ms =
                            (now.tv_sec - data->anim_last_frame_time.tv_sec) * 1000 +
                            (now.tv_nsec - data->anim_last_frame_time.tv_nsec) / 1000000;
                }

                if (advanced) {
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
                void* vtx_dst = NULL;
                VK_CHECK(device_data->vtable.MapMemory(
                    device_data->device, draw->vertex_buffer_mem, 0,
                    draw->vertex_buffer_size, 0, &vtx_dst));
                memcpy(vtx_dst, data->vertices, sizeof(data->vertices));

                VkMappedMemoryRange vtx_range = {};
                vtx_range.sType               = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                vtx_range.memory              = draw->vertex_buffer_mem;
                vtx_range.size                = VK_WHOLE_SIZE;
                VK_CHECK(device_data->vtable.FlushMappedMemoryRanges(
                    device_data->device, 1, &vtx_range));
                device_data->vtable.UnmapMemory(device_data->device,
                                                draw->vertex_buffer_mem);
                draw->vertex_buffer_initialized = 1;
        }

        if (!draw->index_buffer_initialized) {
                void* idx_dst = NULL;
                VK_CHECK(device_data->vtable.MapMemory(
                    device_data->device, draw->index_buffer_mem, 0,
                    draw->index_buffer_size, 0, &idx_dst));
                memcpy(idx_dst, indices, sizeof(indices));

                VkMappedMemoryRange idx_range = {};
                idx_range.sType               = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                idx_range.memory              = draw->index_buffer_mem;
                idx_range.size                = VK_WHOLE_SIZE;
                VK_CHECK(device_data->vtable.FlushMappedMemoryRanges(
                    device_data->device, 1, &idx_range));
                device_data->vtable.UnmapMemory(device_data->device,
                                                draw->index_buffer_mem);
                draw->index_buffer_initialized = 1;
        }

        device_data->vtable.CmdBindPipeline(
            draw->cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            data->device_data->pipeline);

        VkDeviceSize offsets[1] = {0};
        device_data->vtable.CmdBindVertexBuffers(draw->cmd_buffer, 0, 1,
                                                 &draw->vertex_buffer, offsets);
        device_data->vtable.CmdBindIndexBuffer(
            draw->cmd_buffer, draw->index_buffer, 0, VK_INDEX_TYPE_UINT16);

        VkViewport viewport = {};
        viewport.x          = 0;
        viewport.y          = 0;
        viewport.width      = data->width;
        viewport.height     = data->height;
        viewport.minDepth   = 0.0f;
        viewport.maxDepth   = 1.0f;
        device_data->vtable.CmdSetViewport(draw->cmd_buffer, 0, 1, &viewport);

        VkRect2D scissor      = {};
        scissor.offset.x      = 0;
        scissor.offset.y      = 0;
        scissor.extent.width  = data->width;
        scissor.extent.height = data->height;
        device_data->vtable.CmdSetScissor(draw->cmd_buffer, 0, 1, &scissor);

        device_data->vtable.CmdBindDescriptorSets(
            draw->cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            data->device_data->pipeline_layout, 0, 1, &data->descriptor_set, 0,
            NULL);
        device_data->vtable.CmdDrawIndexed(draw->cmd_buffer,
                                            sizeof(indices) / sizeof(indices[0]),
                                            1, 0, 0, 0);

        /* ── single dynamic effect mask draw (optional) ──
         * Uses the shader pipeline that samples the game FB copy.
         * Only runs if the dynamic mask is uploaded and the shader pipeline exists.
         */
        if (data->dynamic_mask.uploaded && data->device_data->shader_pipeline &&
            data->game_fb_image_view) {
                /* ensure vertex_buffer2 exists for dynamic mask */
                size_t vtx2_size = sizeof(data->dynamic_mask.vertices);
                if (draw->vertex_buffer2_size < vtx2_size) {
                        create_or_resize_buffer(device_data, &draw->vertex_buffer2,
                                                &draw->vertex_buffer2_mem,
                                                &draw->vertex_buffer2_size, vtx2_size,
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                }

                /* upload dynamic mask vertices to separate buffer */
                void* vtx_dst = NULL;
                VK_CHECK(device_data->vtable.MapMemory(
                    device_data->device, draw->vertex_buffer2_mem, 0,
                    draw->vertex_buffer2_size, 0, &vtx_dst));
                memcpy(vtx_dst, data->dynamic_mask.vertices,
                       sizeof(data->dynamic_mask.vertices));

                VkMappedMemoryRange vtx_range = {};
                vtx_range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                vtx_range.memory = draw->vertex_buffer2_mem;
                vtx_range.size   = VK_WHOLE_SIZE;
                VK_CHECK(device_data->vtable.FlushMappedMemoryRanges(
                    device_data->device, 1, &vtx_range));
                device_data->vtable.UnmapMemory(device_data->device,
                                                draw->vertex_buffer2_mem);

                device_data->vtable.CmdBindPipeline(
                    draw->cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    data->device_data->shader_pipeline);

                VkDeviceSize mask_offsets[1] = {0};
                device_data->vtable.CmdBindVertexBuffers(
                    draw->cmd_buffer, 0, 1, &draw->vertex_buffer2, mask_offsets);

                /* allocate/update descriptor set for mask + game_fb if needed */
                if (!data->shader_mask_desc_set) {
                        VkDescriptorSetAllocateInfo dsai = {};
                        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                        dsai.descriptorPool     = data->device_data->shader_desc_pool;
                        dsai.descriptorSetCount = 1;
                        dsai.pSetLayouts        = &data->device_data->shader_desc_layout;
                        VK_CHECK(device_data->vtable.AllocateDescriptorSets(
                            device_data->device, &dsai,
                            &data->shader_mask_desc_set));

                        VkDescriptorImageInfo di_mask = {};
                        di_mask.sampler     = data->device_data->crosshair_sampler;
                        di_mask.imageView   = data->dynamic_mask.image_view;
                        di_mask.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                        VkDescriptorImageInfo di_fb = {};
                        di_fb.sampler     = data->device_data->crosshair_sampler;
                        di_fb.imageView   = data->game_fb_image_view;
                        di_fb.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                        VkWriteDescriptorSet writes[2] = {};
                        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[0].dstSet          = data->shader_mask_desc_set;
                        writes[0].dstBinding      = 0;
                        writes[0].descriptorCount = 1;
                        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[0].pImageInfo      = &di_mask;
                        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[1].dstSet          = data->shader_mask_desc_set;
                        writes[1].dstBinding      = 1;
                        writes[1].descriptorCount = 1;
                        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[1].pImageInfo      = &di_fb;
                        device_data->vtable.UpdateDescriptorSets(
                            device_data->device, 2, writes, 0, NULL);
                }

                device_data->vtable.CmdBindDescriptorSets(
                    draw->cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    data->device_data->shader_pipeline_layout, 0, 1,
                    &data->shader_mask_desc_set, 0, NULL);

                /* fill quad NDC bounds into push constants */
                data->dynamic_pc.quad_ndc_min[0]  = data->dynamic_mask.vertices[0].pos.x;
                data->dynamic_pc.quad_ndc_min[1]  = data->dynamic_mask.vertices[0].pos.y;
                data->dynamic_pc.quad_ndc_size[0] = data->dynamic_mask.vertices[2].pos.x -
                                                    data->dynamic_mask.vertices[0].pos.x;
                data->dynamic_pc.quad_ndc_size[1] = data->dynamic_mask.vertices[2].pos.y -
                                                    data->dynamic_mask.vertices[0].pos.y;

                device_data->vtable.CmdPushConstants(
                    draw->cmd_buffer, data->device_data->shader_pipeline_layout,
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                    sizeof(data->dynamic_pc), &data->dynamic_pc);

                device_data->vtable.CmdDrawIndexed(
                    draw->cmd_buffer,
                    sizeof(indices) / sizeof(indices[0]),
                    1, 0, 0, 0);
        }

        device_data->vtable.CmdEndRenderPass(draw->cmd_buffer);

        /*
         * transfer the image back to the present queue family
         * image layout was already changed to present by the render pass
         */
        if (device_data->graphic_queue->family_index !=
            present_queue->family_index) {
                imb.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                imb.pNext         = NULL;
                imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                imb.oldLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                imb.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                imb.image         = data->images[image_index];
                imb.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                imb.subresourceRange.baseMipLevel   = 0;
                imb.subresourceRange.levelCount     = 1;
                imb.subresourceRange.baseArrayLayer = 0;
                imb.subresourceRange.layerCount     = 1;
                imb.srcQueueFamilyIndex =
                    device_data->graphic_queue->family_index;
                imb.dstQueueFamilyIndex = present_queue->family_index;
                device_data->vtable.CmdPipelineBarrier(
                    draw->cmd_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0, NULL, 1,
                    &imb);
        }

        VkResult end_result = device_data->vtable.EndCommandBuffer(draw->cmd_buffer);
        if (end_result != VK_SUCCESS) {
                KROSSHAIR_LOG("[KROSSHAIR] EndCommandBuffer failed: %d\n", end_result);
                return NULL;
        }

        /* when presenting on a different queue than where we're drawing the
         * crosshair *AND* when the application does not provide a semaphore to
         * vkQueuePresent, insert our own cross-engine synchronization
         * semaphore.
         * */
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
                        return NULL;
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
                        return NULL;
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
                        return NULL;
                }
                draw->fence_submitted = 1;
        }

        return draw;
}

/*
 * GPU resources that do not depend on the swapchain (sampler, descriptor
 * pools/layouts, command pool, pipeline layouts).  Created once per device
 * by overlay_CreateDevice, destroyed only in overlay_DestroyDevice.
 */
