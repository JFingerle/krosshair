#include "../include/dispatch.h"
#include <string.h>

#define DISPATCH_LOAD(TABLE, GPA, SCOPE, NAME) \
    (TABLE)->NAME = (PFN_vk##NAME)GPA(SCOPE, "vk" #NAME)

/*
 * Return the Vulkan object handle stored in the first word of a wrapped
 * dispatch instance.
 *
 * inst  Wrapped dispatch instance (e.g. instance_data_t); its first
 *       member is the original Vulkan object pointer (VkInstance,
 *       VkDevice, ...).
 */
void* get_key(void* inst) {
    return *(void**)inst;
}

/*
 * Populate an instance-level dispatch table with function pointers
 * resolved through the instance's GetInstanceProcAddr.
 *
 * instance  The VkInstance against which instance-scoped commands are
 *           resolved.
 * gpa       The next-link GetInstanceProcAddr used for every lookup.
 * table     Dispatch table to fill; zeroed first, then each entry is set
 *           to the resolved function pointer (NULL if unavailable).
 */
void vk_load_instance_commands(VkInstance instance,
                               PFN_vkGetInstanceProcAddr gpa,
                               instance_dispatch_table_t* table) {
    memset(table, 0, sizeof(*table));
    table->GetInstanceProcAddr = gpa;
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
    DISPATCH_LOAD(table, gpa, instance, AcquireXlibDisplayEXT);
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    DISPATCH_LOAD(table, gpa, instance, CreateAndroidSurfaceKHR);
#endif
    DISPATCH_LOAD(table, gpa, instance, CreateDebugReportCallbackEXT);
    DISPATCH_LOAD(table, gpa, instance, CreateDebugUtilsMessengerEXT);
    DISPATCH_LOAD(table, gpa, instance, CreateDevice);
#ifdef VK_USE_PLATFORM_DIRECTFB_EXT
    DISPATCH_LOAD(table, gpa, instance, CreateDirectFBSurfaceEXT);
#endif
    DISPATCH_LOAD(table, gpa, instance, CreateDisplayModeKHR);
    DISPATCH_LOAD(table, gpa, instance, CreateDisplayPlaneSurfaceKHR);
    DISPATCH_LOAD(table, gpa, instance, CreateHeadlessSurfaceEXT);
#ifdef VK_USE_PLATFORM_IOS_MVK
    DISPATCH_LOAD(table, gpa, instance, CreateIOSSurfaceMVK);
#endif
#ifdef VK_USE_PLATFORM_FUCHSIA
    DISPATCH_LOAD(table, gpa, instance, CreateImagePipeSurfaceFUCHSIA);
#endif
    DISPATCH_LOAD(table, gpa, instance, CreateInstance);
#ifdef VK_USE_PLATFORM_MACOS_MVK
    DISPATCH_LOAD(table, gpa, instance, CreateMacOSSurfaceMVK);
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
    DISPATCH_LOAD(table, gpa, instance, CreateMetalSurfaceEXT);
#endif
#ifdef VK_USE_PLATFORM_GGP
    DISPATCH_LOAD(table, gpa, instance, CreateStreamDescriptorSurfaceGGP);
#endif
#ifdef VK_USE_PLATFORM_VI_NN
    DISPATCH_LOAD(table, gpa, instance, CreateViSurfaceNN);
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    DISPATCH_LOAD(table, gpa, instance, CreateWaylandSurfaceKHR);
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, instance, CreateWin32SurfaceKHR);
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
    DISPATCH_LOAD(table, gpa, instance, CreateXcbSurfaceKHR);
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
    DISPATCH_LOAD(table, gpa, instance, CreateXlibSurfaceKHR);
#endif
    DISPATCH_LOAD(table, gpa, instance, DebugReportMessageEXT);
    DISPATCH_LOAD(table, gpa, instance, DestroyDebugReportCallbackEXT);
    DISPATCH_LOAD(table, gpa, instance, DestroyDebugUtilsMessengerEXT);
    DISPATCH_LOAD(table, gpa, instance, DestroyInstance);
    DISPATCH_LOAD(table, gpa, instance, DestroySurfaceKHR);
    DISPATCH_LOAD(table, gpa, instance, EnumerateDeviceExtensionProperties);
    DISPATCH_LOAD(table, gpa, instance, EnumerateDeviceLayerProperties);
    DISPATCH_LOAD(table, gpa, instance, EnumerateInstanceExtensionProperties);
    DISPATCH_LOAD(table, gpa, instance, EnumerateInstanceLayerProperties);
    DISPATCH_LOAD(table, gpa, instance, EnumerateInstanceVersion);
    DISPATCH_LOAD(table, gpa, instance, EnumeratePhysicalDeviceGroups);
    DISPATCH_LOAD(table, gpa, instance, EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR);
    DISPATCH_LOAD(table, gpa, instance, EnumeratePhysicalDevices);
    DISPATCH_LOAD(table, gpa, instance, GetDisplayModeProperties2KHR);
    DISPATCH_LOAD(table, gpa, instance, GetDisplayModePropertiesKHR);
    DISPATCH_LOAD(table, gpa, instance, GetDisplayPlaneCapabilities2KHR);
    DISPATCH_LOAD(table, gpa, instance, GetDisplayPlaneCapabilitiesKHR);
    DISPATCH_LOAD(table, gpa, instance, GetDisplayPlaneSupportedDisplaysKHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceCalibrateableTimeDomainsEXT);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceCooperativeMatrixPropertiesNV);
