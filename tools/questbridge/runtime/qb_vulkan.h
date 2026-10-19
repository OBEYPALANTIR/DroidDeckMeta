/*
 * qb_vulkan.h - the Vulkan half of libopenxr_questbridge.so, as seen by the runtime core.
 *
 * The core (qb_runtime.c, qb_session.c, qb_swapchain.c, qb_input.c) owns every OpenXR handle and the
 * socket; this module owns everything that touches the app's Vulkan device: XR_KHR_vulkan_enable and
 * XR_KHR_vulkan_enable2, importing the service's gralloc dma-bufs as VkImages, the guest-owned images
 * format the service does not allocate, depth), the blit/resolve into the imported image on release
 * (or the direct image's ownership handoff), and the sync_file fence that tells the service the frame
 * is rendered.
 *
 * Every function is called from the app's thread with the core's session lock held; nothing here
 * blocks on the socket.
 */
#ifndef QB_VULKAN_H
#define QB_VULKAN_H

#define XR_USE_GRAPHICS_API_VULKAN 1
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "questbridge_protocol.h"

struct qb_vk;               /* per-session Vulkan state, opaque to the core */
struct qb_vk_swapchain;     /* per-swapchain image state, opaque to the core */

/* ---- XR_KHR_vulkan_enable (the app creates the instance and device itself) */

/* Space-separated extension names the app must enable; the usual two-call size idiom on `cap`/`count`
 * (count includes the terminating NUL). Instance: VK_KHR_get_physical_device_properties2,
 * VK_KHR_external_memory_capabilities, VK_KHR_external_fence_capabilities,
 * VK_KHR_external_semaphore_capabilities. Device: VK_KHR_external_memory, VK_KHR_external_memory_fd,
 * VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier, VK_KHR_external_fence,
 * VK_KHR_external_fence_fd, VK_KHR_external_semaphore, VK_KHR_external_semaphore_fd,
 * VK_KHR_dedicated_allocation, VK_KHR_image_format_list. */
XrResult qb_vk_instance_extensions(uint32_t cap, uint32_t *count, char *buf);
XrResult qb_vk_device_extensions(uint32_t cap, uint32_t *count, char *buf);
/* The physical device the app must use: the first one that offers every device extension above,
 * preferring a non-CPU type (Turnip over llvmpipe). `gipa` is libvulkan's vkGetInstanceProcAddr
 * (qb_vk_loader_gipa) when the app did not hand one over. */
XrResult qb_vk_physical_device(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice *out);
/* minApiVersionSupported = 1.1, maxApiVersionSupported = 1.3. */
XrResult qb_vk_requirements(XrGraphicsRequirementsVulkanKHR *out);
/* vkGetInstanceProcAddr from the system libvulkan.so.1 (dlopen once; NULL when absent). */
PFN_vkGetInstanceProcAddr qb_vk_loader_gipa(void);

/* ---- XR_KHR_vulkan_enable2 (this module creates them with the extensions above appended) */
XrResult qb_vk_create_instance(const XrVulkanInstanceCreateInfoKHR *info, VkInstance *instance, VkResult *vk_result);
XrResult qb_vk_create_device(const XrVulkanDeviceCreateInfoKHR *info, VkDevice *device, VkResult *vk_result);
XrResult qb_vk_physical_device2(const XrVulkanGraphicsDeviceGetInfoKHR *info, VkPhysicalDevice *out);

/* ---- session binding */

/* Validate the app's binding (its physical device must be the one qb_vk_physical_device would pick,
 * the queue family must support graphics) and load the device entry points through `gipa` (the one
 * remembered from qb_vk_create_instance, else qb_vk_loader_gipa). Creates the command pool and the
 * reusable fence/semaphore objects. XR_ERROR_GRAPHICS_DEVICE_INVALID on mismatch. */
XrResult qb_vk_bind(const XrGraphicsBindingVulkanKHR *binding, struct qb_vk **out);
/* Waits for the device to go idle and frees everything; every swapchain must be gone already. */
void qb_vk_unbind(struct qb_vk *vk);

/* ---- swapchains */

