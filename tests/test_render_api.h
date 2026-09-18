/*
 * Declarations for the otherwise-static helpers in src/render.c, exposed
 * via the RENDER_API macro in UNIT_TEST builds so tests/test_render.c
 * can drive them directly. render_swapchain_display is already exported
 * via krosshair.h.
 */
#ifndef TEST_RENDER_API_H
#define TEST_RENDER_API_H

#include <stddef.h>
#include <vulkan/vulkan.h>
#include "krosshair.h"

int wait_draw_slot_ready(device_data_t* device_data,
                         krosshair_draw_t* draw, unsigned image_index);
void ensure_game_fb_copy(swapchain_data_t* data);
void record_framebuffer_copy(swapchain_data_t* data,
                             VkCommandBuffer cmd_buffer,
                             unsigned image_index, queue_data_t* present_queue);
void transition_swapchain_for_render(swapchain_data_t* data,
                                     VkCommandBuffer cmd_buffer,
                                     unsigned image_index,
                                     queue_data_t* present_queue);
void advance_anim_frame(swapchain_data_t* data, krosshair_draw_t* draw);
void upload_device_memory(device_data_t* device_data, VkDeviceMemory memory,
                          const void* data, size_t bytes);
void ensure_quad_buffers(device_data_t* device_data, swapchain_data_t* data,
                         krosshair_draw_t* draw);
void record_dynamic_mask_draw(swapchain_data_t* data, krosshair_draw_t* draw);
int submit_overlay_draw(device_data_t* device_data, krosshair_draw_t* draw,
                        queue_data_t* present_queue,
                        const VkSemaphore* wait_semaphores,
                        unsigned n_wait_semaphores);

#endif /* TEST_RENDER_API_H */