#ifdef VK_USE_PLATFORM_DIRECTFB_EXT
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceDirectFBPresentationSupportEXT);
#endif
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceDisplayPlaneProperties2KHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceDisplayPlanePropertiesKHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceDisplayProperties2KHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceDisplayPropertiesKHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceExternalBufferProperties);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceExternalFenceProperties);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceExternalImageFormatPropertiesNV);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceExternalSemaphoreProperties);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceFeatures);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceFeatures2);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceFormatProperties);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceFormatProperties2);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceFragmentShadingRatesKHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceImageFormatProperties);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceImageFormatProperties2);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceMemoryProperties);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceMemoryProperties2);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceMultisamplePropertiesEXT);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDevicePresentRectanglesKHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceProperties);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceProperties2);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceQueueFamilyProperties);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceQueueFamilyProperties2);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSparseImageFormatProperties);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSparseImageFormatProperties2);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSurfaceCapabilities2EXT);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSurfaceCapabilities2KHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSurfaceCapabilitiesKHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSurfaceFormats2KHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSurfaceFormatsKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSurfacePresentModes2EXT);
#endif
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSurfacePresentModesKHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceSurfaceSupportKHR);
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceToolPropertiesEXT);
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceWaylandPresentationSupportKHR);
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceWin32PresentationSupportKHR);
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceXcbPresentationSupportKHR);
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
    DISPATCH_LOAD(table, gpa, instance, GetPhysicalDeviceXlibPresentationSupportKHR);
#endif
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
    DISPATCH_LOAD(table, gpa, instance, GetRandROutputDisplayEXT);
#endif
    DISPATCH_LOAD(table, gpa, instance, ReleaseDisplayEXT);
    DISPATCH_LOAD(table, gpa, instance, SubmitDebugUtilsMessageEXT);
}

/*
 * Populate a device-level dispatch table with function pointers resolved
 * through the device's GetDeviceProcAddr.
 *
 * device  The VkDevice against which device-scoped commands are resolved.
 * gpa     The GetDeviceProcAddr used for every lookup (stored in the
 *         table as well).
 * table   Dispatch table to fill; zeroed first, then each entry is set
 *         to the resolved function pointer (NULL if unavailable).
 */