/* Import the images the service allocated for `ci` (fds[i] = image i's dma-buf, owned by this call
 * from now on, closed on destroy or failure). `ci` is the app's request, unmodified; `info` describes
 * what the service actually allocated, which differs in two agreed ways: the service image is
 * (ci->width * ci->arraySize) x ci->height (array layers are flattened side by side; the core sends
 * layer L as sub_image.x = L * ci->width), and for the BGRA pair the service allocated the RGBA
 * counterpart (B8G8R8A8_SRGB -> R8G8B8A8_SRGB, UNORM likewise), so the imported VkImage is created in
 * that RGBA format. If ci asks for arraySize > 1, sampleCount > 1, mipCount > 1, or a format other
 * than the imported image's, the app gets a guest-owned image of exactly what it asked for and
 * qb_vk_swapchain_release blits (vkCmdBlitImage, which also converts formats; layer L lands at x
 * offset L * ci->width) or resolves it into imported image `index`. The imported images are created
 * with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT + VkImageDrmFormatModifierExplicitCreateInfoEXT
 * {info->modifier, one plane: offset 0, rowPitch info->stride_bytes}, memory imported with
 * VkImportMemoryFdInfoKHR (DMA_BUF) as a dedicated allocation of info->size_bytes, usage
 * TRANSFER_SRC | TRANSFER_DST | COLOR_ATTACHMENT | SAMPLED. When vkCreateImage refuses the modifier
 * (XR_ERROR_RUNTIME_FAILURE is NOT returned): fall back to the guest-owned path with a linear
 * imported image only if info->modifier is linear; a refused UBWC import is a hard failure
 * (XR_ERROR_RUNTIME_FAILURE) because the service's buffer cannot be read any other way. The service
 * handoff crosses VK_EXT_queue_family_foreign, so a device without it cannot back a service
 * swapchain at all (XR_ERROR_FEATURE_UNSUPPORTED); there is no IGNORED fallback. */
XrResult qb_vk_swapchain_import(struct qb_vk *vk, const XrSwapchainCreateInfo *ci,
                                const struct qb_swapchain_info *info, const int *fds, uint32_t fd_count,
                                struct qb_vk_swapchain **out);
/* A swapchain that never reaches the service (depth/stencil formats, or anything the core decides to
 * keep local): `image_count` guest-owned images of exactly `ci`; release is a no-op. */
XrResult qb_vk_swapchain_create_local(struct qb_vk *vk, const XrSwapchainCreateInfo *ci, uint32_t image_count,
                                      struct qb_vk_swapchain **out);
/* Idles the queue first; the fds it took over are closed. */
void qb_vk_swapchain_destroy(struct qb_vk *vk, struct qb_vk_swapchain *sc);
uint32_t qb_vk_swapchain_image_count(const struct qb_vk_swapchain *sc);
/* The VkImage the app renders into (xrEnumerateSwapchainImages): guest-owned or imported. */
VkImage qb_vk_swapchain_app_image(const struct qb_vk_swapchain *sc, uint32_t index);
/* 1 when the app image differs from the imported one (a blit/resolve happens on release). */
int qb_vk_swapchain_is_indirect(const struct qb_vk_swapchain *sc);
/* xrAcquireSwapchainImage, once the core picked `index`: when direct, submit the handoff that leaves
 * imported image `index` in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL for the app (from UNDEFINED on
 * first acquire, from GENERAL/FOREIGN once the service has had it) on the bound queue, so queue order
 * puts it ahead of the app's rendering. Indirect and local swapchains: nothing to do (the app image
 * is guest-owned). XR_ERROR_CALL_ORDER_INVALID when the image is already with the app. */
XrResult qb_vk_swapchain_acquire(struct qb_vk *vk, struct qb_vk_swapchain *sc, uint32_t index);
/* xrReleaseSwapchainImage: when indirect, submit the blit/resolve from the app image into imported
 * image `index`; when direct, submit the transition of imported image `index` back to GENERAL/FOREIGN
 * for the service. Both run on the bound queue (the app's work is already queued, so queue order
 * covers it). Local swapchains: nothing to do. The core sends the frame's fence afterwards.
 * XR_ERROR_CALL_ORDER_INVALID when a direct image was not acquired. */
XrResult qb_vk_swapchain_release(struct qb_vk *vk, struct qb_vk_swapchain *sc, uint32_t index);
/* The formats this module can give the app directly or indirectly, appended to the service's list by
 * the core: the depth formats (D32_SFLOAT, D24_UNORM_S8_UINT, D16_UNORM, D32_SFLOAT_S8_UINT) and the
 * BGRA pair (B8G8R8A8_SRGB/UNORM, reached by blit). `cap`/`count` idiom; returns the count. */
uint32_t qb_vk_extra_formats(uint32_t cap, int64_t *out);
/* 1 when `format` is one the core must keep local (never sent to the service). */
int qb_vk_format_is_local(int64_t format);

/* ---- frame completion */

/* Called from xrEndFrame before the END_FRAME request: submits an empty batch after everything the app
 * (and this module's transitions/blits) queued, signalling a fence exportable as a sync_file. On
 * XR_SUCCESS writes the sync_file fd to `*fd` (the caller sends and closes it); `*fd` is -1 only when
 * the fence was successfully waited on the CPU instead (no exportable fence, an already-signalled one,
 * or a failed export that fell back to a real wait): the service may then read the images at once. Any
 * reset/submit/wait failure returns its error with `*fd` at -1: nothing readable may be sent. */
XrResult qb_vk_end_frame_fence(struct qb_vk *vk, int *fd);

#endif