void vk_load_device_commands(VkDevice device,
                             PFN_vkGetDeviceProcAddr gpa,
                             struct vk_device_dispatch_table* table) {
    memset(table, 0, sizeof(*table));
    table->GetDeviceProcAddr = gpa;
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, AcquireFullScreenExclusiveModeEXT);
#endif
    DISPATCH_LOAD(table, gpa, device, AcquireNextImage2KHR);
    DISPATCH_LOAD(table, gpa, device, AcquireNextImageKHR);
    DISPATCH_LOAD(table, gpa, device, AcquirePerformanceConfigurationINTEL);
    DISPATCH_LOAD(table, gpa, device, AcquireProfilingLockKHR);
    DISPATCH_LOAD(table, gpa, device, AllocateCommandBuffers);
    DISPATCH_LOAD(table, gpa, device, AllocateDescriptorSets);
    DISPATCH_LOAD(table, gpa, device, AllocateMemory);
    DISPATCH_LOAD(table, gpa, device, BeginCommandBuffer);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, BindAccelerationStructureMemoryKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, BindBufferMemory);
    DISPATCH_LOAD(table, gpa, device, BindBufferMemory2);
    DISPATCH_LOAD(table, gpa, device, BindImageMemory);
    DISPATCH_LOAD(table, gpa, device, BindImageMemory2);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, BuildAccelerationStructureKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CmdBeginConditionalRenderingEXT);
    DISPATCH_LOAD(table, gpa, device, CmdBeginDebugUtilsLabelEXT);
    DISPATCH_LOAD(table, gpa, device, CmdBeginQuery);
    DISPATCH_LOAD(table, gpa, device, CmdBeginQueryIndexedEXT);
    DISPATCH_LOAD(table, gpa, device, CmdBeginRenderPass);
    DISPATCH_LOAD(table, gpa, device, CmdBeginRenderPass2);
    DISPATCH_LOAD(table, gpa, device, CmdBeginTransformFeedbackEXT);
    DISPATCH_LOAD(table, gpa, device, CmdBindDescriptorSets);
    DISPATCH_LOAD(table, gpa, device, CmdBindIndexBuffer);
    DISPATCH_LOAD(table, gpa, device, CmdBindPipeline);
    DISPATCH_LOAD(table, gpa, device, CmdBindPipelineShaderGroupNV);
    DISPATCH_LOAD(table, gpa, device, CmdBindShadingRateImageNV);
    DISPATCH_LOAD(table, gpa, device, CmdBindTransformFeedbackBuffersEXT);
    DISPATCH_LOAD(table, gpa, device, CmdBindVertexBuffers);
    DISPATCH_LOAD(table, gpa, device, CmdBindVertexBuffers2EXT);
    DISPATCH_LOAD(table, gpa, device, CmdBlitImage);
    DISPATCH_LOAD(table, gpa, device, CmdBlitImage2KHR);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CmdBuildAccelerationStructureIndirectKHR);
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CmdBuildAccelerationStructureKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CmdBuildAccelerationStructureNV);
    DISPATCH_LOAD(table, gpa, device, CmdClearAttachments);
    DISPATCH_LOAD(table, gpa, device, CmdClearColorImage);
    DISPATCH_LOAD(table, gpa, device, CmdClearDepthStencilImage);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CmdCopyAccelerationStructureKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CmdCopyAccelerationStructureNV);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CmdCopyAccelerationStructureToMemoryKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CmdCopyBuffer);
    DISPATCH_LOAD(table, gpa, device, CmdCopyBuffer2KHR);
    DISPATCH_LOAD(table, gpa, device, CmdCopyBufferToImage);
    DISPATCH_LOAD(table, gpa, device, CmdCopyBufferToImage2KHR);
    DISPATCH_LOAD(table, gpa, device, CmdCopyImage);
    DISPATCH_LOAD(table, gpa, device, CmdCopyImage2KHR);
    DISPATCH_LOAD(table, gpa, device, CmdCopyImageToBuffer);
    DISPATCH_LOAD(table, gpa, device, CmdCopyImageToBuffer2KHR);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CmdCopyMemoryToAccelerationStructureKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CmdCopyQueryPoolResults);
    DISPATCH_LOAD(table, gpa, device, CmdDebugMarkerBeginEXT);
    DISPATCH_LOAD(table, gpa, device, CmdDebugMarkerEndEXT);
    DISPATCH_LOAD(table, gpa, device, CmdDebugMarkerInsertEXT);
    DISPATCH_LOAD(table, gpa, device, CmdDispatch);
    DISPATCH_LOAD(table, gpa, device, CmdDispatchBase);
    DISPATCH_LOAD(table, gpa, device, CmdDispatchIndirect);
    DISPATCH_LOAD(table, gpa, device, CmdDraw);
    DISPATCH_LOAD(table, gpa, device, CmdDrawIndexed);
    DISPATCH_LOAD(table, gpa, device, CmdDrawIndexedIndirect);
    DISPATCH_LOAD(table, gpa, device, CmdDrawIndexedIndirectCount);
    DISPATCH_LOAD(table, gpa, device, CmdDrawIndirect);
    DISPATCH_LOAD(table, gpa, device, CmdDrawIndirectByteCountEXT);
    DISPATCH_LOAD(table, gpa, device, CmdDrawIndirectCount);
    DISPATCH_LOAD(table, gpa, device, CmdDrawMeshTasksIndirectCountNV);
    DISPATCH_LOAD(table, gpa, device, CmdDrawMeshTasksIndirectNV);
    DISPATCH_LOAD(table, gpa, device, CmdDrawMeshTasksNV);
    DISPATCH_LOAD(table, gpa, device, CmdEndConditionalRenderingEXT);
    DISPATCH_LOAD(table, gpa, device, CmdEndDebugUtilsLabelEXT);
    DISPATCH_LOAD(table, gpa, device, CmdEndQuery);
    DISPATCH_LOAD(table, gpa, device, CmdEndQueryIndexedEXT);
    DISPATCH_LOAD(table, gpa, device, CmdEndRenderPass);
    DISPATCH_LOAD(table, gpa, device, CmdEndRenderPass2);
    DISPATCH_LOAD(table, gpa, device, CmdEndTransformFeedbackEXT);
    DISPATCH_LOAD(table, gpa, device, CmdExecuteCommands);
    DISPATCH_LOAD(table, gpa, device, CmdExecuteGeneratedCommandsNV);
    DISPATCH_LOAD(table, gpa, device, CmdFillBuffer);
    DISPATCH_LOAD(table, gpa, device, CmdInsertDebugUtilsLabelEXT);
    DISPATCH_LOAD(table, gpa, device, CmdNextSubpass);
    DISPATCH_LOAD(table, gpa, device, CmdNextSubpass2);
    DISPATCH_LOAD(table, gpa, device, CmdPipelineBarrier);
    DISPATCH_LOAD(table, gpa, device, CmdPreprocessGeneratedCommandsNV);
    DISPATCH_LOAD(table, gpa, device, CmdPushConstants);
    DISPATCH_LOAD(table, gpa, device, CmdPushDescriptorSetKHR);
    DISPATCH_LOAD(table, gpa, device, CmdPushDescriptorSetWithTemplateKHR);
    DISPATCH_LOAD(table, gpa, device, CmdResetEvent);
    DISPATCH_LOAD(table, gpa, device, CmdResetQueryPool);
    DISPATCH_LOAD(table, gpa, device, CmdResolveImage);
    DISPATCH_LOAD(table, gpa, device, CmdResolveImage2KHR);
    DISPATCH_LOAD(table, gpa, device, CmdSetBlendConstants);
    DISPATCH_LOAD(table, gpa, device, CmdSetCheckpointNV);
    DISPATCH_LOAD(table, gpa, device, CmdSetCoarseSampleOrderNV);
    DISPATCH_LOAD(table, gpa, device, CmdSetCullModeEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetDepthBias);
    DISPATCH_LOAD(table, gpa, device, CmdSetDepthBounds);
    DISPATCH_LOAD(table, gpa, device, CmdSetDepthBoundsTestEnableEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetDepthCompareOpEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetDepthTestEnableEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetDepthWriteEnableEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetDeviceMask);
    DISPATCH_LOAD(table, gpa, device, CmdSetDiscardRectangleEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetEvent);
    DISPATCH_LOAD(table, gpa, device, CmdSetExclusiveScissorNV);
    DISPATCH_LOAD(table, gpa, device, CmdSetFragmentShadingRateKHR);
    DISPATCH_LOAD(table, gpa, device, CmdSetFrontFaceEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetLineStippleEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetLineWidth);
    DISPATCH_LOAD(table, gpa, device, CmdSetPerformanceMarkerINTEL);
    DISPATCH_LOAD(table, gpa, device, CmdSetPerformanceOverrideINTEL);
    DISPATCH_LOAD(table, gpa, device, CmdSetPerformanceStreamMarkerINTEL);
    DISPATCH_LOAD(table, gpa, device, CmdSetPrimitiveTopologyEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetSampleLocationsEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetScissor);
    DISPATCH_LOAD(table, gpa, device, CmdSetScissorWithCountEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetStencilCompareMask);
    DISPATCH_LOAD(table, gpa, device, CmdSetStencilOpEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetStencilReference);
    DISPATCH_LOAD(table, gpa, device, CmdSetStencilTestEnableEXT);
    DISPATCH_LOAD(table, gpa, device, CmdSetStencilWriteMask);
    DISPATCH_LOAD(table, gpa, device, CmdSetViewport);
    DISPATCH_LOAD(table, gpa, device, CmdSetViewportShadingRatePaletteNV);
    DISPATCH_LOAD(table, gpa, device, CmdSetViewportWScalingNV);
    DISPATCH_LOAD(table, gpa, device, CmdSetViewportWithCountEXT);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CmdTraceRaysIndirectKHR);
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CmdTraceRaysKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CmdTraceRaysNV);
    DISPATCH_LOAD(table, gpa, device, CmdUpdateBuffer);
    DISPATCH_LOAD(table, gpa, device, CmdWaitEvents);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CmdWriteAccelerationStructuresPropertiesKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CmdWriteBufferMarkerAMD);
    DISPATCH_LOAD(table, gpa, device, CmdWriteTimestamp);
    DISPATCH_LOAD(table, gpa, device, CompileDeferredNV);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CopyAccelerationStructureKHR);
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CopyAccelerationStructureToMemoryKHR);
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CopyMemoryToAccelerationStructureKHR);
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CreateAccelerationStructureKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CreateAccelerationStructureNV);
    DISPATCH_LOAD(table, gpa, device, CreateBuffer);
    DISPATCH_LOAD(table, gpa, device, CreateBufferView);
    DISPATCH_LOAD(table, gpa, device, CreateCommandPool);
    DISPATCH_LOAD(table, gpa, device, CreateComputePipelines);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CreateDeferredOperationKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CreateDescriptorPool);
    DISPATCH_LOAD(table, gpa, device, CreateDescriptorSetLayout);
    DISPATCH_LOAD(table, gpa, device, CreateDescriptorUpdateTemplate);
    DISPATCH_LOAD(table, gpa, device, CreateEvent);
    DISPATCH_LOAD(table, gpa, device, CreateFence);
    DISPATCH_LOAD(table, gpa, device, CreateFramebuffer);
    DISPATCH_LOAD(table, gpa, device, CreateGraphicsPipelines);
    DISPATCH_LOAD(table, gpa, device, CreateImage);
    DISPATCH_LOAD(table, gpa, device, CreateImageView);
    DISPATCH_LOAD(table, gpa, device, CreateIndirectCommandsLayoutNV);
    DISPATCH_LOAD(table, gpa, device, CreatePipelineCache);
    DISPATCH_LOAD(table, gpa, device, CreatePipelineLayout);
    DISPATCH_LOAD(table, gpa, device, CreatePrivateDataSlotEXT);
    DISPATCH_LOAD(table, gpa, device, CreateQueryPool);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, CreateRayTracingPipelinesKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, CreateRayTracingPipelinesNV);
    DISPATCH_LOAD(table, gpa, device, CreateRenderPass);
    DISPATCH_LOAD(table, gpa, device, CreateRenderPass2);
    DISPATCH_LOAD(table, gpa, device, CreateSampler);
    DISPATCH_LOAD(table, gpa, device, CreateSamplerYcbcrConversion);
    DISPATCH_LOAD(table, gpa, device, CreateSemaphore);
    DISPATCH_LOAD(table, gpa, device, CreateShaderModule);
    DISPATCH_LOAD(table, gpa, device, CreateSharedSwapchainsKHR);
    DISPATCH_LOAD(table, gpa, device, CreateSwapchainKHR);
    DISPATCH_LOAD(table, gpa, device, CreateValidationCacheEXT);
    DISPATCH_LOAD(table, gpa, device, DebugMarkerSetObjectNameEXT);
    DISPATCH_LOAD(table, gpa, device, DebugMarkerSetObjectTagEXT);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, DeferredOperationJoinKHR);
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, DestroyAccelerationStructureKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, DestroyBuffer);
    DISPATCH_LOAD(table, gpa, device, DestroyBufferView);
    DISPATCH_LOAD(table, gpa, device, DestroyCommandPool);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, DestroyDeferredOperationKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, DestroyDescriptorPool);
    DISPATCH_LOAD(table, gpa, device, DestroyDescriptorSetLayout);
    DISPATCH_LOAD(table, gpa, device, DestroyDescriptorUpdateTemplate);
    DISPATCH_LOAD(table, gpa, device, DestroyDevice);
    DISPATCH_LOAD(table, gpa, device, DestroyEvent);
    DISPATCH_LOAD(table, gpa, device, DestroyFence);
    DISPATCH_LOAD(table, gpa, device, DestroyFramebuffer);
    DISPATCH_LOAD(table, gpa, device, DestroyImage);
    DISPATCH_LOAD(table, gpa, device, DestroyImageView);
    DISPATCH_LOAD(table, gpa, device, DestroyIndirectCommandsLayoutNV);
    DISPATCH_LOAD(table, gpa, device, DestroyPipeline);
    DISPATCH_LOAD(table, gpa, device, DestroyPipelineCache);
    DISPATCH_LOAD(table, gpa, device, DestroyPipelineLayout);
    DISPATCH_LOAD(table, gpa, device, DestroyPrivateDataSlotEXT);
    DISPATCH_LOAD(table, gpa, device, DestroyQueryPool);
    DISPATCH_LOAD(table, gpa, device, DestroyRenderPass);
    DISPATCH_LOAD(table, gpa, device, DestroySampler);
    DISPATCH_LOAD(table, gpa, device, DestroySamplerYcbcrConversion);
    DISPATCH_LOAD(table, gpa, device, DestroySemaphore);
    DISPATCH_LOAD(table, gpa, device, DestroyShaderModule);
    DISPATCH_LOAD(table, gpa, device, DestroySwapchainKHR);
    DISPATCH_LOAD(table, gpa, device, DestroyValidationCacheEXT);
    DISPATCH_LOAD(table, gpa, device, DeviceWaitIdle);
    DISPATCH_LOAD(table, gpa, device, DisplayPowerControlEXT);
    DISPATCH_LOAD(table, gpa, device, EndCommandBuffer);
    DISPATCH_LOAD(table, gpa, device, FlushMappedMemoryRanges);
    DISPATCH_LOAD(table, gpa, device, FreeCommandBuffers);
    DISPATCH_LOAD(table, gpa, device, FreeDescriptorSets);
    DISPATCH_LOAD(table, gpa, device, FreeMemory);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, GetAccelerationStructureDeviceAddressKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, GetAccelerationStructureHandleNV);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, GetAccelerationStructureMemoryRequirementsKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, GetAccelerationStructureMemoryRequirementsNV);
    DISPATCH_LOAD(table, gpa, device, GetBufferDeviceAddress);
    DISPATCH_LOAD(table, gpa, device, GetBufferMemoryRequirements);
    DISPATCH_LOAD(table, gpa, device, GetBufferMemoryRequirements2);
    DISPATCH_LOAD(table, gpa, device, GetBufferOpaqueCaptureAddress);
    DISPATCH_LOAD(table, gpa, device, GetCalibratedTimestampsEXT);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, GetDeferredOperationMaxConcurrencyKHR);
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, GetDeferredOperationResultKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, GetDescriptorSetLayoutSupport);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, GetDeviceAccelerationStructureCompatibilityKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, GetDeviceGroupPeerMemoryFeatures);
    DISPATCH_LOAD(table, gpa, device, GetDeviceGroupPresentCapabilitiesKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, GetDeviceGroupSurfacePresentModes2EXT);
#endif
    DISPATCH_LOAD(table, gpa, device, GetDeviceGroupSurfacePresentModesKHR);
    DISPATCH_LOAD(table, gpa, device, GetDeviceMemoryCommitment);
    DISPATCH_LOAD(table, gpa, device, GetDeviceMemoryOpaqueCaptureAddress);
    DISPATCH_LOAD(table, gpa, device, GetDeviceQueue);
    DISPATCH_LOAD(table, gpa, device, GetDeviceQueue2);
    DISPATCH_LOAD(table, gpa, device, GetEventStatus);
    DISPATCH_LOAD(table, gpa, device, GetFenceFdKHR);
    DISPATCH_LOAD(table, gpa, device, GetFenceStatus);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, GetFenceWin32HandleKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, GetGeneratedCommandsMemoryRequirementsNV);
    DISPATCH_LOAD(table, gpa, device, GetImageDrmFormatModifierPropertiesEXT);
    DISPATCH_LOAD(table, gpa, device, GetImageMemoryRequirements);
    DISPATCH_LOAD(table, gpa, device, GetImageMemoryRequirements2);
    DISPATCH_LOAD(table, gpa, device, GetImageSparseMemoryRequirements);
    DISPATCH_LOAD(table, gpa, device, GetImageSparseMemoryRequirements2);
    DISPATCH_LOAD(table, gpa, device, GetImageSubresourceLayout);
    DISPATCH_LOAD(table, gpa, device, GetImageViewAddressNVX);
    DISPATCH_LOAD(table, gpa, device, GetImageViewHandleNVX);
    DISPATCH_LOAD(table, gpa, device, GetMemoryFdKHR);
    DISPATCH_LOAD(table, gpa, device, GetMemoryFdPropertiesKHR);
    DISPATCH_LOAD(table, gpa, device, GetMemoryHostPointerPropertiesEXT);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, GetMemoryWin32HandleKHR);
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, GetMemoryWin32HandleNV);
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, GetMemoryWin32HandlePropertiesKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, GetPastPresentationTimingGOOGLE);
    DISPATCH_LOAD(table, gpa, device, GetPerformanceParameterINTEL);
    DISPATCH_LOAD(table, gpa, device, GetPipelineCacheData);
    DISPATCH_LOAD(table, gpa, device, GetPipelineExecutableInternalRepresentationsKHR);
    DISPATCH_LOAD(table, gpa, device, GetPipelineExecutablePropertiesKHR);
    DISPATCH_LOAD(table, gpa, device, GetPipelineExecutableStatisticsKHR);
    DISPATCH_LOAD(table, gpa, device, GetPrivateDataEXT);
    DISPATCH_LOAD(table, gpa, device, GetQueryPoolResults);
    DISPATCH_LOAD(table, gpa, device, GetQueueCheckpointDataNV);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, GetRayTracingCaptureReplayShaderGroupHandlesKHR);
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, GetRayTracingShaderGroupHandlesKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, GetRefreshCycleDurationGOOGLE);
    DISPATCH_LOAD(table, gpa, device, GetRenderAreaGranularity);
    DISPATCH_LOAD(table, gpa, device, GetSemaphoreCounterValue);
    DISPATCH_LOAD(table, gpa, device, GetSemaphoreFdKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, GetSemaphoreWin32HandleKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, GetShaderInfoAMD);
    DISPATCH_LOAD(table, gpa, device, GetSwapchainCounterEXT);
    DISPATCH_LOAD(table, gpa, device, GetSwapchainImagesKHR);
    DISPATCH_LOAD(table, gpa, device, GetSwapchainStatusKHR);
    DISPATCH_LOAD(table, gpa, device, GetValidationCacheDataEXT);
    DISPATCH_LOAD(table, gpa, device, ImportFenceFdKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, ImportFenceWin32HandleKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, ImportSemaphoreFdKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, ImportSemaphoreWin32HandleKHR);
#endif
    DISPATCH_LOAD(table, gpa, device, InitializePerformanceApiINTEL);
    DISPATCH_LOAD(table, gpa, device, InvalidateMappedMemoryRanges);
    DISPATCH_LOAD(table, gpa, device, MapMemory);
    DISPATCH_LOAD(table, gpa, device, MergePipelineCaches);
    DISPATCH_LOAD(table, gpa, device, MergeValidationCachesEXT);
    DISPATCH_LOAD(table, gpa, device, QueueBeginDebugUtilsLabelEXT);
    DISPATCH_LOAD(table, gpa, device, QueueBindSparse);
    DISPATCH_LOAD(table, gpa, device, QueueEndDebugUtilsLabelEXT);
    DISPATCH_LOAD(table, gpa, device, QueueInsertDebugUtilsLabelEXT);
    DISPATCH_LOAD(table, gpa, device, QueuePresentKHR);
    DISPATCH_LOAD(table, gpa, device, QueueSetPerformanceConfigurationINTEL);
    DISPATCH_LOAD(table, gpa, device, QueueSubmit);
    DISPATCH_LOAD(table, gpa, device, QueueWaitIdle);
    DISPATCH_LOAD(table, gpa, device, RegisterDeviceEventEXT);
    DISPATCH_LOAD(table, gpa, device, RegisterDisplayEventEXT);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DISPATCH_LOAD(table, gpa, device, ReleaseFullScreenExclusiveModeEXT);
#endif
    DISPATCH_LOAD(table, gpa, device, ReleasePerformanceConfigurationINTEL);
    DISPATCH_LOAD(table, gpa, device, ReleaseProfilingLockKHR);
    DISPATCH_LOAD(table, gpa, device, ResetCommandBuffer);
    DISPATCH_LOAD(table, gpa, device, ResetCommandPool);
    DISPATCH_LOAD(table, gpa, device, ResetDescriptorPool);
    DISPATCH_LOAD(table, gpa, device, ResetEvent);
    DISPATCH_LOAD(table, gpa, device, ResetFences);
    DISPATCH_LOAD(table, gpa, device, ResetQueryPool);
    DISPATCH_LOAD(table, gpa, device, SetDebugUtilsObjectNameEXT);
    DISPATCH_LOAD(table, gpa, device, SetDebugUtilsObjectTagEXT);
    DISPATCH_LOAD(table, gpa, device, SetEvent);
    DISPATCH_LOAD(table, gpa, device, SetHdrMetadataEXT);
    DISPATCH_LOAD(table, gpa, device, SetLocalDimmingAMD);
    DISPATCH_LOAD(table, gpa, device, SetPrivateDataEXT);
    DISPATCH_LOAD(table, gpa, device, SignalSemaphore);
    DISPATCH_LOAD(table, gpa, device, TrimCommandPool);
    DISPATCH_LOAD(table, gpa, device, UninitializePerformanceApiINTEL);
    DISPATCH_LOAD(table, gpa, device, UnmapMemory);
    DISPATCH_LOAD(table, gpa, device, UpdateDescriptorSetWithTemplate);
    DISPATCH_LOAD(table, gpa, device, UpdateDescriptorSets);
    DISPATCH_LOAD(table, gpa, device, WaitForFences);
    DISPATCH_LOAD(table, gpa, device, WaitSemaphores);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    DISPATCH_LOAD(table, gpa, device, WriteAccelerationStructuresPropertiesKHR);
#endif
}
